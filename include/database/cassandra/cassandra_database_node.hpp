#pragma once

#include "database/async_helper.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "database/table_data_provider.hpp"
#include <map>
#include <string>
#include <vector>

class CassandraDatabase;

/**
 * @brief Per-keyspace data for Cassandra.
 *
 * Cassandra hierarchy: Cluster -> Keyspace -> Tables / Materialized Views.
 * One CassandraDatabaseNode per keyspace, mirroring the MongoDB/MySQL layout.
 */
class CassandraDatabaseNode : public IDatabaseNode, public ITableDataProvider {
public:
    CassandraDatabase* parentDb = nullptr;

    std::string name; // keyspace name

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;

    std::vector<Table> tables;
    std::vector<Table> views; // materialized views

    bool tablesLoaded = false;
    bool viewsLoaded = false;

    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    std::string lastTablesError;
    std::string lastViewsError;

    bool expanded = false;
    bool tablesExpanded = false;
    bool viewsExpanded = false;

    // ========== IDatabaseNode ==========

    [[nodiscard]] std::string getName() const override {
        return name;
    }

    [[nodiscard]] std::string getFullPath() const override;

    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::CASSANDRA;
    }

    QueryResult executeQuery(const std::string& sql, int limit = 1000) override;

    std::vector<Table>& getTables() override {
        return tables;
    }
    [[nodiscard]] const std::vector<Table>& getTables() const override {
        return tables;
    }

    std::vector<Table>& getViews() override {
        return views;
    }
    [[nodiscard]] const std::vector<Table>& getViews() const override {
        return views;
    }

    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause = "",
                                                       const std::string& orderBy = "") override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause = "") override;

    [[nodiscard]] bool isTablesLoaded() const override {
        return tablesLoaded;
    }
    [[nodiscard]] bool isViewsLoaded() const override {
        return viewsLoaded;
    }
    [[nodiscard]] bool isLoadingTables() const override {
        return tablesLoader.isRunning();
    }
    [[nodiscard]] bool isLoadingViews() const override {
        return viewsLoader.isRunning();
    }

    void startTablesLoadAsync(bool force = false) override;
    void startViewsLoadAsync(bool force = false) override;
    void checkLoadingStatus() override;

    [[nodiscard]] const std::string& getLastTablesError() const override {
        return lastTablesError;
    }
    [[nodiscard]] const std::string& getLastViewsError() const override {
        return lastViewsError;
    }

    void startTableRefreshAsync(const std::string& tableName) override;
    [[nodiscard]] bool isTableRefreshing(const std::string& tableName) const override;
    void checkTableRefreshStatusAsync(const std::string& tableName) override;

    // ========== Cassandra-specific ==========

    std::vector<Table> loadTablesSync();
    std::vector<Table> loadViewsSync();
    std::vector<Column> loadColumns(const std::string& tableName);
    Table refreshTableSync(const std::string& tableName);

    std::pair<bool, std::string> dropTable(const std::string& tableName);

    std::pair<bool, std::string> createTable(const Table& table) override;
};
