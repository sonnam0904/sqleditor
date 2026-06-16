#include "database/mongodb.hpp"
#include "database/mongodb/mongo_bson_format.hpp"
#include "database/mongodb/mongo_shell.hpp"
#include "database/server_version.hpp"
#include <bsoncxx/json.hpp>
#include <format>
#include <ranges>
#include <spdlog/spdlog.h>

namespace {
    std::string trimLiteral(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.remove_prefix(1);
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    bsoncxx::document::view_or_value parseShellDocument(std::string_view literal) {
        if (trimLiteral(literal).empty()) {
            return bsoncxx::builder::stream::document{} << bsoncxx::builder::stream::finalize;
        }
        return bsoncxx::from_json(shellLiteralToExtendedJson(literal));
    }

    bsoncxx::document::value parseShellBson(std::string_view literal) {
        return bsoncxx::from_json(shellLiteralToExtendedJson(literal));
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

    std::string friendlyMongoConnectError(const std::string& raw, bool legacy) {
        if (raw.find("wire version") != std::string::npos &&
            raw.find("requires at least") != std::string::npos) {
            if (legacy) {
                return "MongoDB Legacy requires server 4.2 or newer. MongoDB 3.x is not "
                       "supported by the bundled driver (wire version 4 = MongoDB 3.2).";
            }
            return "MongoDB server is too old. This app requires MongoDB 4.2 or newer.";
        }
        return raw;
    }

    std::string fetchMongoServerVersion(mongocxx::client& client, std::string_view preferredDb) {
        const auto tryBuildInfo = [&](const std::string& dbName) -> std::string {
            auto database = client[dbName];
            bsoncxx::builder::stream::document cmd;
            cmd << "buildInfo" << 1;
            const auto result = database.run_command(cmd.view());
            const auto view = result.view();
            if (view["version"]) {
                return std::string(view["version"].get_string().value);
            }
            return {};
        };

        try {
            if (!preferredDb.empty()) {
                if (auto version = tryBuildInfo(std::string(preferredDb)); !version.empty()) {
                    return version;
                }
            }
            if (auto version = tryBuildInfo("admin"); !version.empty()) {
                return version;
            }
            return tryBuildInfo("test");
        } catch (const std::exception& e) {
            spdlog::warn("Failed to read MongoDB server version: {}", e.what());
        }
        return {};
    }

    void appendFindRows(StatementResult& s, mongocxx::cursor& cursor) {
        mongo_bson::appendCursorAsTable(s, cursor);
    }

    bool executeMongoShellCommand(const MongoShellCommand& cmd, mongocxx::database& db,
                                  StatementResult& s, int rowLimit) {
        const auto& method = cmd.method;

        if (iequals(method, "createCollection")) {
            if (cmd.args.empty()) {
                s.success = false;
                s.errorMessage = "createCollection requires a collection name";
                return false;
            }
            std::string collName = trimLiteral(cmd.args.front());
            if (collName.size() >= 2 &&
                ((collName.front() == '"' && collName.back() == '"') ||
                 (collName.front() == '\'' && collName.back() == '\''))) {
                collName = collName.substr(1, collName.size() - 2);
            }
            db.create_collection(collName);
            s.message = "Collection created successfully";
            return true;
        }

        if (iequals(method, "runCommand")) {
            if (cmd.args.empty()) {
                s.success = false;
                s.errorMessage = "runCommand requires a document argument";
                return false;
            }
            auto cmdResult = db.run_command(parseShellDocument(cmd.args.front()));
            s.columnNames.push_back("result");
            std::vector<std::string> row;
            row.push_back(bsoncxx::to_json(cmdResult.view()));
            s.tableData.push_back(std::move(row));
            s.message = "Command executed successfully";
            return true;
        }

        if (cmd.collection.empty()) {
            s.success = false;
            s.errorMessage = std::format("Unknown db command: db.{}()", method);
            return false;
        }

        auto coll = db[cmd.collection];
        const int effectiveLimit = cmd.limit >= 0 ? cmd.limit : rowLimit;

        if (iequals(method, "find") || iequals(method, "findOne")) {
            bsoncxx::document::view_or_value filter = parseShellDocument(
                cmd.args.empty() ? "{}" : cmd.args[0]);
            mongocxx::options::find opts;
            opts.limit(iequals(method, "findOne") ? 1 : effectiveLimit);
            if (cmd.skip > 0) {
                opts.skip(cmd.skip);
            }
            auto cursor = coll.find(filter, opts);
            appendFindRows(s, cursor);
            return true;
        }

        if (iequals(method, "aggregate")) {
            mongocxx::pipeline pipeline;
            if (!cmd.args.empty()) {
                const auto pipelineValue = parseShellBson(cmd.args[0]);
                for (auto&& stage : pipelineValue.view()) {
                    pipeline.append_stage(stage.get_document().value);
                }
            }
            auto cursor = coll.aggregate(pipeline);
            appendFindRows(s, cursor);
            return true;
        }

        if (iequals(method, "insertOne")) {
            if (cmd.args.empty()) {
                s.success = false;
                s.errorMessage = "insertOne requires a document argument";
                return false;
            }
            coll.insert_one(parseShellDocument(cmd.args.front()));
            s.message = "Insert executed successfully";
            return true;
        }

        if (iequals(method, "insertMany")) {
            if (cmd.args.empty()) {
                s.success = false;
                s.errorMessage = "insertMany requires an array argument";
                return false;
            }
            std::vector<bsoncxx::document::view> docs;
            for (auto&& d : parseShellBson(cmd.args.front()).view()) {
                docs.push_back(d.get_document().value);
            }
            coll.insert_many(docs);
            s.message = std::format("Inserted {} document{}", docs.size(),
                                    docs.size() == 1 ? "" : "s");
            return true;
        }

        if (iequals(method, "updateOne") || iequals(method, "updateMany")) {
            if (cmd.args.size() < 2) {
                s.success = false;
                s.errorMessage = std::format("{} requires filter and update arguments", method);
                return false;
            }
            const auto filter = parseShellDocument(cmd.args[0]);
            const auto update = parseShellDocument(cmd.args[1]);
            if (iequals(method, "updateOne")) {
                auto result = coll.update_one(filter, update);
                s.affectedRows = result ? static_cast<int>(result->modified_count()) : 0;
            } else {
                auto result = coll.update_many(filter, update);
                s.affectedRows = result ? static_cast<int>(result->modified_count()) : 0;
            }
            s.message = std::format("Updated {} document{}", s.affectedRows,
                                    s.affectedRows == 1 ? "" : "s");
            return true;
        }

        if (iequals(method, "deleteOne") || iequals(method, "deleteMany")) {
            if (cmd.args.empty()) {
                s.success = false;
                s.errorMessage = std::format("{} requires a filter argument", method);
                return false;
            }
            const auto filter = parseShellDocument(cmd.args[0]);
            if (iequals(method, "deleteOne")) {
                auto result = coll.delete_one(filter);
                s.affectedRows = result ? static_cast<int>(result->deleted_count()) : 0;
            } else {
                auto result = coll.delete_many(filter);
                s.affectedRows = result ? static_cast<int>(result->deleted_count()) : 0;
            }
            s.message = std::format("Deleted {} document{}", s.affectedRows,
                                    s.affectedRows == 1 ? "" : "s");
            return true;
        }

        if (iequals(method, "count") || iequals(method, "countDocuments")) {
            bsoncxx::document::view_or_value filter = parseShellDocument(
                cmd.args.empty() ? "{}" : cmd.args[0]);
            const auto count = coll.count_documents(filter);
            s.columnNames.push_back("count");
            s.tableData.push_back({std::to_string(count)});
            s.message = std::format("Count: {}", count);
            return true;
        }

        if (iequals(method, "drop")) {
            coll.drop();
            s.message = "Collection dropped successfully";
            return true;
        }

        s.success = false;
        s.errorMessage = std::format("Unsupported shell method: {}", method);
        return false;
    }
} // namespace

mongocxx::instance& MongoDBDatabase::getDriverInstance() {
    static mongocxx::instance instance{};
    return instance;
}

MongoDBDatabase::MongoDBDatabase(const DatabaseConnectionInfo& connInfo) {
    // Ensure driver is initialized
    getDriverInstance();

    this->connectionInfo = connInfo;
    if (connectionInfo.port == 0 || connectionInfo.port == 5432) {
        connectionInfo.port = 27017; // Default MongoDB port
    }
    spdlog::debug("Creating MongoDBDatabase with host = '{}', port = {}, showAllDatabases = {}",
                  connectionInfo.host, connectionInfo.port, connInfo.showAllDatabases);
}

MongoDBDatabase::~MongoDBDatabase() {
    databasesLoader.cancel();
    refreshWorkflow.cancel();

    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->collectionsLoader.cancel();
        }
    }

    disconnect();
}

MongoDBDatabaseNode* MongoDBDatabase::getDatabaseData(const std::string& dbName) {
    const auto it = databaseDataCache.find(dbName);
    if (it == databaseDataCache.end()) {
        auto newData = std::make_unique<MongoDBDatabaseNode>();
        newData->name = dbName;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[dbName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

std::pair<bool, std::string> MongoDBDatabase::connect() {
    if (connected) {
        return {true, ""};
    }

    setAttemptedConnection(true);
    auto [prepOk, prepErr] = prepareConnectionForConnect();
    if (!prepOk) {
        connected = false;
        setLastConnectionError(prepErr);
        return {false, prepErr};
    }

    try {
        std::string uri = connectionInfo.buildConnectionString();
        spdlog::debug("Connecting to MongoDB: {}", uri);

        {
            std::lock_guard lock(poolMutex);
            connectionPool = std::make_unique<mongocxx::pool>(mongocxx::uri{uri});
        }

        connected = true;
        setLastConnectionError("");

        db_version::fetchAndStoreServerVersion(*this);
        if (serverVersion_.empty()) {
            std::lock_guard lock(poolMutex);
            connectionPool.reset();
            connected = false;
            return {false, "MongoDB connection failed: unable to read server version"};
        }

        spdlog::debug("Successfully connected to MongoDB at {}:{} (version {})",
                      connectionInfo.host, connectionInfo.port, serverVersion_);

        // Start loading databases if showAllDatabases is enabled
        if (connectionInfo.showAllDatabases && !databasesLoaded && !databasesLoader.isRunning()) {
            spdlog::debug("Starting async database loading after connection...");
            refreshDatabaseNames();
        }

        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("MongoDB connection failed: {}", e.what());
        std::lock_guard lock(poolMutex);
        connectionPool.reset();
        connected = false;
        clearServerVersion();
        const bool legacy = connectionInfo.type == DatabaseType::MONGODB_LEGACY;
        std::string error =
            "MongoDB connection failed: " + friendlyMongoConnectError(e.what(), legacy);
        setLastConnectionError(error);
        return {false, error};
    }
}

void MongoDBDatabase::disconnect() {
    std::lock_guard lock(poolMutex);
    connectionPool.reset();
    stopSshTunnel();
    connected = false;
    clearServerVersion();
}

void MongoDBDatabase::refreshConnection() {
    refreshWorkflow.start([this]() -> bool {
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        auto [success, error] = connect();
        if (!success) {
            setLastConnectionError(error);
            return false;
        }

        if (connectionInfo.showAllDatabases) {
            spdlog::debug("Loading database names synchronously for refresh...");
            auto databases = getDatabaseNamesAsync();

            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames = std::move(databases);
        } else {
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames.clear();
        }

        spdlog::debug("MongoDB refresh workflow completed for {} databases",
                      databaseDataCache.size());
        return true;
    });
}

QueryResult MongoDBDatabase::executeQuery(const std::string& query, int rowLimit) {
    return executeQueryForDatabase(query, rowLimit, connectionInfo.database);
}

QueryResult MongoDBDatabase::executeQueryForDatabase(const std::string& query, int rowLimit,
                                                     const std::string& dbName) {
    QueryResult result;
    StatementResult s;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!connect().first) {
        s.success = false;
        s.errorMessage = "Not connected to database";
        result.statements.push_back(std::move(s));
        return result;
    }

    try {
        const std::string trimmedQuery = trimLiteral(query);
        if (auto shellCmd = tryParseMongoShell(trimmedQuery)) {
            auto client = getClient();
            auto db = (*client)[dbName];
            if (!executeMongoShellCommand(*shellCmd, db, s, rowLimit)) {
                result.statements.push_back(std::move(s));
                return result;
            }
        } else if (!trimmedQuery.empty() && trimmedQuery.front() == '{') {
        // Legacy JSON query format:
        // { "database": "db", "collection": "coll", "command": "find", "filter": {} }
        auto doc = bsoncxx::from_json(trimmedQuery);
        auto view = doc.view();

        std::string effectiveDbName = dbName;
        if (view["database"]) {
            effectiveDbName = std::string(view["database"].get_string().value);
        }
        std::string collName;
        std::string command = "find";

        if (view["collection"]) {
            collName = std::string(view["collection"].get_string().value);
        }
        if (view["command"]) {
            command = std::string(view["command"].get_string().value);
        }

        auto client = getClient();
        auto db = (*client)[effectiveDbName];

        if (command == "find" && !collName.empty()) {
            auto coll = db[collName];

            bsoncxx::document::view_or_value filter = bsoncxx::builder::stream::document{}
                                                      << bsoncxx::builder::stream::finalize;
            if (view["filter"]) {
                filter = view["filter"].get_document().value;
            }

            mongocxx::options::find opts;
            opts.limit(rowLimit);

            auto cursor = coll.find(filter, opts);
            appendFindRows(s, cursor);
        } else if (command == "aggregate" && !collName.empty()) {
            auto coll = db[collName];

            mongocxx::pipeline pipeline;
            if (view["pipeline"]) {
                for (auto&& stage : view["pipeline"].get_array().value) {
                    pipeline.append_stage(stage.get_document().value);
                }
            }

            auto cursor = coll.aggregate(pipeline);
            appendFindRows(s, cursor);
        } else if (command == "insert" && !collName.empty()) {
            auto coll = db[collName];
            if (view["document"]) {
                coll.insert_one(view["document"].get_document().value);
            } else if (view["documents"]) {
                std::vector<bsoncxx::document::view> docs;
                for (auto&& d : view["documents"].get_array().value) {
                    docs.push_back(d.get_document().value);
                }
                coll.insert_many(docs);
            }
            s.message = "Insert executed successfully";
        } else if (command == "update" && !collName.empty()) {
            auto coll = db[collName];
            auto filter = view["filter"].get_document().value;
            auto update = view["update"].get_document().value;
            auto updateResult = coll.update_many(filter, update);
            s.affectedRows = updateResult ? static_cast<int>(updateResult->modified_count()) : 0;
            s.message = std::format("Updated {} document{}", s.affectedRows,
                                    s.affectedRows == 1 ? "" : "s");
        } else if (command == "delete" && !collName.empty()) {
            auto coll = db[collName];
            auto filter = view["filter"].get_document().value;
            auto deleteResult = coll.delete_many(filter);
            s.affectedRows = deleteResult ? static_cast<int>(deleteResult->deleted_count()) : 0;
            s.message = std::format("Deleted {} document{}", s.affectedRows,
                                    s.affectedRows == 1 ? "" : "s");
        } else if (command == "createCollection" && !collName.empty()) {
            db.create_collection(collName);
            s.message = "Collection created successfully";
        } else if (command == "dropCollection" && !collName.empty()) {
            db[collName].drop();
            s.message = "Collection dropped successfully";
        } else if (command == "runCommand") {
            if (view["commandDoc"]) {
                auto cmdResult = db.run_command(view["commandDoc"].get_document().value);
                s.columnNames.push_back("result");
                std::vector<std::string> row;
                row.push_back(bsoncxx::to_json(cmdResult.view()));
                s.tableData.push_back(std::move(row));
                s.message = "Command executed successfully";
            } else {
                s.success = false;
                s.errorMessage = "runCommand requires commandDoc";
                result.statements.push_back(std::move(s));
                return result;
            }
        } else {
            s.success = false;
            s.errorMessage = "Unknown command or missing collection name";
            result.statements.push_back(std::move(s));
            return result;
        }
        } else {
            s.success = false;
            s.errorMessage =
                "Invalid MongoDB query. Use shell syntax, e.g. db.collection.find({})";
            result.statements.push_back(std::move(s));
            return result;
        }
    } catch (const std::exception& e) {
        s.success = false;
        s.errorMessage = e.what();
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    result.statements.push_back(std::move(s));

    return result;
}

std::unordered_map<std::string, std::unique_ptr<MongoDBDatabaseNode>>&
MongoDBDatabase::getDatabaseDataMap() {
    if (!databasesLoaded && !databasesLoader.isRunning() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void MongoDBDatabase::refreshDatabaseNames() {
    if (databasesLoader.isRunning()) {
        return;
    }

    databasesLoaded = false;
    databasesLoader.start([this]() { return getDatabaseNamesAsync(); });
}

bool MongoDBDatabase::isLoadingDatabases() const {
    return databasesLoader.isRunning();
}

bool MongoDBDatabase::hasPendingAsyncWork() const {
    if (isConnecting() || isLoadingDatabases()) {
        return true;
    }

    for (const auto& [_, dbNode] : databaseDataCache) {
        if (!dbNode) {
            continue;
        }

        if (dbNode->collectionsLoader.isRunning()) {
            return true;
        }
    }

    return false;
}

void MongoDBDatabase::checkDatabasesStatusAsync() {
    databasesLoader.check([this](const std::vector<std::string>& databases) {
        spdlog::debug("Async database loading completed. Found {} databases.", databases.size());

        for (const auto& dbName : databases) {
            getDatabaseData(dbName);
        }

        databasesLoaded = true;
    });
}

void MongoDBDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([this](const bool success) {
        if (success) {
            spdlog::debug("MongoDB refresh workflow completed successfully");
            std::vector<std::string> refreshedDatabases;
            {
                std::lock_guard lock(refreshStateMutex);
                refreshedDatabases = std::move(pendingRefreshDatabaseNames);
                pendingRefreshDatabaseNames.clear();
            }

            for (const auto& dbName : refreshedDatabases) {
                getDatabaseData(dbName);
            }

            databasesLoaded = true;

            // Trigger child refresh on the main thread to avoid data races
            for (auto& [_, dbDataPtr] : databaseDataCache) {
                if (dbDataPtr) {
                    dbDataPtr->startCollectionsLoadAsync(true);
                }
            }
        } else {
            spdlog::error("MongoDB refresh workflow failed");
        }
    });
}

std::vector<std::string> MongoDBDatabase::getDatabaseNamesAsync() const {
    spdlog::debug("MongoDBDatabase::getDatabaseNamesAsync");
    std::vector<std::string> result;

    try {
        if (!isConnected()) {
            spdlog::error("Cannot load databases: not connected");
            return result;
        }

        if (!connectionInfo.showAllDatabases) {
            if (!connectionInfo.database.empty()) {
                result.push_back(connectionInfo.database);
            }
            return result;
        }

        auto client = getClient();
        auto databases = client->list_database_names();

        for (const auto& dbName : databases) {
            result.push_back(dbName);
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to list databases: {}", e.what());
    }

    spdlog::debug("Found {} databases", result.size());
    return result;
}

std::pair<bool, std::string> MongoDBDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    try {
        auto client = getClient();
        (*client)[dbName].drop();

        databaseDataCache.erase(dbName);

        spdlog::debug("Database '{}' dropped successfully", dbName);
        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("Failed to drop database: {}", e.what());
        return {false, e.what()};
    }
}

mongocxx::pool::entry MongoDBDatabase::getClient() const {
    std::lock_guard lock(poolMutex);
    if (!connectionPool) {
        throw std::runtime_error("MongoDBDatabase::getClient: Connection pool not available");
    }
    return connectionPool->acquire();
}

std::string MongoDBDatabase::readServerVersion() const {
    try {
        const auto client = getClient();
        return fetchMongoServerVersion(*client, connectionInfo.database);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to read MongoDB server version: {}", e.what());
        return {};
    }
}
