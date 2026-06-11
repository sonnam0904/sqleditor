#include "database/mongodb/mongo_filter.hpp"

#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <cctype>
#include <optional>
#include <spdlog/spdlog.h>
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

        size_t start = i;
        if (s[i] == '-') {
            ++i;
        }
        while (i < s.size() &&
               (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' ||
                s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        if (i > start) {
            const std::string num = std::string(s.substr(start, i - start));
            document d;
            if (num.find('.') != std::string::npos || num.find('e') != std::string::npos ||
                num.find('E') != std::string::npos) {
                d << "v" << std::stod(num);
            } else {
                d << "v" << static_cast<std::int64_t>(std::stoll(num));
            }
            return d << finalize;
        }

        const size_t kwStart = i;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        const std::string kw = std::string(s.substr(kwStart, i - kwStart));
        document d;
        if (iequals(kw, "true")) {
            d << "v" << true;
            return d << finalize;
        }
        if (iequals(kw, "false")) {
            d << "v" << false;
            return d << finalize;
        }
        if (iequals(kw, "null")) {
            d << "v" << bsoncxx::types::b_null{};
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
            appendElementValue(d << field, valueElem);
        } else if (op == "!=" || op == "<>") {
            appendElementValue(d << field << open_document << "$ne", valueElem);
            d << close_document;
        } else if (op == "<") {
            appendElementValue(d << field << open_document << "$lt", valueElem);
            d << close_document;
        } else if (op == ">") {
            appendElementValue(d << field << open_document << "$gt", valueElem);
            d << close_document;
        } else if (op == "<=") {
            appendElementValue(d << field << open_document << "$lte", valueElem);
            d << close_document;
        } else if (op == ">=") {
            appendElementValue(d << field << open_document << "$gte", valueElem);
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

    std::optional<bsoncxx::document::value> parseSqlStyleFilter(std::string filter) {
        filter = trim(filter);
        if (filter.empty()) {
            return document{} << finalize;
        }

        const auto orParts = splitTopLevel(filter, "OR");
        std::vector<bsoncxx::document::value> orDocs;
        for (const auto& orPart : orParts) {
            const auto andParts = splitTopLevel(orPart, "AND");
            std::vector<bsoncxx::document::value> andDocs;
            for (const auto& andPart : andParts) {
                auto cond = parseCondition(andPart);
                if (!cond) {
                    return std::nullopt;
                }
                andDocs.push_back(std::move(*cond));
            }
            orDocs.push_back(combineConditions(std::move(andDocs), "$and"));
        }
        return combineConditions(std::move(orDocs), "$or");
    }
} // namespace

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
