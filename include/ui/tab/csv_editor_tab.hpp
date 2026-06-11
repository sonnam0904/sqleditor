#pragma once

#include "database/async_helper.hpp"
#include "ui/tab/tab.hpp"
#include "ui/table_renderer.hpp"
#include "ui/text_editor.hpp"
#include <memory>
#include <string>
#include <vector>

struct CsvLoadResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};

class CsvEditorTab final : public Tab {
public:
    CsvEditorTab(const std::string& name, std::string filePath);

    void render() override;
    [[nodiscard]] bool hasUnsavedChanges() const override;

    [[nodiscard]] const std::string& getFilePath() const {
        return filePath_;
    }

private:
    enum class ViewMode { Table, Raw };

    std::string filePath_;
    // parsed data
    std::vector<std::string> headers_;
    std::vector<std::vector<std::string>> rows_;

    // raw text editor (for raw view)
    sqleditor::TextEditor rawEditor_;
    bool rawPaletteSet_ = false;
    bool rawLastDark_ = true;

    ViewMode viewMode_ = ViewMode::Table;

    // edit state
    bool hasChanges_ = false;
    bool loadError_ = false;
    std::string errorMessage_;
    bool hasValidationError_ = false;
    std::string validationError_;

    // table renderer (table view)
    std::unique_ptr<TableRenderer> tableRenderer_;
    bool tableRendererDataDirty_ = false;
    int selectedRow_ = -1;
    int selectedCol_ = -1;

    // raw view dirty flag (raw buffer was edited)
    bool rawDirty_ = false;
    // raw editor needs to be repopulated from headers_/rows_ before next render
    bool rawNeedsSync_ = true;

    // async file load
    AsyncOperation<CsvLoadResult> loadOp_;
    bool isLoading_ = false;

    void startLoad();
    void applyLoadResult(CsvLoadResult result);
    void saveFile();

    void syncTableToRaw();
    bool syncRawToTable();

    void initTableRenderer();
    void syncTableRendererData();
    void clearValidationError();
    void setValidationError(std::string message);

    [[nodiscard]] bool hasPendingChanges() const;
    void renderToolbar();
    void renderTableView();
    void renderRawView();
};
