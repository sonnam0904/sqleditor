#include "utils/process_runner.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
#if defined(_WIN32)
    std::string quoteWindowsArg(const std::string& arg) {
        if (arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string::npos) {
            std::string quoted = "\"";
            size_t backslashes = 0;
            for (const char c : arg) {
                if (c == '\\') {
                    ++backslashes;
                } else if (c == '"') {
                    quoted.append(backslashes * 2 + 1, '\\');
                    quoted += c;
                    backslashes = 0;
                } else {
                    quoted.append(backslashes, '\\');
                    backslashes = 0;
                    quoted += c;
                }
            }
            quoted.append(backslashes * 2, '\\');
            quoted += '"';
            return quoted;
        }
        return arg;
    }

    std::string buildCommandLine(const std::vector<std::string>& args) {
        std::string commandLine;
        for (const auto& arg : args) {
            if (!commandLine.empty())
                commandLine += ' ';
            commandLine += quoteWindowsArg(arg);
        }
        return commandLine;
    }

    std::vector<char>
    buildEnvironmentBlock(const std::unordered_map<std::string, std::string>& overrides) {
        std::vector<std::string> entries;
        LPCH env = GetEnvironmentStringsA();
        if (env) {
            for (LPCH current = env; *current != '\0'; current += std::strlen(current) + 1) {
                std::string entry = current;
                const auto eq = entry.find('=');
                const std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
                if (!overrides.contains(key)) {
                    entries.push_back(std::move(entry));
                }
            }
            FreeEnvironmentStringsA(env);
        }

        for (const auto& [key, value] : overrides) {
            entries.push_back(key + "=" + value);
        }
        std::ranges::sort(entries);

        std::vector<char> block;
        for (const auto& entry : entries) {
            block.insert(block.end(), entry.begin(), entry.end());
            block.push_back('\0');
        }
        block.push_back('\0');
        return block;
    }
#endif
} // namespace

ProcessResult ProcessRunner::run(const ProcessSpec& spec) {
    ProcessResult result;
    if (spec.args.empty() || spec.args.front().empty()) {
        result.errorMessage = "No executable specified";
        return result;
    }

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        result.errorMessage = "Failed to create process pipe";
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string commandLine = buildCommandLine(spec.args);
    auto environmentBlock = buildEnvironmentBlock(spec.environment);

    const BOOL created =
        CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       environmentBlock.data(), nullptr, &si, &pi);
    CloseHandle(writePipe);

    if (!created) {
        CloseHandle(readPipe);
        result.errorMessage = std::format("Failed to start '{}'", spec.args.front());
        return result;
    }

    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        result.output.append(buffer, bytesRead);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    result.exitCode = static_cast<int>(exitCode);
    result.success = exitCode == 0;
    if (!result.success && result.output.empty()) {
        result.errorMessage =
            std::format("'{}' exited with code {}", spec.args.front(), result.exitCode);
    }
    return result;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        result.errorMessage =
            std::format("Failed to create process pipe: {}", std::strerror(errno));
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        result.errorMessage = std::format("Failed to fork process: {}", std::strerror(errno));
        return result;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        for (const auto& [key, value] : spec.environment) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.reserve(spec.args.size() + 1);
        for (const auto& arg : spec.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv.front(), argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    close(pipefd[1]);
    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        result.output.append(buffer, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
        result.success = result.exitCode == 0;
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
        result.success = false;
    }

    if (!result.success && result.output.empty()) {
        result.errorMessage =
            result.exitCode == 127
                ? std::format("'{}' was not found in PATH", spec.args.front())
                : std::format("'{}' exited with code {}", spec.args.front(), result.exitCode);
    }
    return result;
#endif
}
