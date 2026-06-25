#pragma once

#include "database/async_helper.hpp"
#include "database/db_interface.hpp"
#include "ui/auto_complete_input.hpp"
#include "ui/tab/tab.hpp"
#include "ui/table_renderer.hpp"
#include "ui/text_editor.hpp"
#include "themes.hpp"
#include "utils/table_exporter.hpp"
#include <memory>
#include <string>
#include <vector>

// Forward declarations
class IDatabaseNode;

enum class MongoCollectionViewMode { Table, Json };

class TableViewerTab final : public Tab {
public:
    TableViewerTab(const std::string& name, std::string databasePath, Table table,
                   IDatabaseNode* node);

    void render() override;

    // Table Viewer specific methods
    [[nodiscard]] const std::string& getDatabasePath() const {
        return databasePath;
    }
    [[nodiscard]] const Table& getTable() const {
        return table_;
    }
    [[nodiscard]] IDatabaseNode* getDatabaseNode() const {
        return node_;
    }
    void loadDataAsync();
    void checkAsyncLoadStatus();
    void nextPage();
    void previousPage();
    void firstPage();
    void lastPage();
    void updateTableMetadata(const Table& table);
    void syncTableMetadataFromNode();
    void refreshData();
    void saveChanges();
    void cancelChanges();
    void addRow();
    void deleteRow(int row);
    void duplicateRow(int row);

    // SQL generation and confirmation dialog
    std::vector<std::string> generateUpdateSQL();
    [[nodiscard]] std::vector<std::string> generateMongoShellCommands() const;
    [[nodiscard]] std::vector<std::string> getPrimaryKeyColumns() const;
    void showSaveConfirmationDialog();
    void checkSQLExecutionStatus();

private:
    std::string databasePath;
    Table table_;
    IDatabaseNode* node_ = nullptr;
    std::vector<std::vector<std::string>> tableData;
    std::vector<std::vector<std::string>> originalData;
    std::vector<std::vector<bool>> editedCells;
    std::vector<bool> isNewRow;
    struct DeletedRow {
        int index = 0;
        std::vector<std::string> values;
    };
    std::vector<DeletedRow> deletedRows;
    bool initialSelectionDone = false;
    int currentPage = 0;
    int rowsPerPage = 100;
    int totalRows = 0;
    bool mongoTotalRowsKnown = false;

    // Async loading state
    bool hasLoadingError = false;
    std::string loadingError;
    AsyncOperation<bool> dataLoadOp;
    AsyncOperation<int> mongoCountOp;

    // Edit state
    int selectedRow = -1;
    int selectedCol = -1;
    bool hasChanges = false;

    // Save confirmation dialog state
    bool showSaveDialog = false;
    bool dialogOpened = false;
    std::vector<std::string> pendingUpdateSQL;
    sqleditor::TextEditor saveDialogEditor_;

    // Async SQL execution state
    AsyncOperation<std::pair<bool, std::string>> sqlExecutionOp;

    // Table renderer
    std::unique_ptr<TableRenderer> tableRenderer;

    // Filter functionality
    char filterBuffer[512] = {0};
    std::string currentFilter;
    bool filterChanged = false;
    bool filterParseError = false;
    std::unique_ptr<AutoCompleteInput> filterAutoComplete;

    // Sorting state
    int sortColumn = -1;
    std::string sortColumnName;
    SortDirection sortDirection = SortDirection::None;

    // Right panel state
    bool rightPanelOpen = false;
    float rightPanelWidth = 300.0f;
    int activeRightPanelTab = 0; // 0 = Value, 1 = Metadata
    char valuePanelBuffer[4096] = {0};
    bool valuePanelBufferDirty = false;
    int lastSyncedRow = -1;
    int lastSyncedCol = -1;
    std::string metadataFilter;

    // MongoDB collection viewer
    MongoCollectionViewMode mongoViewMode_ = MongoCollectionViewMode::Table;
    std::vector<std::string> mongoDocumentJson_;

    [[nodiscard]] bool isMongoCollection() const;

    // Helper methods
    void initializeTableRenderer();
    void selectCell(int row, int col);
    void applyFilter();
    void initializeFilterAutoComplete();
    void exportFilteredData(ExportFormat format);
    [[nodiscard]] std::string buildOrderByClause() const;
    [[nodiscard]] std::string buildTableRef() const;
    [[nodiscard]] std::string buildRowWhere(const std::vector<std::string>& rowValues) const;
    [[nodiscard]] bool hasPendingChanges() const;

    // Right panel methods
    void renderRightPanelToggleStrip(float stripWidth, float availableHeight);
    void renderRightPanel(float panelWidth, float availableHeight);
    void renderValueTab();
    void renderMetadataTab();
    void renderMongoJsonView(float width, float height);
    void syncValuePanelBuffer();
    void renderToolbar(const Theme::Colors& colors);
    void renderPaginationBar(const Theme::Colors& colors);
    void startMongoTotalCount();
    [[nodiscard]] bool mongoHasMorePages() const;
    [[nodiscard]] bool canGoToNextPage() const;
    [[nodiscard]] bool canGoToLastPage() const;
};
