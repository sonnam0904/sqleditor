#pragma once

#include "async_helper.hpp"
#include "db_interface.hpp"
#include "mongodb_old/mongodb_old_database_node.hpp"
#include "mongodb_old/mongo_legacy_client.hpp"
#include "query_executor.hpp"
#include <mutex>
#include <unordered_map>
#include <vector>

class MongoDBOldDatabase final : public DatabaseInterface, public IQueryExecutor {
    friend class MongoDBOldDatabaseNode;

public:
    explicit MongoDBOldDatabase(const DatabaseConnectionInfo& connInfo);
    ~MongoDBOldDatabase() override;

    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    std::pair<bool, std::string> dropDatabase(const std::string& dbName) override;

    QueryResult executeQuery(const std::string& query, int rowLimit = 1000) override;
    QueryResult executeQueryForDatabase(const std::string& query, int rowLimit,
                                        const std::string& dbName);

    void refreshDatabaseNames();

    bool isConnecting() const override {
        return connectionOp.isRunning() || refreshWorkflow.isRunning();
    }

    bool areDatabasesLoaded() const {
        return databasesLoaded;
    }
    bool isLoadingDatabases() const;
    void checkDatabasesStatusAsync();
    void checkRefreshWorkflowAsync();

    [[nodiscard]] bool hasPendingAsyncWork() const override;

protected:
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    mutable MongoLegacyClient legacyClient_;
    std::unordered_map<std::string, std::unique_ptr<MongoDBOldDatabaseNode>> databaseDataCache;
    bool databasesLoaded = false;
    std::vector<std::string> pendingRefreshDatabaseNames;
    mutable std::mutex refreshStateMutex;

    AsyncOperation<std::vector<std::string>> databasesLoader;
    AsyncOperation<bool> refreshWorkflow;

public:
    MongoDBOldDatabaseNode* getDatabaseData(const std::string& dbName);

    std::unordered_map<std::string, std::unique_ptr<MongoDBOldDatabaseNode>>& getDatabaseDataMap();
    const std::unordered_map<std::string, std::unique_ptr<MongoDBOldDatabaseNode>>&
    getDatabaseDataMap() const {
        return databaseDataCache;
    }

    MongoLegacyClient& getLegacyClient() {
        return legacyClient_;
    }
    const MongoLegacyClient& getLegacyClient() const {
        return legacyClient_;
    }

    [[nodiscard]] std::string readServerVersion() const;
};
