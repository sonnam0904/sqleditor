#pragma once

#include "database/db.hpp"
#include <bsoncxx/document/view.hpp>
#include <mongocxx/cursor.hpp>
#include <vector>

namespace mongo_bson {
    std::string elementToDisplayString(const bsoncxx::document::element& elem);

    std::vector<std::string>
    collectColumnNames(const std::vector<bsoncxx::document::view>& documents);

    void appendDocumentsAsTable(StatementResult& result,
                                const std::vector<bsoncxx::document::view>& documents);

    void appendCursorAsTable(StatementResult& result, mongocxx::cursor& cursor);

    // Compass-style label for inferred schema types (objectId -> ObjectId, etc.)
    std::string mongoTypeDisplayLabel(std::string_view schemaType);
} // namespace mongo_bson
