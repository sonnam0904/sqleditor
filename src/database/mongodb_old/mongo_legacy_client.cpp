#include "database/mongodb_old/mongo_legacy_client.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <spdlog/spdlog.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

std::filesystem::path parentDirectory(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty()) {
        return std::filesystem::current_path();
    }
    return parent;
}

} // namespace

MongoLegacyClient::~MongoLegacyClient() {
    shutdown();
}

std::filesystem::path MongoLegacyClient::bridgeExecutablePath() {
    std::filesystem::path exeDir;

#if defined(__linux__)
    std::array<char, 4096> path{};
    const ssize_t len = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (len > 0) {
        path[static_cast<size_t>(len)] = '\0';
        exeDir = parentDirectory(std::filesystem::path(path.data()));
    }
#elif defined(__APPLE__)
    std::array<char, 4096> path{};
    uint32_t size = static_cast<uint32_t>(path.size());
    if (_NSGetExecutablePath(path.data(), &size) == 0) {
        exeDir = parentDirectory(std::filesystem::path(path.data()));
    }
#elif defined(_WIN32)
    std::array<char, MAX_PATH> path{};
    const DWORD len = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (len > 0) {
        exeDir = parentDirectory(std::filesystem::path(std::string(path.data(), len)));
    }
#endif

    if (exeDir.empty()) {
        exeDir = std::filesystem::current_path();
    }

#if defined(_WIN32)
    return exeDir / "sqleditor-mongo3-bridge.exe";
#else
    return exeDir / "sqleditor-mongo3-bridge";
#endif
}

bool MongoLegacyClient::startBridgeProcess(std::string& error) {
    if (bridgeRunning_) {
        return true;
    }

    const auto bridgePath = bridgeExecutablePath();
    if (!std::filesystem::exists(bridgePath)) {
        error = std::format("MongoDB Old bridge not found: {}", bridgePath.string());
        return false;
    }

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;

    if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0) ||
        !CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
        error = "Failed to create bridge pipes";
        return false;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = stdoutWrite;

    PROCESS_INFORMATION pi{};
    std::string cmd = bridgePath.string();
    const auto exeDir = bridgePath.parent_path().string();
    const BOOL created = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, exeDir.c_str(), &si, &pi);
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);

    if (!created) {
        CloseHandle(stdinWrite);
        CloseHandle(stdoutRead);
        error = "Failed to start MongoDB Old bridge process";
        return false;
    }

    CloseHandle(pi.hThread);
    childProcess_ = pi.hProcess;
    stdinWrite_ = stdinWrite;
    stdoutRead_ = stdoutRead;
#else
    int inPipe[2]{-1, -1};
    int outPipe[2]{-1, -1};
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0) {
        error = "Failed to create bridge pipes";
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        error = "Failed to fork bridge process";
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);
        return false;
    }

    if (pid == 0) {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);

        const std::string path = bridgePath.string();
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(inPipe[0]);
    close(outPipe[1]);
    childPid_ = pid;
    stdinWrite_ = inPipe[1];
    stdoutRead_ = outPipe[0];
#endif

    bridgeRunning_ = true;
    spdlog::debug("Started MongoDB Old bridge: {}", bridgePath.string());
    return true;
}

void MongoLegacyClient::stopBridgeProcess() {
    if (!bridgeRunning_) {
        return;
    }

#if defined(_WIN32)
    if (stdinWrite_) {
        CloseHandle(static_cast<HANDLE>(stdinWrite_));
        stdinWrite_ = nullptr;
    }
    if (stdoutRead_) {
        CloseHandle(static_cast<HANDLE>(stdoutRead_));
        stdoutRead_ = nullptr;
    }
    if (childProcess_) {
        TerminateProcess(static_cast<HANDLE>(childProcess_), 0);
        WaitForSingleObject(static_cast<HANDLE>(childProcess_), 5000);
        CloseHandle(static_cast<HANDLE>(childProcess_));
        childProcess_ = nullptr;
    }
#else
    if (stdinWrite_ >= 0) {
        close(stdinWrite_);
        stdinWrite_ = -1;
    }
    if (stdoutRead_ >= 0) {
        close(stdoutRead_);
        stdoutRead_ = -1;
    }
    if (childPid_ > 0) {
        kill(childPid_, SIGTERM);
        int status = 0;
        waitpid(childPid_, &status, 0);
        childPid_ = -1;
    }
#endif

    bridgeRunning_ = false;
}

bool MongoLegacyClient::ensureStarted(std::string& error) {
    std::lock_guard lock(mutex_);
    return startBridgeProcess(error);
}

void MongoLegacyClient::shutdown() {
    std::lock_guard lock(mutex_);
    if (bridgeRunning_) {
        std::string error;
        nlohmann::json req;
        req["id"] = nextId_++;
        req["op"] = "disconnect";
        sendRequest(req.dump(), error);
    }
    stopBridgeProcess();
}

bool MongoLegacyClient::writeLine(const std::string& line, std::string& error) {
#if defined(_WIN32)
    const HANDLE handle = static_cast<HANDLE>(stdinWrite_);
    std::string payload = line;
    payload.push_back('\n');
    DWORD written = 0;
    if (!WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) ||
        written != payload.size()) {
        error = "Failed to write to MongoDB Old bridge";
        return false;
    }
#else
    std::string payload = line;
    payload.push_back('\n');
    ssize_t offset = 0;
    while (offset < static_cast<ssize_t>(payload.size())) {
        const ssize_t n = write(stdinWrite_, payload.data() + offset, payload.size() - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "Failed to write to MongoDB Old bridge";
            return false;
        }
        offset += n;
    }
#endif
    return true;
}

bool MongoLegacyClient::readLine(std::string& line, std::string& error) {
    line.clear();
    char ch = '\0';

    while (true) {
#if defined(_WIN32)
        DWORD read = 0;
        if (!ReadFile(static_cast<HANDLE>(stdoutRead_), &ch, 1, &read, nullptr) || read == 0) {
            error = "MongoDB Old bridge closed stdout";
            return false;
        }
#else
        const ssize_t n = read(stdoutRead_, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "Failed to read from MongoDB Old bridge";
            return false;
        }
        if (n == 0) {
            error = "MongoDB Old bridge closed stdout";
            return false;
        }
#endif
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line.push_back(ch);
        }
    }
    return true;
}

MongoLegacyClient::BridgeResponse MongoLegacyClient::sendRequest(const std::string& requestJson,
                                                                 std::string& error) {
    BridgeResponse response;
    if (!bridgeRunning_) {
        error = "MongoDB Old bridge is not running";
        return response;
    }

    if (!writeLine(requestJson, error)) {
        stopBridgeProcess();
        return response;
    }

    std::string line;
    if (!readLine(line, error)) {
        stopBridgeProcess();
        return response;
    }

    try {
        const auto json = nlohmann::json::parse(line);
        response.rawJson = line;
        response.ok = json.value("ok", false);
        if (!response.ok && json.contains("error") && json["error"].is_string()) {
            response.error = json["error"].get<std::string>();
        }
    } catch (const std::exception& e) {
        error = std::format("Invalid bridge response: {}", e.what());
        stopBridgeProcess();
    }

    return response;
}

bool MongoLegacyClient::connect(const std::string& uri, std::string& error) {
    std::lock_guard lock(mutex_);
    if (!startBridgeProcess(error)) {
        return false;
    }

    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "connect";
    req["uri"] = uri;

    const auto resp = sendRequest(req.dump(), error);
    if (!error.empty()) {
        return false;
    }
    if (!resp.ok) {
        error = resp.error.empty() ? "MongoDB Old connect failed" : resp.error;
        return false;
    }
    return true;
}

void MongoLegacyClient::disconnect() {
    std::lock_guard lock(mutex_);
    if (!bridgeRunning_) {
        return;
    }
    std::string error;
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "disconnect";
    sendRequest(req.dump(), error);
}

std::string MongoLegacyClient::buildInfo(const std::string& db, std::string& error) {
    std::lock_guard lock(mutex_);
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "build_info";
    req["db"] = db;

    const auto resp = sendRequest(req.dump(), error);
    if (!error.empty() || !resp.ok) {
        if (error.empty()) {
            error = resp.error;
        }
        return {};
    }

    try {
        const auto json = nlohmann::json::parse(resp.rawJson);
        if (json.contains("version") && json["version"].is_string()) {
            return json["version"].get<std::string>();
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return {};
}

std::vector<std::string> MongoLegacyClient::listDatabases(std::string& error) {
    std::lock_guard lock(mutex_);
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "list_databases";

    const auto resp = sendRequest(req.dump(), error);
    if (!error.empty() || !resp.ok) {
        if (error.empty()) {
            error = resp.error;
        }
        return {};
    }

    try {
        const auto json = nlohmann::json::parse(resp.rawJson);
        if (json.contains("databases") && json["databases"].is_array()) {
            std::vector<std::string> names;
            for (const auto& item : json["databases"]) {
                if (item.is_string()) {
                    names.push_back(item.get<std::string>());
                }
            }
            return names;
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return {};
}

std::vector<std::string> MongoLegacyClient::listCollections(const std::string& db,
                                                             std::string& error) {
    std::lock_guard lock(mutex_);
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "list_collections";
    req["db"] = db;

    const auto resp = sendRequest(req.dump(), error);
    if (!error.empty() || !resp.ok) {
        if (error.empty()) {
            error = resp.error;
        }
        return {};
    }

    try {
        const auto json = nlohmann::json::parse(resp.rawJson);
        if (json.contains("collections") && json["collections"].is_array()) {
            std::vector<std::string> names;
            for (const auto& item : json["collections"]) {
                if (item.is_string()) {
                    names.push_back(item.get<std::string>());
                }
            }
            return names;
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return {};
}

std::vector<MongoLegacyClient::SchemaField>
MongoLegacyClient::sampleSchema(const std::string& db, const std::string& collection, int limit,
                                std::string& error) {
    std::lock_guard lock(mutex_);
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "sample_schema";
    req["db"] = db;
    req["collection"] = collection;
    req["limit"] = limit;

    const auto resp = sendRequest(req.dump(), error);
    if (!error.empty() || !resp.ok) {
        if (error.empty()) {
            error = resp.error;
        }
        return {};
    }

    std::vector<SchemaField> fields;
    try {
        const auto json = nlohmann::json::parse(resp.rawJson);
        if (json.contains("fields") && json["fields"].is_array()) {
            for (const auto& item : json["fields"]) {
                SchemaField field;
                field.name = item.value("name", "");
                field.type = item.value("type", "unknown");
                if (!field.name.empty()) {
                    fields.push_back(std::move(field));
                }
            }
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return fields;
}

MongoLegacyClient::FindResult MongoLegacyClient::find(const std::string& db,
                                                      const std::string& collection,
                                                      const std::string& filterJson, int limit,
                                                      int skip, const std::string& sortJson,
                                                      std::string& error) {
    std::lock_guard lock(mutex_);
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "find";
    req["db"] = db;
    req["collection"] = collection;
    req["filter"] = filterJson;
    req["limit"] = limit;
    req["skip"] = skip;
    if (!sortJson.empty()) {
        req["sort"] = sortJson;
    }

    const auto resp = sendRequest(req.dump(), error);
    FindResult result;
    if (!error.empty() || !resp.ok) {
        if (error.empty()) {
            error = resp.error;
        }
        return result;
    }

    try {
        const auto json = nlohmann::json::parse(resp.rawJson);
        if (json.contains("documents") && json["documents"].is_array()) {
            for (const auto& item : json["documents"]) {
                if (item.is_string()) {
                    result.documentsJson.push_back(item.get<std::string>());
                }
            }
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return result;
}

int64_t MongoLegacyClient::count(const std::string& db, const std::string& collection,
                                 const std::string& filterJson, std::string& error) {
    std::lock_guard lock(mutex_);
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "count";
    req["db"] = db;
    req["collection"] = collection;
    req["filter"] = filterJson;

    const auto resp = sendRequest(req.dump(), error);
    if (!error.empty() || !resp.ok) {
        if (error.empty()) {
            error = resp.error;
        }
        return -1;
    }

    try {
        const auto json = nlohmann::json::parse(resp.rawJson);
        if (json.contains("count") && json["count"].is_number_integer()) {
            return json["count"].get<int64_t>();
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return -1;
}

int64_t MongoLegacyClient::estimatedDocumentCount(const std::string& db,
                                                  const std::string& collection,
                                                  std::string& error) {
    std::lock_guard lock(mutex_);
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "estimated_count";
    req["db"] = db;
    req["collection"] = collection;

    const auto resp = sendRequest(req.dump(), error);
    if (!error.empty() || !resp.ok) {
        if (error.empty()) {
            error = resp.error;
        }
        return -1;
    }

    try {
        const auto json = nlohmann::json::parse(resp.rawJson);
        if (json.contains("count") && json["count"].is_number_integer()) {
            return json["count"].get<int64_t>();
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return -1;
}

MongoLegacyClient::FindResult MongoLegacyClient::aggregate(const std::string& db,
                                                           const std::string& collection,
                                                           const std::string& pipelineJson,
                                                           int limit, std::string& error) {
    std::lock_guard lock(mutex_);
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "aggregate";
    req["db"] = db;
    req["collection"] = collection;
    req["pipeline"] = pipelineJson;
    req["limit"] = limit;

    const auto resp = sendRequest(req.dump(), error);
    FindResult result;
    if (!error.empty() || !resp.ok) {
        if (error.empty()) {
            error = resp.error;
        }
        return result;
    }

    try {
        const auto json = nlohmann::json::parse(resp.rawJson);
        if (json.contains("documents") && json["documents"].is_array()) {
            for (const auto& item : json["documents"]) {
                if (item.is_string()) {
                    result.documentsJson.push_back(item.get<std::string>());
                }
            }
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return result;
}

std::string MongoLegacyClient::runCommandJson(const std::string& db,
                                              const std::string& commandJson, std::string& error) {
    std::lock_guard lock(mutex_);
    nlohmann::json req;
    req["id"] = nextId_++;
    req["op"] = "run_command";
    req["db"] = db;
    req["command"] = commandJson;

    const auto resp = sendRequest(req.dump(), error);
    if (!error.empty() || !resp.ok) {
        if (error.empty()) {
            error = resp.error;
        }
        return {};
    }

    try {
        const auto json = nlohmann::json::parse(resp.rawJson);
        if (json.contains("result") && json["result"].is_string()) {
            return json["result"].get<std::string>();
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return {};
}
