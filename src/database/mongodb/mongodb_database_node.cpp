#include "database/mongodb/mongo_bson_format.hpp"
#include "database/mongodb/mongo_filter.hpp"
#include "database/mongodb/mongodb_database_node.hpp"
#include "database/mongodb.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <unordered_set>
#include <spdlog/spdlog.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>

DatabaseInterface* MongoDBDatabaseNode::ownerDatabase() const {
    return parentDb;
}

std::string MongoDBDatabaseNode::getFullPath() const {
    return name;
}

DatabaseType MongoDBDatabaseNode::getDatabaseType() const {
    if (parentDb) {
        return parentDb->getConnectionInfo().type;
    }
    return DatabaseType::MONGODB;
}

void MongoDBDatabaseNode::startTablesLoadAsync(bool force) {
    startCollectionsLoadAsync(force);
}

void MongoDBDatabaseNode::startViewsLoadAsync(bool force) {
    // MongoDB views are just special collections, handled in collectionsLoader
    // For now, views are empty
    if (force) {
        views.clear();
    }
    viewsLoaded = true;
}

void MongoDBDatabaseNode::startCollectionsLoadAsync(bool force) {
    spdlog::debug("startCollectionsLoadAsync for db: {}{}", name,
                  (force ? " (force refresh)" : ""));
    if (!parentDb) {
        return;
    }

    if (collectionsLoader.isRunning()) {
        return;
    }

    if (force) {
        collections.clear();
        collectionsLoaded = false;
        lastCollectionsError.clear();
    }

    if (!force && collectionsLoaded) {
        return;
    }

    collectionsLoader.start([this]() { return getCollectionsAsync(); });
}

void MongoDBDatabaseNode::checkCollectionsStatusAsync() {
    collectionsLoader.check([this](const std::vector<Table>& result) {
        collections = result;
        spdlog::debug("Async collection loading completed for database {}. Found {} collections",
                      name, collections.size());
        collectionsLoaded = true;
    });
}

std::vector<Table> MongoDBDatabaseNode::getCollectionsAsync() {
    std::vector<Table> result;

    if (!collectionsLoader.isRunning()) {
        return result;
    }

    try {
        if (!parentDb) {
            return result;
        }

        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto collectionNames = db.list_collection_names();

        spdlog::debug(
            std::format("Found {} collections in database {}", collectionNames.size(), name));

        for (const auto& collName : collectionNames) {
            if (!collectionsLoader.isRunning()) {
                break;
            }

            // Skip system collections
            if (collName.starts_with("system.")) {
                continue;
            }

            Table collection;
            collection.name = collName;
            collection.fullName = parentDb->getConnectionInfo().name + "." + name + "." + collName;

            // Infer schema by sampling documents
            collection.columns = inferSchemaFromSample(collName, 100);
            collection.indexes = getCollectionIndexes(collName);

            result.push_back(std::move(collection));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting collections for database {}: {}", name, e.what());
        lastCollectionsError = e.what();
    }

    return result;
}

std::vector<Column> MongoDBDatabaseNode::inferSchemaFromSample(const std::string& collectionName,
                                                               int sampleSize) {
    std::vector<Column> columns;
    std::unordered_set<std::string> seenFields;

    try {
        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto coll = db[collectionName];

        mongocxx::options::find opts;
        opts.limit(sampleSize);

        auto cursor = coll.find({}, opts);

        for (auto&& doc : cursor) {
            for (auto&& elem : doc) {
                std::string fieldName(elem.key());
                if (!seenFields.insert(fieldName).second) {
                    continue;
                }

                Column col;
                col.name = fieldName;

                // Map BSON type to string
                switch (elem.type()) {
                case bsoncxx::type::k_double:
                    col.type = "double";
                    break;
                case bsoncxx::type::k_string:
                    col.type = "string";
                    break;
                case bsoncxx::type::k_document:
                    col.type = "object";
                    break;
                case bsoncxx::type::k_array:
                    col.type = "array";
                    break;
                case bsoncxx::type::k_binary:
                    col.type = "binary";
                    break;
                case bsoncxx::type::k_oid:
                    col.type = "objectId";
                    break;
                case bsoncxx::type::k_bool:
                    col.type = "bool";
                    break;
                case bsoncxx::type::k_date:
                    col.type = "date";
                    break;
                case bsoncxx::type::k_null:
                    col.type = "null";
                    break;
                case bsoncxx::type::k_int32:
                    col.type = "int32";
                    break;
                case bsoncxx::type::k_int64:
                    col.type = "int64";
                    break;
                case bsoncxx::type::k_decimal128:
                    col.type = "decimal128";
                    break;
                default:
                    col.type = "unknown";
                    break;
                }

                // _id is always the primary key
                if (fieldName == "_id") {
                    col.isPrimaryKey = true;
                    col.isNotNull = true;
                }

                columns.push_back(col);
            }
        }

    } catch (const std::exception& e) {
        spdlog::error("Error inferring schema for {}: {}", collectionName, e.what());
    }

    return columns;
}

std::vector<Index> MongoDBDatabaseNode::getCollectionIndexes(const std::string& collectionName) {
    std::vector<Index> indexes;

    try {
        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto coll = db[collectionName];

        auto cursor = coll.list_indexes();
        for (auto&& doc : cursor) {
            Index idx;

            // index name
            if (auto it = doc.find("name"); it != doc.end()) {
                idx.name = std::string(it->get_string().value);
            }

            // key fields and direction
            if (auto it = doc.find("key");
                it != doc.end() && it->type() == bsoncxx::type::k_document) {
                for (auto&& field : it->get_document().value) {
                    idx.columns.emplace_back(field.key());
                }
            }

            // unique flag
            if (auto it = doc.find("unique");
                it != doc.end() && it->type() == bsoncxx::type::k_bool) {
                idx.isUnique = it->get_bool().value;
            }

            // _id index is the primary index
            if (idx.name == "_id_") {
                idx.isPrimary = true;
                idx.isUnique = true;
            }

            indexes.push_back(std::move(idx));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error fetching indexes for {}.{}: {}", name, collectionName, e.what());
    }

    return indexes;
}

void MongoDBDatabaseNode::startTableRefreshAsync(const std::string& collectionName) {
    spdlog::debug("Starting async refresh for collection: {}", collectionName);

    if (collectionRefreshLoaders.contains(collectionName) &&
        collectionRefreshLoaders[collectionName].isRunning()) {
        spdlog::debug("Collection {} is already being refreshed", collectionName);
        return;
    }

    collectionRefreshLoaders[collectionName].start(
        [this, collectionName]() { return refreshCollectionAsync(collectionName); });
    spdlog::debug("Async refresh started for collection: {}", collectionName);
}

void MongoDBDatabaseNode::checkTableRefreshStatusAsync(const std::string& collectionName) {
    auto it = collectionRefreshLoaders.find(collectionName);
    if (it == collectionRefreshLoaders.end()) {
        return;
    }

    it->second.check([this, collectionName](const Table& refreshedCollection) {
        const auto collIt = std::ranges::find_if(
            collections, [&collectionName](const Table& t) { return t.name == collectionName; });

        if (collIt != collections.end()) {
            *collIt = refreshedCollection;
            spdlog::debug("Collection {} refreshed successfully", collectionName);
        }

        collectionRefreshLoaders.erase(collectionName);
    });
}

Table MongoDBDatabaseNode::refreshCollectionAsync(const std::string& collectionName) {
    spdlog::debug("Refreshing collection: {}", collectionName);

    Table refreshedCollection;
    refreshedCollection.name = collectionName;
    refreshedCollection.fullName =
        parentDb->getConnectionInfo().name + "." + name + "." + collectionName;

    try {
        refreshedCollection.columns = inferSchemaFromSample(collectionName, 100);
        refreshedCollection.indexes = getCollectionIndexes(collectionName);
    } catch (const std::exception& e) {
        spdlog::error("Error refreshing collection {}: {}", collectionName, e.what());
        throw;
    }

    return refreshedCollection;
}

bool MongoDBDatabaseNode::isTableRefreshing(const std::string& collectionName) const {
    auto it = collectionRefreshLoaders.find(collectionName);
    bool isRefreshing = it != collectionRefreshLoaders.end() && it->second.isRunning();
    if (isRefreshing) {
        spdlog::debug("Collection {} is currently refreshing", collectionName);
    }
    return isRefreshing;
}

std::vector<std::vector<std::string>>
MongoDBDatabaseNode::getTableData(const Table& collection, const int limit, const int offset,
                                  const std::string& filter, const std::string& sort) {
    std::vector<std::vector<std::string>> result;
    const std::string& collectionName = collection.name;

    try {
        // Get column names to know which fields to extract
        auto columnNames = getColumnNames(collection);

        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto coll = db[collectionName];

        mongocxx::options::find opts;
        opts.limit(limit);
        opts.skip(offset);

        bsoncxx::document::value filterDoc = parseMongoFilter(filter);
        if (!sort.empty()) {
            const auto sortDoc = parseMongoSort(sort);
            if (!sortDoc.view().empty()) {
                opts.sort(sortDoc.view());
            }
        }

        auto cursor = coll.find(filterDoc.view(), opts);

        for (auto&& doc : cursor) {
            std::vector<std::string> row;
            row.reserve(columnNames.size());

            // Extract value for each column
            for (const auto& colName : columnNames) {
                if (auto elem = doc[colName]) {
                    row.push_back(mongo_bson::elementToDisplayString(elem));
                } else {
                    row.push_back(""); // Field not present in this document
                }
            }

            result.push_back(std::move(row));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting collection data for {}: {}", collectionName, e.what());
    }

    return result;
}

std::vector<std::string>
MongoDBDatabaseNode::getCollectionDocumentsAsJson(const Table& collection, const int limit,
                                                  const int offset, const std::string& filter,
                                                  const std::string& sort) {
    std::vector<std::string> result;
    const std::string& collectionName = collection.name;

    try {
        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto coll = db[collectionName];

        mongocxx::options::find opts;
        opts.limit(limit);
        opts.skip(offset);

        bsoncxx::document::value filterDoc = parseMongoFilter(filter);
        if (!sort.empty()) {
            const auto sortDoc = parseMongoSort(sort);
            if (!sortDoc.view().empty()) {
                opts.sort(sortDoc.view());
            }
        }

        auto cursor = coll.find(filterDoc.view(), opts);
        for (auto&& doc : cursor) {
            result.push_back(bsoncxx::to_json(doc));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting collection JSON for {}: {}", collectionName, e.what());
    }

    return result;
}

std::vector<std::string> MongoDBDatabaseNode::getColumnNames(const Table& collection) {
    if (!collection.columns.empty()) {
        std::vector<std::string> names;
        names.reserve(collection.columns.size());
        for (const auto& col : collection.columns) {
            names.push_back(col.name);
        }
        return names;
    }

    const std::string& collectionName = collection.name;
    // Find the collection in our loaded collections to get inferred schema
    const auto it = std::ranges::find_if(
        collections, [&collectionName](const Table& t) { return t.name == collectionName; });

    if (it != collections.end() && !it->columns.empty()) {
        std::vector<std::string> names;
        names.reserve(it->columns.size());
        for (const auto& col : it->columns) {
            names.push_back(col.name);
        }
        return names;
    }

    // Fallback if schema not yet inferred
    return {"_id", "document"};
}

int MongoDBDatabaseNode::getRowCount(const Table& collection, const std::string& filter) {
    const std::string& collectionName = collection.name;
    try {
        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto coll = db[collectionName];

        const auto filterDoc = parseMongoFilter(filter);
        return static_cast<int>(coll.count_documents(filterDoc.view()));
    } catch (const std::exception& e) {
        spdlog::error("Error getting row count for {}: {}", collectionName, e.what());
        return 0;
    }
}

int MongoDBDatabaseNode::getEstimatedDocumentCount(const Table& collection) {
    const std::string& collectionName = collection.name;
    try {
        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto coll = db[collectionName];
        return static_cast<int>(coll.estimated_document_count());
    } catch (const std::exception& e) {
        spdlog::error("Error getting estimated document count for {}: {}", collectionName,
                      e.what());
        return 0;
    }
}

QueryResult MongoDBDatabaseNode::executeQuery(const std::string& query, const int rowLimit) {
    if (parentDb) {
        return parentDb->executeQueryForDatabase(query, rowLimit, name);
    }

    QueryResult result;
    StatementResult s;
    s.success = false;
    s.errorMessage = "No parent database connection";
    result.statements.push_back(std::move(s));
    return result;
}

void MongoDBDatabaseNode::checkLoadingStatus() {
    checkCollectionsStatusAsync();
    // Views are handled as part of collections
}

std::pair<bool, std::string>
MongoDBDatabaseNode::dropCollection(const std::string& collectionName) {
    auto query =
        std::format(R"({{"database": "{}", "collection": "{}", "command": "dropCollection"}})",
                    name, collectionName);
    auto r = executeQuery(query);
    if (r.success()) {
        startCollectionsLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}
