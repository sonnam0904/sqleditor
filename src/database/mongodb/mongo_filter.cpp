#include "database/mongodb/mongo_filter.hpp"

#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <spdlog/spdlog.h>
#include <sstream>
#include <vector>

namespace {
    using bsoncxx::builder::stream::document;
    using bsoncxx::builder::stream::finalize;
    using bsoncxx::builder::stream::open_array;
    using bsoncxx::builder::stream::close_array;
    using bsoncxx::builder::stream::open_document;
    using bsoncxx::builder::stream::close_document;

    std::string trim(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.remove_prefix(1);
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    std::string stripOuterParens(std::string s) {
        s = trim(s);
        while (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
            int depth = 0;
            bool wraps = true;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '(') {
                    ++depth;
                } else if (s[i] == ')') {
                    --depth;
                    if (depth == 0 && i + 1 < s.size()) {
                        wraps = false;
                        break;
                    }
                }
            }
            if (!wraps) {
                break;
            }
            s = trim(s.substr(1, s.size() - 2));
        }
        return s;
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

    void skipWs(std::string_view s, size_t& i) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
    }

    std::string readIdentifier(std::string_view s, size_t& i) {
        skipWs(s, i);
        if (i >= s.size()) {
            return {};
        }
        if (s[i] == '"' || s[i] == '`') {
            const char quote = s[i++];
            std::string out;
            while (i < s.size() && s[i] != quote) {
                out.push_back(s[i++]);
            }
            if (i < s.size() && s[i] == quote) {
                ++i;
            }
            return out;
        }
        size_t start = i;
        while (i < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '.')) {
            ++i;
        }
        return std::string(s.substr(start, i - start));
    }

    std::string readQuotedString(std::string_view s, size_t& i, char quote) {
        std::string out;
        while (i < s.size()) {
            if (s[i] == quote) {
                if (i + 1 < s.size() && s[i + 1] == quote) {
                    out.push_back(quote);
                    i += 2;
                    continue;
                }
                ++i;
                break;
            }
            out.push_back(s[i++]);
        }
        return out;
    }

    bool isObjectIdHex(std::string_view s) {
        if (s.size() != 24) {
            return false;
        }
        for (char c : s) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        return true;
    }

    std::time_t utcTimeFromTm(std::tm tm) {
#ifdef _WIN32
        return _mkgmtime(&tm);
#else
        return timegm(&tm);
#endif
    }

    std::optional<std::chrono::milliseconds> parseMongoShellDateString(std::string raw) {
        raw = trim(raw);
        if (raw.empty()) {
            return std::nullopt;
        }

        std::replace(raw.begin(), raw.end(), ' ', 'T');
        if (!raw.empty() && raw.back() == 'Z') {
            raw.pop_back();
        }

        int millis = 0;
        if (const auto dot = raw.find('.'); dot != std::string::npos) {
            const std::string frac = raw.substr(dot + 1);
            raw.resize(dot);
            if (!frac.empty()) {
                try {
                    std::string padded = frac;
                    while (padded.size() < 3) {
                        padded.push_back('0');
                    }
                    if (padded.size() > 3) {
                        padded.resize(3);
                    }
                    millis = std::stoi(padded);
                } catch (...) {
                    return std::nullopt;
                }
            }
        }

        std::tm tm{};
        std::istringstream ss(raw);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        if (ss.fail()) {
            return std::nullopt;
        }

        const auto seconds = utcTimeFromTm(tm);
        if (seconds < 0) {
            return std::nullopt;
        }
        return std::chrono::milliseconds{seconds * 1000LL + millis};
    }

    std::optional<std::string> readAlphaToken(std::string_view s, size_t& i) {
        skipWs(s, i);
        const size_t start = i;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        if (start == i) {
            return std::nullopt;
        }
        return std::string(s.substr(start, i - start));
    }

    bool readParenQuotedArg(std::string_view s, size_t& i, std::string& out) {
        skipWs(s, i);
        if (i >= s.size() || s[i] != '(') {
            return false;
        }
        ++i;
        skipWs(s, i);
        if (i >= s.size()) {
            return false;
        }
        const char quote = s[i];
        if (quote != '"' && quote != '\'') {
            return false;
        }
        ++i;
        out = readQuotedString(s, i, quote);
        skipWs(s, i);
        if (i >= s.size() || s[i] != ')') {
            return false;
        }
        ++i;
        return true;
    }

    std::optional<std::string> elementAsString(const bsoncxx::document::element& elem) {
        switch (elem.type()) {
        case bsoncxx::type::k_string:
            return std::string(elem.get_string().value);
        case bsoncxx::type::k_int32:
            return std::to_string(elem.get_int32().value);
        case bsoncxx::type::k_int64:
            return std::to_string(elem.get_int64().value);
        default:
            return std::nullopt;
        }
    }

    template <typename StreamContext>
    void appendElementValue(StreamContext&& ctx, const bsoncxx::document::element& elem);

    template <typename StreamContext>
    void appendFilterValue(StreamContext&& ctx, const std::string& field,
                           const bsoncxx::document::element& elem) {
        if (field == "_id") {
            if (const auto text = elementAsString(elem)) {
                if (isObjectIdHex(*text)) {
                    ctx << bsoncxx::oid{*text};
                    return;
                }
            }
        }
        appendElementValue(ctx, elem);
    }

    template <typename StreamContext>
    void appendElementValue(StreamContext&& ctx, const bsoncxx::document::element& elem) {
        switch (elem.type()) {
        case bsoncxx::type::k_string:
            ctx << std::string(elem.get_string().value);
            break;
        case bsoncxx::type::k_int32:
            ctx << elem.get_int32().value;
            break;
        case bsoncxx::type::k_int64:
            ctx << elem.get_int64().value;
            break;
        case bsoncxx::type::k_double:
            ctx << elem.get_double().value;
            break;
        case bsoncxx::type::k_bool:
            ctx << elem.get_bool().value;
            break;
        case bsoncxx::type::k_date:
            ctx << bsoncxx::types::b_date{elem.get_date().value};
            break;
        case bsoncxx::type::k_oid:
            ctx << elem.get_oid().value;
            break;
        case bsoncxx::type::k_null:
            ctx << bsoncxx::types::b_null{};
            break;
        default:
            ctx << bsoncxx::types::b_null{};
            break;
        }
    }

    std::optional<bsoncxx::document::value> readLiteralValue(std::string_view s, size_t& i) {
        skipWs(s, i);
        if (i >= s.size()) {
            return std::nullopt;
        }

        const size_t saved = i;
        if (const auto fn = readAlphaToken(s, i)) {
            if (iequals(*fn, "ISODate")) {
                std::string arg;
                if (readParenQuotedArg(s, i, arg)) {
                    if (const auto millis = parseMongoShellDateString(arg)) {
                        document d;
                        d << "v" << bsoncxx::types::b_date{*millis};
                        return d << finalize;
                    }
                    return std::nullopt;
                }
            } else if (iequals(*fn, "ObjectId")) {
                std::string arg;
                if (readParenQuotedArg(s, i, arg) && isObjectIdHex(arg)) {
                    document d;
                    d << "v" << bsoncxx::oid{arg};
                    return d << finalize;
                }
            }
            i = saved;
        }

        if (s[i] == '\'') {
            ++i;
            document d;
            d << "v" << readQuotedString(s, i, '\'');
            return d << finalize;
        }
        if (s[i] == '"') {
            ++i;
            document d;
            d << "v" << readQuotedString(s, i, '"');
            return d << finalize;
        }

        size_t tokenStart = i;
        while (i < s.size()) {
            const char c = s[i];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
                ++i;
            } else {
                break;
            }
        }
        if (i > tokenStart) {
            const std::string token = std::string(s.substr(tokenStart, i - tokenStart));
            document d;
            if (iequals(token, "true")) {
                d << "v" << true;
                return d << finalize;
            }
            if (iequals(token, "false")) {
                d << "v" << false;
                return d << finalize;
            }
            if (iequals(token, "null")) {
                d << "v" << bsoncxx::types::b_null{};
                return d << finalize;
            }

            bool hasAlpha = false;
            for (char c : token) {
                if (std::isalpha(static_cast<unsigned char>(c)) &&
                    c != 'e' && c != 'E') {
                    hasAlpha = true;
                    break;
                }
            }
            if (!hasAlpha) {
                try {
                    if (token.find('.') != std::string::npos || token.find('e') != std::string::npos ||
                        token.find('E') != std::string::npos) {
                        d << "v" << std::stod(token);
                    } else {
                        d << "v" << static_cast<std::int64_t>(std::stoll(token));
                    }
                    return d << finalize;
                } catch (...) {
                }
            }
            d << "v" << token;
            return d << finalize;
        }

        return std::nullopt;
    }

    std::string likeToRegex(std::string pattern) {
        std::string regex = "^";
        for (char ch : pattern) {
            switch (ch) {
            case '%':
                regex += ".*";
                break;
            case '_':
                regex += ".";
                break;
            case '.':
            case '^':
            case '$':
            case '*':
            case '+':
            case '?':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '|':
            case '\\':
                regex += '\\';
                regex += ch;
                break;
            default:
                regex += ch;
                break;
            }
        }
        regex += "$";
        return regex;
    }

    bool matchKeyword(std::string_view s, size_t& i, std::string_view kw) {
        skipWs(s, i);
        if (i + kw.size() > s.size()) {
            return false;
        }
        if (!iequals(s.substr(i, kw.size()), kw)) {
            return false;
        }
        if (i + kw.size() < s.size()) {
            const char next = s[i + kw.size()];
            if (std::isalnum(static_cast<unsigned char>(next)) || next == '_') {
                return false;
            }
        }
        i += kw.size();
        return true;
    }

    std::optional<bsoncxx::document::value> parseCondition(std::string cond) {
        cond = stripOuterParens(std::move(cond));
        if (cond.empty()) {
            return std::nullopt;
        }

        std::string_view s = cond;
        size_t i = 0;
        const std::string field = readIdentifier(s, i);
        if (field.empty()) {
            return std::nullopt;
        }
        skipWs(s, i);

        if (matchKeyword(s, i, "IS")) {
            skipWs(s, i);
            if (matchKeyword(s, i, "NOT")) {
                skipWs(s, i);
                if (!matchKeyword(s, i, "NULL")) {
                    return std::nullopt;
                }
                document d;
                d << field << open_document << "$ne" << bsoncxx::types::b_null{} << close_document;
                return d << finalize;
            }
            if (!matchKeyword(s, i, "NULL")) {
                return std::nullopt;
            }
            document d;
            d << field << bsoncxx::types::b_null{};
            return d << finalize;
        }

        if (matchKeyword(s, i, "LIKE")) {
            skipWs(s, i);
            if (i >= s.size() || s[i] != '\'') {
                return std::nullopt;
            }
            ++i;
            const std::string pattern = readQuotedString(s, i, '\'');
            document d;
            d << field << open_document << "$regex" << likeToRegex(pattern) << close_document;
            return d << finalize;
        }

        std::string op;
        if (i + 2 <= s.size() && s.substr(i, 2) == "!=") {
            op = "!=";
            i += 2;
        } else if (i + 2 <= s.size() && s.substr(i, 2) == "<>") {
            op = "<>";
            i += 2;
        } else if (i + 2 <= s.size() && s.substr(i, 2) == "<=") {
            op = "<=";
            i += 2;
        } else if (i + 2 <= s.size() && s.substr(i, 2) == ">=") {
            op = ">=";
            i += 2;
        } else if (i < s.size() && s[i] == '=') {
            op = "=";
            ++i;
        } else if (i < s.size() && s[i] == '<') {
            op = "<";
            ++i;
        } else if (i < s.size() && s[i] == '>') {
            op = ">";
            ++i;
        } else {
            return std::nullopt;
        }

        auto valueDoc = readLiteralValue(s, i);
        if (!valueDoc) {
            return std::nullopt;
        }
        skipWs(s, i);
        if (i != s.size()) {
            return std::nullopt;
        }

        const auto valueElem = valueDoc->view()["v"];
        document d;
        if (op == "=") {
            if (valueElem.type() == bsoncxx::type::k_date) {
                const auto millis = valueElem.get_date().value.count();
                // UI shows dates to second precision; match the whole second when no ms given.
                if (millis % 1000 == 0) {
                    const auto start =
                        bsoncxx::types::b_date{std::chrono::milliseconds{millis}};
                    const auto end =
                        bsoncxx::types::b_date{std::chrono::milliseconds{millis + 1000}};
                    d << field << open_document << "$gte" << start << "$lt" << end << close_document;
                    return d << finalize;
                }
            }
            appendFilterValue(d << field, field, valueElem);
        } else if (op == "!=" || op == "<>") {
            appendFilterValue(d << field << open_document << "$ne", field, valueElem);
            d << close_document;
        } else if (op == "<") {
            appendFilterValue(d << field << open_document << "$lt", field, valueElem);
            d << close_document;
        } else if (op == ">") {
            appendFilterValue(d << field << open_document << "$gt", field, valueElem);
            d << close_document;
        } else if (op == "<=") {
            appendFilterValue(d << field << open_document << "$lte", field, valueElem);
            d << close_document;
        } else if (op == ">=") {
            appendFilterValue(d << field << open_document << "$gte", field, valueElem);
            d << close_document;
        }
        return d << finalize;
    }

    std::vector<std::string> splitTopLevel(std::string_view s, std::string_view keyword) {
        std::vector<std::string> parts;
        size_t start = 0;
        int depth = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '(') {
                ++depth;
            } else if (s[i] == ')') {
                --depth;
            } else if (depth == 0 && i + keyword.size() <= s.size() &&
                       iequals(s.substr(i, keyword.size()), keyword)) {
                const bool leftOk = i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1]));
                const bool rightOk = i + keyword.size() == s.size() ||
                                       std::isspace(static_cast<unsigned char>(s[i + keyword.size()]));
                if (leftOk && rightOk) {
                    parts.push_back(trim(s.substr(start, i - start)));
                    i += keyword.size() - 1;
                    start = i + 1;
                }
            }
        }
        parts.push_back(trim(s.substr(start)));
        parts.erase(std::remove_if(parts.begin(), parts.end(),
                                  [](const std::string& p) { return p.empty(); }),
                    parts.end());
        return parts;
    }

    bsoncxx::document::value combineConditions(std::vector<bsoncxx::document::value> conditions,
                                               const char* opKey) {
        if (conditions.empty()) {
            return document{} << finalize;
        }
        if (conditions.size() == 1) {
            return std::move(conditions.front());
        }
        document out;
        const std::string opKeyStr(opKey);
        auto arr = out << opKeyStr << open_array;
        for (auto& c : conditions) {
            arr << bsoncxx::builder::concatenate(c.view());
        }
        arr << close_array;
        return out << finalize;
    }

    std::optional<bsoncxx::document::value> parseFilterExpr(std::string expr) {
        expr = stripOuterParens(trim(expr));
        if (expr.empty()) {
            return document{} << finalize;
        }

        const auto orParts = splitTopLevel(expr, "OR");
        if (orParts.size() > 1) {
            std::vector<bsoncxx::document::value> docs;
            docs.reserve(orParts.size());
            for (const auto& part : orParts) {
                auto doc = parseFilterExpr(part);
                if (!doc) {
                    return std::nullopt;
                }
                docs.push_back(std::move(*doc));
            }
            return combineConditions(std::move(docs), "$or");
        }

        const auto andParts = splitTopLevel(expr, "AND");
        if (andParts.size() > 1) {
            std::vector<bsoncxx::document::value> docs;
            docs.reserve(andParts.size());
            for (const auto& part : andParts) {
                auto doc = parseFilterExpr(part);
                if (!doc) {
                    return std::nullopt;
                }
                docs.push_back(std::move(*doc));
            }
            return combineConditions(std::move(docs), "$and");
        }

        return parseCondition(expr);
    }

    std::optional<bsoncxx::document::value> parseSqlStyleFilter(std::string filter) {
        return parseFilterExpr(std::move(filter));
    }
} // namespace

bool isValidMongoFilter(const std::string& filter) {
    const std::string trimmed = trim(filter);
    if (trimmed.empty()) {
        return true;
    }

    if (trimmed.front() == '{') {
        try {
            bsoncxx::from_json(trimmed);
            return true;
        } catch (...) {
            return false;
        }
    }

    return parseSqlStyleFilter(trimmed).has_value();
}

bsoncxx::document::value parseMongoFilter(const std::string& filter) {
    const std::string trimmed = trim(filter);
    if (trimmed.empty()) {
        return document{} << finalize;
    }

    if (trimmed.front() == '{') {
        try {
            return bsoncxx::from_json(trimmed);
        } catch (const std::exception& e) {
            spdlog::warn("MongoDB filter JSON parse failed: {}", e.what());
        }
    }

    if (auto doc = parseSqlStyleFilter(trimmed)) {
        return std::move(*doc);
    }

    spdlog::warn("MongoDB filter could not be parsed, ignoring filter: {}", trimmed);
    return document{} << finalize;
}

bsoncxx::document::value parseMongoSort(const std::string& sort) {
    const std::string trimmed = trim(sort);
    if (trimmed.empty()) {
        return document{} << finalize;
    }

    if (trimmed.front() == '{') {
        try {
            return bsoncxx::from_json(trimmed);
        } catch (const std::exception& e) {
            spdlog::warn("MongoDB sort JSON parse failed: {}", e.what());
        }
    }

    document out;
    std::string_view s = trimmed;
    size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        skipWs(s, i);
        if (i >= s.size()) {
            break;
        }
        const std::string field = readIdentifier(s, i);
        if (field.empty()) {
            break;
        }
        skipWs(s, i);
        int direction = 1;
        if (matchKeyword(s, i, "DESC")) {
            direction = -1;
        } else {
            matchKeyword(s, i, "ASC");
        }
        out << field << direction;
        any = true;
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
        }
    }

    if (!any) {
        spdlog::warn("MongoDB sort could not be parsed, ignoring sort: {}", trimmed);
        return document{} << finalize;
    }
    return out << finalize;
}
