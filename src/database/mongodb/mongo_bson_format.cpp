#include "database/mongodb/mongo_bson_format.hpp"


#include <algorithm>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <ctime>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace mongo_bson {
    std::string elementToDisplayString(const bsoncxx::document::element& elem) {
        if (!elem) {
            return "";
        }

        switch (elem.type()) {
        case bsoncxx::type::k_string:
            return std::string(elem.get_string().value);
        case bsoncxx::type::k_int32:
            return std::to_string(elem.get_int32().value);
        case bsoncxx::type::k_int64:
            return std::to_string(elem.get_int64().value);
        case bsoncxx::type::k_double:
            return std::to_string(elem.get_double().value);
        case bsoncxx::type::k_bool:
            return elem.get_bool().value ? "true" : "false";
        case bsoncxx::type::k_oid:
            return elem.get_oid().value.to_string();
        case bsoncxx::type::k_date: {
            const auto millis = elem.get_date().value.count();
            const auto seconds = millis / 1000;
            const auto time = static_cast<std::time_t>(seconds);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &time);
#else
            gmtime_r(&time, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
            return std::string(buf);
        }
        case bsoncxx::type::k_null:
            return std::string(NULL_SENTINEL);
        case bsoncxx::type::k_decimal128:
            return elem.get_decimal128().value.to_string();
        case bsoncxx::type::k_document:
            return bsoncxx::to_json(elem.get_document().value);
        case bsoncxx::type::k_array:
            return bsoncxx::to_json(elem.get_array().value);
        default:
            try {
                auto wrapper = bsoncxx::builder::stream::document{}
                               << "v" << elem.get_value() << bsoncxx::builder::stream::finalize;
                return bsoncxx::to_json(wrapper.view());
            } catch (...) {
                return "<unknown>";
            }
        }
    }

    std::vector<std::string>
    collectColumnNames(const std::vector<bsoncxx::document::view>& documents) {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        for (const auto& doc : documents) {
            for (auto&& elem : doc) {
                const std::string key(elem.key());
                if (seen.insert(key).second) {
                    result.push_back(key);
                }
            }
        }
        return result;
    }

    void appendDocumentsAsTable(StatementResult& result,
                              const std::vector<bsoncxx::document::view>& documents) {
        if (documents.empty()) {
            result.message = "Returned 0 documents";
            return;
        }

        result.columnNames = collectColumnNames(documents);
        result.tableData.reserve(documents.size());
        result.mongoDocumentJson.reserve(documents.size());

        for (const auto& doc : documents) {
            result.mongoDocumentJson.push_back(bsoncxx::to_json(doc));
            std::vector<std::string> row;
            row.reserve(result.columnNames.size());
            for (const auto& colName : result.columnNames) {
                if (auto elem = doc[colName]) {
                    row.push_back(elementToDisplayString(elem));
                } else {
                    row.push_back("");
                }
            }
            result.tableData.push_back(std::move(row));
        }

        result.message = std::format("Returned {} document{}", result.tableData.size(),
                                     result.tableData.size() == 1 ? "" : "s");
    }

    void appendCursorAsTable(StatementResult& result, mongocxx::cursor& cursor) {
        std::vector<bsoncxx::document::view> documents;
        for (auto&& doc : cursor) {
            documents.push_back(doc);
        }
        appendDocumentsAsTable(result, documents);
    }

    std::string mongoTypeDisplayLabel(std::string_view schemaType) {
        static const std::unordered_map<std::string, std::string> labels = {
            {"objectId", "ObjectId"},   {"string", "String"},     {"int32", "Int32"},
            {"int64", "Int64"},         {"double", "Double"},     {"bool", "Boolean"},
            {"date", "Date"},           {"object", "Object"},     {"array", "Array"},
            {"null", "Null"},           {"binary", "Binary"},     {"decimal128", "Decimal128"},
            {"unknown", "Mixed"},
        };
        const std::string key(schemaType);
        if (const auto it = labels.find(key); it != labels.end()) {
            return it->second;
        }
        if (key.empty()) {
            return {};
        }
        std::string out = key;
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        return out;
    }
} // namespace mongo_bson
