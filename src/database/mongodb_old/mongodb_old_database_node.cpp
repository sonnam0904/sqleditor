#include "database/mongodb_old/mongodb_old_database_node.hpp"
#include "database/mongodb/mongo_bson_format.hpp"
#include "database/mongodb/mongo_filter.hpp"
#include "database/mongodb_old.hpp"
#include <algorithm>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/json.hpp>
#include <format>
#include <ranges>
#include <spdlog/spdlog.h>
#include <unordered_set>

DatabaseInterface* MongoDBOldDatabaseNode::ownerDatabase() const {
    return parentDb;
}

std::string MongoDBOldDatabaseNode::getFullPath() const {
    return name;
}

DatabaseType MongoDBOldDatabaseNode::getDatabaseType() const {
    if (parentDb) {
        return parentDb->getConnectionInfo().type;
    }
    return DatabaseType::MONGODB_OLD;
}

void MongoDBOldDatabaseNode::startTablesLoadAsync(bool force) {
    startCollectionsLoadAsync(force);
}

void MongoDBOldDatabaseNode::startViewsLoadAsync(bool force) {
    if (force) {
        views.clear();
    }
    viewsLoaded = true;
}

void MongoDBOldDatabaseNode::startCollectionsLoadAsync(bool force) {
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

void MongoDBOldDatabaseNode::checkCollectionsStatusAsync() {
    collectionsLoader.check([this](const std::vector<Table>& result) {
        collections = result;
        collectionsLoaded = true;
    });
}

std::vector<Table> MongoDBOldDatabaseNode::getCollectionsAsync() {
    std::vector<Table> result;
    if (!collectionsLoader.isRunning() || !parentDb) {
        return result;
    }

    try {
        std::string error;
        auto names = parentDb->getLegacyClient().listCollections(name, error);
        if (!error.empty()) {
            lastCollectionsError = error;
            return result;
        }

        for (const auto& collName : names) {
            if (!collectionsLoader.isRunning()) {
                break;
            }
            if (collName.starts_with("system.")) {
                continue;
            }

            Table collection;
            collection.name = collName;
            collection.fullName = parentDb->getConnectionInfo().name + "." + name + "." + collName;
            collection.columns = inferSchemaFromSample(collName, 100);
            result.push_back(std::move(collection));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting collections for database {}: {}", name, e.what());
        lastCollectionsError = e.what();
    }

    return result;
}

std::vector<Column> MongoDBOldDatabaseNode::inferSchemaFromSample(const std::string& collectionName,
                                                                  int sampleSize) {
    std::vector<Column> columns;
    if (!parentDb) {
        return columns;
    }

    try {
        std::string error;
        const auto fields = parentDb->getLegacyClient().sampleSchema(name, collectionName,
                                                                     sampleSize, error);
        if (!error.empty()) {
            spdlog::error("Error inferring schema for {}: {}", collectionName, error);
            return columns;
        }

        for (const auto& field : fields) {
            Column col;
            col.name = field.name;
            col.type = field.type;
            if (field.name == "_id") {
                col.isPrimaryKey = true;
                col.isNotNull = true;
            }
            columns.push_back(std::move(col));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error inferring schema for {}: {}", collectionName, e.what());
    }

    return columns;
}

void MongoDBOldDatabaseNode::startTableRefreshAsync(const std::string& collectionName) {
    if (collectionRefreshLoaders.contains(collectionName) &&
        collectionRefreshLoaders[collectionName].isRunning()) {
        return;
    }

    collectionRefreshLoaders[collectionName].start(
        [this, collectionName]() { return refreshCollectionAsync(collectionName); });
}

void MongoDBOldDatabaseNode::checkTableRefreshStatusAsync(const std::string& collectionName) {
    auto it = collectionRefreshLoaders.find(collectionName);
    if (it == collectionRefreshLoaders.end()) {
        return;
    }

    it->second.check([this, collectionName](const Table& refreshedCollection) {
        const auto collIt = std::ranges::find_if(
            collections, [&collectionName](const Table& t) { return t.name == collectionName; });

        if (collIt != collections.end()) {
            *collIt = refreshedCollection;
        }
        collectionRefreshLoaders.erase(collectionName);
    });
}

Table MongoDBOldDatabaseNode::refreshCollectionAsync(const std::string& collectionName) {
    Table refreshedCollection;
    refreshedCollection.name = collectionName;
    if (parentDb) {
        refreshedCollection.fullName =
            parentDb->getConnectionInfo().name + "." + name + "." + collectionName;
    }
    refreshedCollection.columns = inferSchemaFromSample(collectionName, 100);
    return refreshedCollection;
}

bool MongoDBOldDatabaseNode::isTableRefreshing(const std::string& collectionName) const {
    auto it = collectionRefreshLoaders.find(collectionName);
    return it != collectionRefreshLoaders.end() && it->second.isRunning();
}

std::vector<std::vector<std::string>>
MongoDBOldDatabaseNode::getTableData(const Table& collection, const int limit, const int offset,
                                     const std::string& filter, const std::string& sort) {
    std::vector<std::vector<std::string>> result;
    if (!parentDb) {
        return result;
    }

    const std::string& collectionName = collection.name;
    try {
        const auto columnNames = getColumnNames(collection);
        std::string error;
        const auto filterJson = bsoncxx::to_json(parseMongoFilter(filter));
        std::string sortJson;
        if (!sort.empty()) {
            sortJson = bsoncxx::to_json(parseMongoSort(sort));
        }

        const auto findResult = parentDb->getLegacyClient().find(
            name, collectionName, filterJson, limit, offset, sortJson, error);
        if (!error.empty()) {
            spdlog::error("Error getting collection data for {}: {}", collectionName, error);
            return result;
        }

        for (const auto& docJson : findResult.documentsJson) {
            const auto doc = bsoncxx::from_json(docJson);
            std::vector<std::string> row;
            row.reserve(columnNames.size());
            for (const auto& colName : columnNames) {
                if (auto elem = doc[colName]) {
                    row.push_back(mongo_bson::elementToDisplayString(elem));
                } else {
                    row.push_back("");
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
MongoDBOldDatabaseNode::getCollectionDocumentsAsJson(const Table& collection, const int limit,
                                                     const int offset, const std::string& filter,
                                                     const std::string& sort) {
    std::vector<std::string> result;
    if (!parentDb) {
        return result;
    }

    try {
        std::string error;
        const auto filterJson = bsoncxx::to_json(parseMongoFilter(filter));
        std::string sortJson;
        if (!sort.empty()) {
            sortJson = bsoncxx::to_json(parseMongoSort(sort));
        }

        const auto findResult = parentDb->getLegacyClient().find(
            name, collection.name, filterJson, limit, offset, sortJson, error);
        if (!error.empty()) {
            spdlog::error("Error getting collection JSON for {}: {}", collection.name, error);
            return result;
        }
        return findResult.documentsJson;
    } catch (const std::exception& e) {
        spdlog::error("Error getting collection JSON for {}: {}", collection.name, e.what());
    }

    return result;
}

std::vector<std::string> MongoDBOldDatabaseNode::getColumnNames(const Table& collection) {
    if (!collection.columns.empty()) {
        std::vector<std::string> names;
        names.reserve(collection.columns.size());
        for (const auto& col : collection.columns) {
            names.push_back(col.name);
        }
        return names;
    }

    const auto it = std::ranges::find_if(collections, [&collection](const Table& t) {
        return t.name == collection.name;
    });

    if (it != collections.end() && !it->columns.empty()) {
        std::vector<std::string> names;
        names.reserve(it->columns.size());
        for (const auto& col : it->columns) {
            names.push_back(col.name);
        }
        return names;
    }

    return {"_id", "document"};
}

int MongoDBOldDatabaseNode::getRowCount(const Table& collection, const std::string& filter) {
    if (!parentDb) {
        return 0;
    }

    try {
        std::string error;
        const auto filterJson = bsoncxx::to_json(parseMongoFilter(filter));
        const int64_t count =
            parentDb->getLegacyClient().count(name, collection.name, filterJson, error);
        if (!error.empty()) {
            spdlog::error("Error getting row count for {}: {}", collection.name, error);
            return 0;
        }
        return static_cast<int>(count);
    } catch (const std::exception& e) {
        spdlog::error("Error getting row count for {}: {}", collection.name, e.what());
        return 0;
    }
}

int MongoDBOldDatabaseNode::getEstimatedDocumentCount(const Table& collection) {
    if (!parentDb) {
        return 0;
    }

    try {
        std::string error;
        const int64_t count =
            parentDb->getLegacyClient().estimatedDocumentCount(name, collection.name, error);
        if (!error.empty()) {
            spdlog::error("Error getting estimated document count for {}: {}", collection.name,
                          error);
            return 0;
        }
        return static_cast<int>(count);
    } catch (const std::exception& e) {
        spdlog::error("Error getting estimated document count for {}: {}", collection.name,
                      e.what());
        return 0;
    }
}

QueryResult MongoDBOldDatabaseNode::executeQuery(const std::string& query, const int rowLimit) {
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

void MongoDBOldDatabaseNode::checkLoadingStatus() {
    checkCollectionsStatusAsync();
}

std::pair<bool, std::string>
MongoDBOldDatabaseNode::dropCollection(const std::string& collectionName) {
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
