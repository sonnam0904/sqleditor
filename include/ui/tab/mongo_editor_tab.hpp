#pragma once

#include "app_state.hpp"
#include "database/async_helper.hpp"
#include "database/db.hpp"
#include "ui/tab/tab.hpp"
#include "ui/text_editor.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "database/database_node.hpp"
class AIChatState;
class AIChatPanel;

enum class MongoResultViewMode { Table, Json };

class MongoEditorTab final : public Tab {
public:
    explicit MongoEditorTab(const std::string& name, IDatabaseNode* node);
    ~MongoEditorTab() override;

    void render() override;

    [[nodiscard]] IDatabaseNode* getDatabaseNode() const {
        return node_;
    }

    void loadFromScript(const SqlScript& script);
    [[nodiscard]] const std::string& getFilePath() const {
        return filePath_;
    }
    [[nodiscard]] int getScriptId() const {
        return scriptId_;
    }
    [[nodiscard]] bool hasUnsavedChanges() const override {
        return contentModified_;
    }

private:
    static constexpr const char* kScriptExtension = ".mongo";

    std::string query_;
    IDatabaseNode* node_ = nullptr;
    sqleditor::TextEditor editor_;

    // query result
    QueryResult queryResult_;
    std::string queryError_;
    std::chrono::milliseconds lastQueryDuration_{0};

    // async execution
    AsyncOperation<QueryResult> queryExecutionOp_;

    // splitter
    float splitterPosition_ = 0.4f;
    float totalContentHeight_ = 0.0f;
    int pendingEditorFocusFrames_ = 3;
    MongoResultViewMode resultViewMode_ = MongoResultViewMode::Table;

    // script file management
    int scriptId_ = 0;
    std::string filePath_;
    std::string scriptName_;
    bool contentModified_ = false;
    bool renamingScript_ = false;
    bool renamingFocusNeeded_ = false;
    char renameBuffer_[256] = {};

    void startQueryExecutionAsync(const std::string& query);
    void checkQueryExecutionStatus();
    void cancelQueryExecution();

    void renderConnectionInfo();
    void renderDatabaseCombo(const std::string& host, const char* label, const std::string& currentName,
                             const std::vector<std::string>& dbNames,
                             const std::function<void(const std::string&)>& onSelect);
    void switchNode(IDatabaseNode* newNode);
    void renderScriptHeader();
    void renderToolbar();
    void renderQueryResults();
    void renderSingleResult(const StatementResult& r, size_t index);
    void renderResultViewToggle(bool hasJsonDocuments);
    bool resultHasJsonDocuments(const StatementResult& r) const;

    void formatQuery();

    void updateCompletionKeywords();
    bool completionKeywordsSet_ = false;
    size_t lastCompletionCollectionCount_ = 0;

    static std::string getDefaultScriptsDir();
    void saveScript();
    void persistScriptToAppState();

    // AI panel
    std::unique_ptr<AIChatState> aiChatState_;
    std::unique_ptr<AIChatPanel> aiChatPanel_;
    bool aiPanelVisible_ = false;
    float aiPanelWidth_ = 350.0f;

    void initAIPanel();
    void renderAIToggleStrip(float stripWidth, float availableHeight);
    void renderAIPanel(float panelWidth, float availableHeight);
};
