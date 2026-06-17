#include "database/mongodb_old.hpp"
#include "database/mongodb/mongo_bson_format.hpp"
#include "database/mongodb/mongo_shell.hpp"
#include "database/server_version.hpp"
#include <bsoncxx/json.hpp>
#include <chrono>
#include <cctype>
#include <format>
#include <nlohmann/json.hpp>
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

std::string shellToJson(std::string_view literal) {
    if (trimLiteral(literal).empty()) {
        return "{}";
    }
    return shellLiteralToExtendedJson(literal);
}

nlohmann::json parseShellJson(std::string_view literal) {
    return nlohmann::json::parse(shellToJson(literal));
}

void appendFindResult(StatementResult& s, const MongoLegacyClient::FindResult& findResult) {
    std::vector<bsoncxx::document::value> owned;
    owned.reserve(findResult.documentsJson.size());
    std::vector<bsoncxx::document::view> docs;
    docs.reserve(findResult.documentsJson.size());
    for (const auto& json : findResult.documentsJson) {
        owned.push_back(bsoncxx::from_json(json));
        docs.push_back(owned.back().view());
    }
    mongo_bson::appendDocumentsAsTable(s, docs);
    s.message = std::format("Found {} document{}", s.tableData.size(),
                            s.tableData.size() == 1 ? "" : "s");
}

int affectedRowsFromReply(const std::string& replyJson) {
    try {
        const auto reply = nlohmann::json::parse(replyJson);
        if (reply.contains("nModified")) {
            return reply["nModified"].get<int>();
        }
        if (reply.contains("n")) {
            return reply["n"].get<int>();
        }
    } catch (...) {
    }
    return 0;
}

bool runDbCommand(MongoLegacyClient& client, const std::string& dbName,
                  const nlohmann::json& command, std::string& error) {
    const auto reply = client.runCommandJson(dbName, command.dump(), error);
    if (!error.empty()) {
        return false;
    }
    (void)reply;
    return true;
}

bool runDbCommandWithReply(MongoLegacyClient& client, const std::string& dbName,
                           const nlohmann::json& command, std::string& error,
                           std::string& replyOut) {
    replyOut = client.runCommandJson(dbName, command.dump(), error);
    return error.empty();
}

std::string unquoteName(std::string name) {
    name = trimLiteral(name);
    if (name.size() >= 2 &&
        ((name.front() == '"' && name.back() == '"') ||
         (name.front() == '\'' && name.back() == '\''))) {
        return name.substr(1, name.size() - 2);
    }
    return name;
}

bool executeMongoShellCommand(const MongoShellCommand& cmd, const std::string& dbName,
                              MongoLegacyClient& client, StatementResult& s, int rowLimit) {
    const auto& method = cmd.method;
    std::string error;

    if (iequals(method, "createCollection")) {
        if (cmd.args.empty()) {
            s.success = false;
            s.errorMessage = "createCollection requires a collection name";
            return false;
        }
        const std::string collName = unquoteName(cmd.args.front());
        if (!runDbCommand(client, dbName, nlohmann::json{{"create", collName}}, error)) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        s.message = "Collection created successfully";
        return true;
    }

    if (iequals(method, "runCommand")) {
        if (cmd.args.empty()) {
            s.success = false;
            s.errorMessage = "runCommand requires a document argument";
            return false;
        }
        std::string reply;
        if (!runDbCommandWithReply(client, dbName, parseShellJson(cmd.args.front()), error,
                                   reply)) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        s.columnNames.push_back("result");
        s.tableData.push_back({reply});
        s.message = "Command executed successfully";
        return true;
    }

    if (cmd.collection.empty()) {
        s.success = false;
        s.errorMessage = std::format("Unknown db command: db.{}()", method);
        return false;
    }

    const std::string& collection = cmd.collection;
    const int aggregateLimit = cmd.limit >= 0 ? cmd.limit : rowLimit;

    if (iequals(method, "find") || iequals(method, "findOne")) {
        const std::string filterJson = shellToJson(cmd.args.empty() ? "{}" : cmd.args[0]);
        const int limit = iequals(method, "findOne")
                              ? 1
                              : (cmd.limit >= 0 ? cmd.limit : kDefaultMongoFindLimit);
        const std::string sortJson = cmd.sort.empty() ? "" : shellToJson(cmd.sort);
        const auto findResult =
            client.find(dbName, collection, filterJson, limit, cmd.skip, sortJson, error);
        if (!error.empty()) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        appendFindResult(s, findResult);
        return true;
    }

    if (iequals(method, "aggregate")) {
        const std::string pipelineJson = shellToJson(cmd.args.empty() ? "[]" : cmd.args[0]);
        const auto findResult =
            client.aggregate(dbName, collection, pipelineJson, aggregateLimit, error);
        if (!error.empty()) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        appendFindResult(s, findResult);
        return true;
    }

    if (iequals(method, "insertOne")) {
        if (cmd.args.empty()) {
            s.success = false;
            s.errorMessage = "insertOne requires a document argument";
            return false;
        }
        nlohmann::json command;
        command["insert"] = collection;
        command["documents"] = nlohmann::json::array({parseShellJson(cmd.args.front())});
        if (!runDbCommand(client, dbName, command, error)) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        s.message = "Insert executed successfully";
        return true;
    }

    if (iequals(method, "insertMany")) {
        if (cmd.args.empty()) {
            s.success = false;
            s.errorMessage = "insertMany requires an array argument";
            return false;
        }
        const auto docs = parseShellJson(cmd.args.front());
        if (!docs.is_array()) {
            s.success = false;
            s.errorMessage = "insertMany requires an array argument";
            return false;
        }
        nlohmann::json command;
        command["insert"] = collection;
        command["documents"] = docs;
        if (!runDbCommand(client, dbName, command, error)) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        s.message = std::format("Inserted {} document{}", docs.size(), docs.size() == 1 ? "" : "s");
        return true;
    }

    if (iequals(method, "updateOne") || iequals(method, "updateMany")) {
        if (cmd.args.size() < 2) {
            s.success = false;
            s.errorMessage = std::format("{} requires filter and update arguments", method);
            return false;
        }
        nlohmann::json command;
        command["update"] = collection;
        command["updates"] = nlohmann::json::array({nlohmann::json{
            {"q", parseShellJson(cmd.args[0])},
            {"u", parseShellJson(cmd.args[1])},
            {"multi", iequals(method, "updateMany")}}});
        std::string reply;
        if (!runDbCommandWithReply(client, dbName, command, error, reply)) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        s.affectedRows = affectedRowsFromReply(reply);
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
        nlohmann::json command;
        command["delete"] = collection;
        command["deletes"] = nlohmann::json::array({nlohmann::json{
            {"q", parseShellJson(cmd.args[0])},
            {"limit", iequals(method, "deleteOne") ? 1 : 0}}});
        std::string reply;
        if (!runDbCommandWithReply(client, dbName, command, error, reply)) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        s.affectedRows = affectedRowsFromReply(reply);
        s.message = std::format("Deleted {} document{}", s.affectedRows,
                                s.affectedRows == 1 ? "" : "s");
        return true;
    }

    if (iequals(method, "count") || iequals(method, "countDocuments")) {
        const std::string filterJson = shellToJson(cmd.args.empty() ? "{}" : cmd.args[0]);
        const int64_t count = client.count(dbName, collection, filterJson, error);
        if (!error.empty()) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        s.columnNames.push_back("count");
        s.tableData.push_back({std::to_string(count)});
        s.message = std::format("Count: {}", count);
        return true;
    }

    if (iequals(method, "drop")) {
        if (!runDbCommand(client, dbName, nlohmann::json{{"drop", collection}}, error)) {
            s.success = false;
            s.errorMessage = error;
            return false;
        }
        s.message = "Collection dropped successfully";
        return true;
    }

    s.success = false;
    s.errorMessage = std::format("Unsupported shell method: {}", method);
    return false;
}

} // namespace

MongoDBOldDatabase::MongoDBOldDatabase(const DatabaseConnectionInfo& connInfo) {
    connectionInfo = connInfo;
    if (connectionInfo.port == 0 || connectionInfo.port == 5432) {
        connectionInfo.port = 27017;
    }
}

MongoDBOldDatabase::~MongoDBOldDatabase() {
    databasesLoader.cancel();
    refreshWorkflow.cancel();

    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->collectionsLoader.cancel();
        }
    }

    disconnect();
}

MongoDBOldDatabaseNode* MongoDBOldDatabase::getDatabaseData(const std::string& dbName) {
    const auto it = databaseDataCache.find(dbName);
    if (it == databaseDataCache.end()) {
        auto newData = std::make_unique<MongoDBOldDatabaseNode>();
        newData->name = dbName;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[dbName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

std::pair<bool, std::string> MongoDBOldDatabase::connect() {
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

    const std::string uri = connectionInfo.buildConnectionString();
    spdlog::debug("Connecting to MongoDB Old: {}", uri);

    std::string error;
    if (!legacyClient_.connect(uri, error)) {
        connected = false;
        clearServerVersion();
        const std::string msg = "MongoDB Old connection failed: " + error;
        setLastConnectionError(msg);
        return {false, msg};
    }

    connected = true;
    setLastConnectionError("");
    db_version::fetchAndStoreServerVersion(*this);

    if (serverVersion_.empty()) {
        legacyClient_.shutdown();
        connected = false;
        return {false, "MongoDB Old connection failed: unable to read server version"};
    }

    if (connectionInfo.showAllDatabases && !databasesLoaded && !databasesLoader.isRunning()) {
        refreshDatabaseNames();
    }

    return {true, ""};
}

void MongoDBOldDatabase::disconnect() {
    legacyClient_.shutdown();
    stopSshTunnel();
    connected = false;
    clearServerVersion();
    databasesLoaded = false;
}

void MongoDBOldDatabase::refreshConnection() {
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
            auto databases = getDatabaseNamesAsync();
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames = std::move(databases);
        } else {
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames.clear();
        }
        return true;
    });
}

QueryResult MongoDBOldDatabase::executeQuery(const std::string& query, int rowLimit) {
    return executeQueryForDatabase(query, rowLimit, connectionInfo.database);
}

QueryResult MongoDBOldDatabase::executeQueryForDatabase(const std::string& query, int rowLimit,
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

    const std::string trimmedQuery = trimLiteral(query);
    try {
        if (auto shellCmd = tryParseMongoShell(trimmedQuery)) {
            if (!executeMongoShellCommand(*shellCmd, dbName, legacyClient_, s, rowLimit)) {
                result.statements.push_back(std::move(s));
                return result;
            }
        } else if (!trimmedQuery.empty() && trimmedQuery.front() == '{') {
        const auto doc = nlohmann::json::parse(trimmedQuery);
        std::string effectiveDbName = dbName;
        if (doc.contains("database") && doc["database"].is_string()) {
            effectiveDbName = doc["database"].get<std::string>();
        }

        const std::string collName =
            doc.contains("collection") && doc["collection"].is_string()
                ? doc["collection"].get<std::string>()
                : "";
        const std::string command =
            doc.contains("command") && doc["command"].is_string() ? doc["command"].get<std::string>()
                                                                  : "find";

        std::string error;
        if (command == "find" && !collName.empty()) {
            const std::string filterJson =
                doc.contains("filter") ? doc["filter"].dump() : "{}";
            const int limit = doc.value("limit", kDefaultMongoFindLimit);
            const int skip = doc.value("skip", 0);
            const std::string sortJson =
                doc.contains("sort") ? doc["sort"].dump() : "";

            const auto findResult = legacyClient_.find(effectiveDbName, collName, filterJson,
                                                       limit, skip, sortJson, error);
            if (!error.empty()) {
                s.success = false;
                s.errorMessage = error;
            } else {
                appendFindResult(s, findResult);
            }
        } else if (command == "aggregate" && !collName.empty()) {
            const std::string pipelineJson =
                doc.contains("pipeline") ? doc["pipeline"].dump() : "[]";
            const int limit = doc.value("limit", rowLimit);
            const auto findResult = legacyClient_.aggregate(effectiveDbName, collName,
                                                            pipelineJson, limit, error);
            if (!error.empty()) {
                s.success = false;
                s.errorMessage = error;
            } else {
                appendFindResult(s, findResult);
            }
        } else if (command == "count" && !collName.empty()) {
            const std::string filterJson =
                doc.contains("filter") ? doc["filter"].dump() : "{}";
            const int64_t count = legacyClient_.count(effectiveDbName, collName, filterJson, error);
            if (!error.empty()) {
                s.success = false;
                s.errorMessage = error;
            } else {
                s.columnNames.push_back("count");
                s.tableData.push_back({std::to_string(count)});
                s.message = std::format("Count: {}", count);
            }
        } else if (command == "dropCollection" && !collName.empty()) {
            const auto cmd = std::format(R"({{"drop": "{}"}})", collName);
            const auto reply =
                legacyClient_.runCommandJson(effectiveDbName, cmd, error);
            if (!error.empty()) {
                s.success = false;
                s.errorMessage = error;
            } else {
                s.message = "Collection dropped successfully";
                (void)reply;
            }
        } else if (command == "runCommand") {
            if (!doc.contains("commandDoc")) {
                s.success = false;
                s.errorMessage = "runCommand requires commandDoc";
            } else {
                const auto reply = legacyClient_.runCommandJson(
                    effectiveDbName, doc["commandDoc"].dump(), error);
                if (!error.empty()) {
                    s.success = false;
                    s.errorMessage = error;
                } else {
                    s.columnNames.push_back("result");
                    s.tableData.push_back({reply});
                    s.message = "Command executed successfully";
                }
            }
        } else if (command == "dropDatabase") {
            const auto cmd = R"({"dropDatabase": 1})";
            const auto reply = legacyClient_.runCommandJson(effectiveDbName, cmd, error);
            if (!error.empty()) {
                s.success = false;
                s.errorMessage = error;
            } else {
                databaseDataCache.erase(effectiveDbName);
                s.message = "Database dropped successfully";
                (void)reply;
            }
        } else {
            s.success = false;
            s.errorMessage = "Unsupported MongoDB Old command: " + command;
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

std::unordered_map<std::string, std::unique_ptr<MongoDBOldDatabaseNode>>&
MongoDBOldDatabase::getDatabaseDataMap() {
    if (!databasesLoaded && !databasesLoader.isRunning() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void MongoDBOldDatabase::refreshDatabaseNames() {
    if (databasesLoader.isRunning()) {
        return;
    }
    databasesLoaded = false;
    databasesLoader.start([this]() { return getDatabaseNamesAsync(); });
}

bool MongoDBOldDatabase::isLoadingDatabases() const {
    return databasesLoader.isRunning();
}

bool MongoDBOldDatabase::hasPendingAsyncWork() const {
    if (isConnecting() || isLoadingDatabases()) {
        return true;
    }
    for (const auto& [_, dbNode] : databaseDataCache) {
        if (dbNode && dbNode->collectionsLoader.isRunning()) {
            return true;
        }
    }
    return false;
}

void MongoDBOldDatabase::checkDatabasesStatusAsync() {
    databasesLoader.check([this](const std::vector<std::string>& databases) {
        for (const auto& dbName : databases) {
            getDatabaseData(dbName);
        }
        databasesLoaded = true;
    });
}

void MongoDBOldDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([this](const bool success) {
        if (!success) {
            return;
        }
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
        for (auto& [_, dbDataPtr] : databaseDataCache) {
            if (dbDataPtr) {
                dbDataPtr->startTablesLoadAsync(true);
            }
        }
    });
}

std::vector<std::string> MongoDBOldDatabase::getDatabaseNamesAsync() const {
    std::vector<std::string> result;
    if (!isConnected()) {
        return result;
    }

    if (!connectionInfo.showAllDatabases) {
        if (!connectionInfo.database.empty()) {
            result.push_back(connectionInfo.database);
        }
        return result;
    }

    std::string error;
    result = legacyClient_.listDatabases(error);
    if (!error.empty()) {
        spdlog::error("Failed to list databases (MongoDB Old): {}", error);
        result.clear();
    }
    return result;
}

std::pair<bool, std::string> MongoDBOldDatabase::dropDatabase(const std::string& dbName) {
    const auto query =
        std::format(R"({{"database": "{}", "command": "dropDatabase"}})", dbName);
    auto r = executeQuery(query);
    if (r.success()) {
        databaseDataCache.erase(dbName);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::string MongoDBOldDatabase::readServerVersion() const {
    if (!isConnected()) {
        return {};
    }
    std::string error;
    std::string version = legacyClient_.buildInfo(
        connectionInfo.database.empty() ? "admin" : connectionInfo.database, error);
    if (!error.empty()) {
        version = legacyClient_.buildInfo("admin", error);
    }
    return version;
}
