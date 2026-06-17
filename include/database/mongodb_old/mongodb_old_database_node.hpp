#pragma once

#include "database/async_helper.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "database/table_data_provider.hpp"
#include <map>
#include <string>
#include <vector>

class MongoDBOldDatabase;

class MongoDBOldDatabaseNode : public IDatabaseNode, public ITableDataProvider {
public:
    MongoDBOldDatabase* parentDb = nullptr;

    std::string name;

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;

    std::vector<Table> collections;
    std::vector<Table> views;

    bool collectionsLoaded = false;
    bool viewsLoaded = false;

    AsyncOperation<std::vector<Table>> collectionsLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    std::map<std::string, AsyncOperation<Table>> collectionRefreshLoaders;

    std::string lastCollectionsError;
    std::string lastViewsError;

    bool expanded = false;
    bool collectionsExpanded = false;
    bool viewsExpanded = false;

    [[nodiscard]] std::string getName() const override {
        return name;
    }

    [[nodiscard]] std::string getFullPath() const override;

    [[nodiscard]] DatabaseType getDatabaseType() const override;

    QueryResult executeQuery(const std::string& sql, int limit = 1000) override;

    std::vector<Table>& getTables() override {
        return collections;
    }
    [[nodiscard]] const std::vector<Table>& getTables() const override {
        return collections;
    }

    std::vector<Table>& getViews() override {
        return views;
    }
    [[nodiscard]] const std::vector<Table>& getViews() const override {
        return views;
    }

    std::vector<std::vector<std::string>> getTableData(const Table& collection, int limit,
                                                       int offset, const std::string& filter = "",
                                                       const std::string& sort = "") override;

    [[nodiscard]] std::vector<std::string> getColumnNames(const Table& table) override;

    [[nodiscard]] int getRowCount(const Table& table,
                                  const std::string& whereClause = "") override;
    [[nodiscard]] int getEstimatedDocumentCount(const Table& collection);

    [[nodiscard]] bool isTablesLoaded() const override {
        return collectionsLoaded;
    }

    [[nodiscard]] bool isViewsLoaded() const override {
        return viewsLoaded;
    }

    [[nodiscard]] bool isLoadingTables() const override {
        return collectionsLoader.isRunning();
    }

    [[nodiscard]] bool isLoadingViews() const override {
        return viewsLoader.isRunning();
    }

    void startTablesLoadAsync(bool force = false) override;
    void startViewsLoadAsync(bool force = false) override;
    void checkLoadingStatus() override;

    [[nodiscard]] const std::string& getLastTablesError() const override {
        return lastCollectionsError;
    }

    void startTableRefreshAsync(const std::string& tableName) override;
    [[nodiscard]] bool isTableRefreshing(const std::string& tableName) const override;
    void checkTableRefreshStatusAsync(const std::string& tableName) override;

    std::vector<std::string> getCollectionDocumentsAsJson(const Table& collection, int limit,
                                                          int offset, const std::string& filter,
                                                          const std::string& sort);

    std::pair<bool, std::string> dropCollection(const std::string& collectionName);

private:
    void startCollectionsLoadAsync(bool force);
    void checkCollectionsStatusAsync();
    std::vector<Table> getCollectionsAsync();
    std::vector<Column> inferSchemaFromSample(const std::string& collectionName, int sampleSize);
    Table refreshCollectionAsync(const std::string& collectionName);
};
