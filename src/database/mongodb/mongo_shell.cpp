#include "database/mongodb/mongo_shell.hpp"

#include <cctype>
#include <regex>
#include <string_view>

namespace {
    std::string trim(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.remove_prefix(1);
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    std::string stripLineComments(std::string s) {
        std::string out;
        out.reserve(s.size());
        bool inSingle = false;
        bool inDouble = false;
        for (size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (!inSingle && !inDouble && c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') {
                    ++i;
                }
                if (i < s.size()) {
                    out.push_back('\n');
                }
                continue;
            }
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
            } else if (c == '"' && !inSingle) {
                inDouble = !inDouble;
            }
            out.push_back(c);
        }
        return out;
    }

    bool iequals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    std::string readIdentifier(std::string_view s, size_t& i) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        size_t start = i;
        while (i < s.size()) {
            const char c = s[i];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
                ++i;
            } else {
                break;
            }
        }
        return std::string(s.substr(start, i - start));
    }

    std::optional<std::string> extractParenContent(std::string_view s, size_t& i) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        if (i >= s.size() || s[i] != '(') {
            return std::nullopt;
        }
        ++i;
        const size_t start = i;
        int depth = 1;
        while (i < s.size() && depth > 0) {
            const char c = s[i];
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
                if (depth == 0) {
                    const std::string inner = std::string(s.substr(start, i - start));
                    ++i;
                    return trim(inner);
                }
            }
            ++i;
        }
        return std::nullopt;
    }

    std::vector<std::string> splitTopLevelArgs(std::string_view inner) {
        std::vector<std::string> args;
        if (trim(inner).empty()) {
            return args;
        }

        size_t start = 0;
        int depth = 0;
        bool inSingle = false;
        bool inDouble = false;
        for (size_t i = 0; i <= inner.size(); ++i) {
            const bool atEnd = i == inner.size();
            if (!atEnd) {
                const char c = inner[i];
                if (c == '\'' && !inDouble) {
                    inSingle = !inSingle;
                } else if (c == '"' && !inSingle) {
                    inDouble = !inDouble;
                } else if (!inSingle && !inDouble) {
                    if (c == '(' || c == '{' || c == '[') {
                        ++depth;
                    } else if (c == ')' || c == '}' || c == ']') {
                        --depth;
                    }
                }
            }

            if (atEnd || (!inSingle && !inDouble && depth == 0 && inner[i] == ',')) {
                const std::string part = trim(inner.substr(start, i - start));
                if (!part.empty()) {
                    args.push_back(part);
                }
                start = i + 1;
            }
        }
        return args;
    }

    bool parseTrailingChain(std::string& query, int& limit, int& skip) {
        bool changed = false;
        while (true) {
            const std::string trimmed = trim(query);
            if (trimmed.size() < 8) {
                break;
            }

            size_t dotPos = trimmed.rfind('.');
            if (dotPos == std::string::npos || dotPos + 1 >= trimmed.size()) {
                break;
            }

            const std::string tail = trimmed.substr(dotPos + 1);
            std::string_view tailView = tail;
            size_t idx = 0;
            const std::string name = readIdentifier(tailView, idx);
            if (name.empty()) {
                break;
            }

            int* target = nullptr;
            if (iequals(name, "limit")) {
                target = &limit;
            } else if (iequals(name, "skip")) {
                target = &skip;
            } else {
                break;
            }

            auto arg = extractParenContent(tailView, idx);
            if (!arg || idx < tailView.size()) {
                break;
            }

            try {
                *target = std::stoi(trim(*arg));
            } catch (...) {
                return false;
            }

            query = trim(trimmed.substr(0, dotPos));
            changed = true;
        }
        return changed;
    }
} // namespace

std::string shellLiteralToExtendedJson(std::string_view literal) {
    std::string s = trim(literal);
    if (s.empty()) {
        return "{}";
    }

    {
        static const std::regex objectIdRe(
            R"re((\bObjectId\s*\(\s*)"([0-9a-fA-F]{24})"(\s*\)))re",
            std::regex::icase);
        s = std::regex_replace(s, objectIdRe, R"({"$oid": "$2"})");
    }
    {
        static const std::regex objectIdSingleRe(
            R"re((\bObjectId\s*\(\s*)'([0-9a-fA-F]{24})'(\s*\)))re",
            std::regex::icase);
        s = std::regex_replace(s, objectIdSingleRe, R"({"$oid": "$2"})");
    }
    {
        static const std::regex isoDateRe(
            R"re((\bISODate\s*\(\s*)"([^"]*)"(\s*\)))re", std::regex::icase);
        s = std::regex_replace(s, isoDateRe, R"({"$date": "$2"})");
    }
    {
        static const std::regex isoDateSingleRe(
            R"re((\bISODate\s*\(\s*)'([^']*)'(\s*\)))re", std::regex::icase);
        s = std::regex_replace(s, isoDateSingleRe, R"({"$date": "$2"})");
    }

    {
        static const std::regex singleQuoted(R"('([^'\\]*(?:\\.[^'\\]*)*)')");
        s = std::regex_replace(s, singleQuoted, R"("$1")");
    }

    {
        static const std::regex unquotedKeys(R"(([{,]\s*)([A-Za-z_$][\w$]*)\s*:)");
        s = std::regex_replace(s, unquotedKeys, R"($1"$2":)");
    }

    return s;
}

std::optional<MongoShellCommand> tryParseMongoShell(const std::string& query) {
    std::string work = stripLineComments(query);
    work = trim(work);
    if (work.empty() || work.front() == '{') {
        return std::nullopt;
    }

    if (!iequals(work.substr(0, std::min(work.size(), size_t{3})), "db.") &&
        work.rfind("db.", 0) != 0) {
        return std::nullopt;
    }

    MongoShellCommand cmd;
    parseTrailingChain(work, cmd.limit, cmd.skip);

    size_t pos = 3;
    const std::string first = readIdentifier(work, pos);
    if (first.empty()) {
        return std::nullopt;
    }

    while (pos < work.size() && std::isspace(static_cast<unsigned char>(work[pos]))) {
        ++pos;
    }
    if (pos >= work.size()) {
        return std::nullopt;
    }

    if (work[pos] == '(') {
        cmd.method = first;
        auto argsInner = extractParenContent(work, pos);
        if (!argsInner) {
            return std::nullopt;
        }
        cmd.args = splitTopLevelArgs(*argsInner);
        while (pos < work.size() && std::isspace(static_cast<unsigned char>(work[pos]))) {
            ++pos;
        }
        if (pos != work.size()) {
            return std::nullopt;
        }
        return cmd;
    }

    if (work[pos] != '.') {
        return std::nullopt;
    }
    ++pos;
    cmd.collection = first;

    const std::string method = readIdentifier(work, pos);
    if (method.empty()) {
        return std::nullopt;
    }
    cmd.method = method;

    auto argsInner = extractParenContent(work, pos);
    if (!argsInner) {
        return std::nullopt;
    }
    cmd.args = splitTopLevelArgs(*argsInner);

    while (pos < work.size() && std::isspace(static_cast<unsigned char>(work[pos]))) {
        ++pos;
    }
    if (pos != work.size()) {
        return std::nullopt;
    }

    return cmd;
}
