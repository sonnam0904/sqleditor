#include "ui/tab/mongo_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "ai/ai_chat.hpp"
#include "application.hpp"
#include "database/mongodb/mongodb_database_node.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/ai_chat_panel.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/json_tree_view.hpp"
#include "ui/table_renderer.hpp"
#include "utils/sentry_utils.hpp"
#include "utils/spinner.hpp"
#include "utils/splitter.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

namespace {
    constexpr const char* LABEL_RUNNING_QUERY = "Running query...";
    constexpr const char* LABEL_CANCEL = "Cancel";
    constexpr const char* LABEL_NO_ROWS = "No rows returned.";
    constexpr const char* LABEL_ROW_LIMIT = "(limited to 1000 rows)";
    constexpr const char* LABEL_NO_RESULTS =
        "No results to display. Execute a query to see results here.";
    constexpr int MAX_QUERY_ROWS = 1000;

    using CI = sqleditor::TextEditor::CompletionItem;
    using CK = sqleditor::TextEditor::CompletionKind;
    using CompletionRequest = sqleditor::TextEditor::CompletionRequest;

    std::string toLowerCopy(std::string_view s) {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    }

    std::string getLinePrefix(const CompletionRequest& request) {
        int lineStart = request.cursorIndex;
        while (lineStart > 0) {
            const char ch = request.content[static_cast<size_t>(lineStart - 1)];
            if (ch == '\n' || ch == '\r') {
                break;
            }
            --lineStart;
        }
        return std::string(request.content.substr(static_cast<size_t>(lineStart),
                                                  static_cast<size_t>(request.cursorIndex -
                                                                        lineStart)));
    }

    std::string lineWithoutCurrentWord(const std::string& linePrefix, std::string_view currentWord) {
        if (currentWord.empty() || linePrefix.size() < currentWord.size()) {
            return linePrefix;
        }
        return linePrefix.substr(0, linePrefix.size() - currentWord.size());
    }

    enum class MongoShellCompletionContext {
        Start,
        AfterDb,
        AfterCollection,
        AfterMethodChain,
        InDocument,
        Generic,
    };

    MongoShellCompletionContext detectMongoShellContext(const std::string& linePrefix,
                                                        std::string_view currentWord) {
        std::string line = lineWithoutCurrentWord(linePrefix, currentWord);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }

        if (line.empty()) {
            return MongoShellCompletionContext::Start;
        }

        int braceDepth = 0;
        for (char ch : line) {
            if (ch == '{') {
                ++braceDepth;
            } else if (ch == '}') {
                --braceDepth;
            }
        }
        if (braceDepth > 0) {
            return MongoShellCompletionContext::InDocument;
        }

        if (line.rfind("db.", 0) != 0) {
            return MongoShellCompletionContext::Generic;
        }

        const std::string rest = line.substr(3);
        if (rest.empty()) {
            return MongoShellCompletionContext::AfterDb;
        }

        const size_t firstDot = rest.find('.');
        if (firstDot == std::string::npos) {
            return MongoShellCompletionContext::AfterDb;
        }

        const std::string afterCollection = rest.substr(firstDot + 1);
        if (afterCollection.empty()) {
            return MongoShellCompletionContext::AfterCollection;
        }

        if (line.find('(') != std::string::npos) {
            const size_t lastDot = line.rfind('.');
            if (lastDot != std::string::npos && lastDot + 1 < line.size()) {
                const std::string tail = line.substr(lastDot + 1);
                if (tail.find('(') == std::string::npos) {
                    return MongoShellCompletionContext::AfterMethodChain;
                }
            }
        }

        if (afterCollection.find('(') == std::string::npos) {
            return MongoShellCompletionContext::AfterCollection;
        }

        return MongoShellCompletionContext::AfterMethodChain;
    }

    std::optional<std::string> extractCollectionName(std::string_view line) {
        constexpr std::string_view prefix = "db.";
        const size_t pos = line.find(prefix);
        if (pos == std::string_view::npos) {
            return std::nullopt;
        }

        size_t i = pos + prefix.size();
        std::string name;
        while (i < line.size()) {
            const char c = line[i];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
                name.push_back(c);
                ++i;
            } else {
                break;
            }
        }
        if (name.empty()) {
            return std::nullopt;
        }
        return name;
    }

    CI makeSnippet(std::string label, std::string insert, std::string detail, CK kind) {
        CI item;
        item.text = std::move(label);
        item.insertText = std::move(insert);
        item.matchText = item.text;
        item.detailText = std::move(detail);
        item.kind = kind;
        return item;
    }

    void appendIfMatches(std::vector<CI>& out, CI item, std::string_view currentWord) {
        const std::string lowerWord = toLowerCopy(currentWord);
        const std::string lowerMatch = toLowerCopy(item.matchText);
        if (!lowerWord.empty() && lowerMatch.find(lowerWord) != 0) {
            return;
        }
        out.push_back(std::move(item));
    }

    std::vector<CI> buildContextualCompletions(MongoShellCompletionContext ctx,
                                               const std::vector<Table>& collections,
                                               std::string_view currentWord,
                                               const std::string& linePrefix) {
        std::vector<CI> items;

        switch (ctx) {
        case MongoShellCompletionContext::Start:
            appendIfMatches(items, makeSnippet("db", "db.", "database handle", CK::Keyword),
                            currentWord);
            break;

        case MongoShellCompletionContext::AfterDb:
            for (const auto& coll : collections) {
                appendIfMatches(items,
                                makeSnippet(coll.name, coll.name + ".", "collection", CK::Table),
                                currentWord);
            }
            appendIfMatches(items,
                            makeSnippet("createCollection", R"(createCollection(""))",
                                        "create collection", CK::Function),
                            currentWord);
            appendIfMatches(items,
                            makeSnippet("runCommand", "runCommand({ ping: 1 })", "admin command",
                                        CK::Function),
                            currentWord);
            appendIfMatches(items,
                            makeSnippet("getCollectionNames", "getCollectionNames()", "list collections",
                                        CK::Function),
                            currentWord);
            break;

        case MongoShellCompletionContext::AfterCollection:
            appendIfMatches(items, makeSnippet("find", "find({})", "query documents", CK::Function),
                            currentWord);
            appendIfMatches(items, makeSnippet("findOne", "findOne({})", "query one document",
                                            CK::Function),
                            currentWord);
            appendIfMatches(items,
                            makeSnippet("aggregate", "aggregate([{ $match: {} }])", "aggregation pipeline",
                                        CK::Function),
                            currentWord);
            appendIfMatches(items, makeSnippet("insertOne", "insertOne({})", "insert document",
                                            CK::Function),
                            currentWord);
            appendIfMatches(items, makeSnippet("insertMany", "insertMany([{}])", "insert documents",
                                            CK::Function),
                            currentWord);
            appendIfMatches(items,
                            makeSnippet("updateOne", "updateOne({}, { $set: {} })", "update one",
                                        CK::Function),
                            currentWord);
            appendIfMatches(items,
                            makeSnippet("updateMany", "updateMany({}, { $set: {} })", "update many",
                                        CK::Function),
                            currentWord);
            appendIfMatches(items, makeSnippet("deleteOne", "deleteOne({})", "delete one",
                                            CK::Function),
                            currentWord);
            appendIfMatches(items, makeSnippet("deleteMany", "deleteMany({})", "delete many",
                                            CK::Function),
                            currentWord);
            appendIfMatches(items,
                            makeSnippet("countDocuments", "countDocuments({})", "count documents",
                                        CK::Function),
                            currentWord);
            appendIfMatches(items, makeSnippet("distinct", R"(distinct("field"))", "distinct values",
                                            CK::Function),
                            currentWord);
            appendIfMatches(items, makeSnippet("drop", "drop()", "drop collection", CK::Function),
                            currentWord);
            break;

        case MongoShellCompletionContext::AfterMethodChain:
            appendIfMatches(items, makeSnippet("limit", "limit(100)", "limit results", CK::Function),
                            currentWord);
            appendIfMatches(items, makeSnippet("skip", "skip(0)", "skip results", CK::Function),
                            currentWord);
            appendIfMatches(items,
                            makeSnippet("sort", "sort({ _id: 1 })", "sort results", CK::Function),
                            currentWord);
            break;

        case MongoShellCompletionContext::InDocument: {
            static const std::vector<std::pair<const char*, const char*>> operators = {
                {"$match", "{ $match: {} }"},
                {"$group", R"({ $group: { _id: "$field", count: { $sum: 1 } } })"},
                {"$sort", R"({ $sort: { _id: 1 } })"},
                {"$project", R"({ $project: { field: 1 } })"},
                {"$limit", "{ $limit: 100 }"},
                {"$skip", "{ $skip: 0 }"},
                {"$lookup", R"({ $lookup: { from: "", localField: "", foreignField: "_id", as: "" } })"},
                {"$unwind", R"({ $unwind: "$field" })"},
                {"$set", R"({ $set: { field: "value" } })"},
                {"$gt", R"({ $gt: 0 })"},
                {"$gte", R"({ $gte: 0 })"},
                {"$lt", R"({ $lt: 0 })"},
                {"$lte", R"({ $lte: 0 })"},
                {"$in", R"({ $in: [] })"},
                {"$ne", R"({ $ne: null })"},
                {"$exists", R"({ $exists: true })"},
                {"$regex", R"({ $regex: "pattern" })"},
            };
            for (const auto& [label, snippet] : operators) {
                appendIfMatches(items, makeSnippet(label, snippet, "operator", CK::Function),
                                currentWord);
            }
            appendIfMatches(items, makeSnippet("ObjectId", R"(ObjectId(""))", "ObjectId value",
                                               CK::Function),
                            currentWord);
            appendIfMatches(items, makeSnippet("ISODate", R"(ISODate(""))", "date value", CK::Function),
                            currentWord);

            if (const auto collName = extractCollectionName(linePrefix)) {
                for (const auto& coll : collections) {
                    if (coll.name != *collName) {
                        continue;
                    }
                    for (const auto& col : coll.columns) {
                        appendIfMatches(items,
                                        makeSnippet(col.name, col.name + ": ", "field", CK::Column),
                                        currentWord);
                    }
                    break;
                }
            }
            break;
        }

        case MongoShellCompletionContext::Generic:
            break;
        }

        return items;
    }

    std::vector<CI> filterMongoShellCompletions(const CompletionRequest& request,
                                                const std::vector<CI>& fallbackItems,
                                                const std::vector<Table>& collections) {
        if (request.forced) {
            std::vector<CI> all;
            all.reserve(64);
            const std::string linePrefix = getLinePrefix(request);
            const auto ctx = detectMongoShellContext(linePrefix, request.currentWord);
            for (const auto ctxType :
                 {MongoShellCompletionContext::Start, MongoShellCompletionContext::AfterDb,
                  MongoShellCompletionContext::AfterCollection,
                  MongoShellCompletionContext::AfterMethodChain,
                  MongoShellCompletionContext::InDocument}) {
                auto part = buildContextualCompletions(ctxType, collections, "", linePrefix);
                all.insert(all.end(), std::make_move_iterator(part.begin()),
                           std::make_move_iterator(part.end()));
            }
            std::ranges::sort(all, [](const CI& a, const CI& b) { return a.text < b.text; });
            auto ret = std::ranges::unique(all, [](const CI& a, const CI& b) {
                return a.text == b.text;
            });
            all.erase(ret.begin(), ret.end());
            return all;
        }

        const std::string linePrefix = getLinePrefix(request);
        const auto ctx = detectMongoShellContext(linePrefix, request.currentWord);
        auto items = buildContextualCompletions(ctx, collections, request.currentWord, linePrefix);

        if (items.empty() && ctx == MongoShellCompletionContext::Generic) {
            const std::string lowerWord = toLowerCopy(request.currentWord);
            for (const auto& item : fallbackItems) {
                const std::string lowerMatch =
                    toLowerCopy(item.matchText.empty() ? item.text : item.matchText);
                if (lowerWord.empty() || lowerMatch.find(lowerWord) == 0) {
                    items.push_back(item);
                }
            }
        }

        std::ranges::sort(items, [](const CI& a, const CI& b) { return a.text < b.text; });
        return items;
    }
} // namespace

MongoEditorTab::MongoEditorTab(const std::string& name, IDatabaseNode* node)
    : Tab(name, TabType::MONGO_EDITOR), node_(node) {
    editor_.SetShowLineNumbers(true);
    editor_.SetLanguage(sqleditor::TextEditor::Language::MongoShell);

    std::string exampleCollection = "collection";
    if (node_ && !node_->getTables().empty()) {
        exampleCollection = node_->getTables().front().name;
    }

    const std::string placeholder = std::format(
        "db.{}.find({{}})\n"
        "\n"
        "// MongoDB shell (mongosh) syntax:\n"
        "// db.collection.find({{ field: \"value\" }}).limit(100)\n"
        "// db.collection.aggregate([{{ $match: {{}} }}])\n"
        "// db.collection.insertOne({{ name: \"test\" }})\n"
        "// db.collection.updateMany({{ a: 1 }}, {{ $set: {{ b: 2 }} }})\n"
        "// db.collection.deleteMany({{ a: 1 }})\n"
        "// db.createCollection(\"new_collection\")\n"
        "// db.runCommand({{ ping: 1 }})",
        exampleCollection);
    editor_.SetPlaceholder(placeholder);
    editor_.SetSubmitCallback([this] {
        query_ = editor_.GetText();
        startQueryExecutionAsync(query_);
    });
}

MongoEditorTab::~MongoEditorTab() {
    queryExecutionOp_.cancel();
}

void MongoEditorTab::render() {
    const bool dark = Application::getInstance().isDarkTheme();
    editor_.SetPalette(
        sqleditor::TextEditor::FromTheme(dark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT));

    if (!completionKeywordsSet_ ||
        (node_ && node_->getTables().size() != lastCompletionCollectionCount_)) {
        updateCompletionKeywords();
    }

    checkQueryExecutionStatus();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Theme::Spacing::S);
    renderHeader();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::S);

    AISettingsDialog::instance().render();

    constexpr float toggleStripWidth = 28.0f;
    const float totalWidth = ImGui::GetContentRegionAvail().x;
    totalContentHeight_ = ImGui::GetContentRegionAvail().y;

    const float panelContentWidth = aiPanelVisible_ ? aiPanelWidth_ : 0.0f;
    float editorAreaWidth = totalWidth - toggleStripWidth - panelContentWidth;
    editorAreaWidth = std::max(200.0f, editorAreaWidth);

    // left pane: editor + results
    if (ImGui::BeginChild("##mongo_left_pane", ImVec2(editorAreaWidth, totalContentHeight_),
                          false)) {
        float paneHeight = ImGui::GetContentRegionAvail().y;
        const float toolbarHeight = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::S;
        const float editorHeight = paneHeight * splitterPosition_;
        const float resultsHeight = paneHeight * (1.0f - splitterPosition_) - 6.0f - toolbarHeight;

        if (ImGui::BeginChild("MongoEditor", ImVec2(-1, editorHeight), true,
                              ImGuiWindowFlags_NoScrollbar)) {
            if (pendingEditorFocusFrames_ > 0) {
                editor_.SetFocus();
                pendingEditorFocusFrames_--;
            }
            editor_.Render("##Mongo", ImVec2(-1, -1), true);
            query_ = editor_.GetText();
        }
        ImGui::EndChild();

        renderToolbar();
        UIUtils::Splitter("##mongo_splitter", &splitterPosition_, totalContentHeight_, 100.0f,
                          200.0f);

        if (ImGui::BeginChild("MongoResults", ImVec2(-1, resultsHeight), true,
                              ImGuiWindowFlags_NoScrollbar)) {
            ImVec2 contentStart = ImGui::GetCursorScreenPos();
            const bool isRunning = queryExecutionOp_.isRunning();
            if (isRunning)
                ImGui::BeginDisabled();
            renderQueryResults();
            if (isRunning)
                ImGui::EndDisabled();

            if (isRunning) {
                ImVec2 winPos = ImGui::GetWindowPos();
                ImVec2 winSize = ImGui::GetWindowSize();
                ImVec2 overlayEnd(winPos.x + winSize.x, winPos.y + winSize.y);

                const auto& colors = Application::getInstance().getCurrentColors();
                ImVec4 bg = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(colors.base));
                bg.w = 0.75f;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(contentStart, overlayEnd, ImGui::GetColorU32(bg));

                float cx = (contentStart.x + overlayEnd.x) * 0.5f;
                float cy = (contentStart.y + overlayEnd.y) * 0.5f;

                constexpr float spinnerRadius = 10.0f;
                ImGui::SetCursorScreenPos(
                    ImVec2(cx - spinnerRadius, cy - spinnerRadius - Theme::Spacing::M));
                UIUtils::Spinner("##mongo_results_spinner", spinnerRadius, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));

                const char* loadingText = LABEL_RUNNING_QUERY;
                ImVec2 textSize = ImGui::CalcTextSize(loadingText);
                ImGui::SetCursorScreenPos(
                    ImVec2(cx - textSize.x * 0.5f, cy + spinnerRadius + Theme::Spacing::S));
                ImGui::Text("%s", loadingText);
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    // AI panel content (when open)
    if (aiPanelVisible_) {
        ImGui::SameLine(0, 0);
        renderAIPanel(panelContentWidth, totalContentHeight_);
    }

    // toggle strip on the far right (always visible)
    ImGui::SameLine(0, 0);
    renderAIToggleStrip(toggleStripWidth, totalContentHeight_);
}

void MongoEditorTab::renderHeader() const {
    if (!node_) {
        ImGui::Text("Query Editor (No database selected)");
        ImGui::Separator();
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.green));
    ImGui::Text(ICON_FA_DATABASE);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, Theme::Spacing::S);
    ImGui::Text("%s", node_->getFullPath().c_str());

    ImGui::Separator();
}

void MongoEditorTab::renderToolbar() {
    if (queryExecutionOp_.isRunning()) {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_PLAY " Run");
        ImGui::EndDisabled();

        ImGui::SameLine(0, Theme::Spacing::M);
        if (ImGui::Button(LABEL_CANCEL)) {
            cancelQueryExecution();
        }
    } else {
        if (ImGui::Button(ICON_FA_PLAY " Run")) {
            startQueryExecutionAsync(query_);
        }
        ImGui::SameLine(0, Theme::Spacing::M);
        if (ImGui::Button(ICON_FA_ALIGN_LEFT " Format")) {
            formatQuery();
        }
    }
}

bool MongoEditorTab::resultHasJsonDocuments(const StatementResult& r) const {
    return !r.mongoDocumentJson.empty() &&
           r.mongoDocumentJson.size() == r.tableData.size();
}

void MongoEditorTab::renderResultViewToggle(const bool hasJsonDocuments) {
    if (!hasJsonDocuments) {
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::SameLine(0, Theme::Spacing::L);

    const bool tableView = resultViewMode_ == MongoResultViewMode::Table;
    if (tableView) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface1);
    }
    if (ImGui::Button(ICON_FA_TABLE " Table")) {
        resultViewMode_ = MongoResultViewMode::Table;
    }
    if (tableView) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Table view");
    }

    ImGui::SameLine();
    const bool jsonView = resultViewMode_ == MongoResultViewMode::Json;
    if (jsonView) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface1);
    }
    if (ImGui::Button(ICON_FA_CODE " JSON")) {
        resultViewMode_ = MongoResultViewMode::Json;
    }
    if (jsonView) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("JSON document view");
    }
}

void MongoEditorTab::renderQueryResults() {
    if (queryResult_.empty()) {
        ImGui::Text("%s", LABEL_NO_RESULTS);
        return;
    }

    if (queryResult_.executionTimeMs > 0) {
        ImGui::Text("Execution time: %.2f ms", queryResult_.executionTimeMs);
    }

    if (queryResult_.size() == 1) {
        renderSingleResult(queryResult_[0], 0);
        return;
    }

    if (ImGui::BeginTabBar("##MongoQueryResultTabs")) {
        int tabIndex = 0;
        for (size_t i = 0; i < queryResult_.size(); ++i) {
            const auto& r = queryResult_[i];
            std::string tabLabel;
            if (!r.success) {
                tabLabel = std::format("Error##{}", i);
            } else {
                tabLabel = std::format("Result {}##{}", tabIndex + 1, i);
            }
            ++tabIndex;

            if (ImGui::BeginTabItem(tabLabel.c_str())) {
                renderSingleResult(r, i);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

void MongoEditorTab::renderSingleResult(const StatementResult& r, size_t index) {
    if (!r.success) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", r.errorMessage.c_str());
        return;
    }

    if (r.columnNames.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", r.message.c_str());
        return;
    }

    const bool hasJsonDocuments = resultHasJsonDocuments(r);
    const bool showJsonView =
        hasJsonDocuments && resultViewMode_ == MongoResultViewMode::Json;

    if (r.tableData.empty()) {
        ImGui::Text("%s", LABEL_NO_ROWS);
    } else {
        ImGui::Text("Rows: %zu", r.tableData.size());
        if (static_cast<int>(r.tableData.size()) >= MAX_QUERY_ROWS) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", LABEL_ROW_LIMIT);
        }
        renderResultViewToggle(hasJsonDocuments);
    }

    if (r.tableData.empty()) {
        return;
    }

    if (showJsonView) {
        const float listHeight = std::max(ImGui::GetContentRegionAvail().y - 8.0f, 50.0f);
        if (ImGui::BeginChild(std::format("MongoJsonResults_{}", index).c_str(),
                              ImVec2(-1, listHeight), ImGuiChildFlags_None)) {
            JsonTreeView::renderDocumentList(r.mongoDocumentJson,
                                             ImGui::GetID("queryDocList"));
        }
        ImGui::EndChild();
        return;
    }

    float tableHeight = std::max(ImGui::GetContentRegionAvail().y - 20.0f, 50.0f);

    TableRenderer::Config config;
    config.allowEditing = false;
    config.showRowNumbers = true;
    config.minHeight = tableHeight;

    TableRenderer tableRenderer(config);
    tableRenderer.setColumns(r.columnNames);
    tableRenderer.setData(r.tableData);

    std::string tableId = "MongoQueryResults_" + std::to_string(index);
    tableRenderer.render(tableId.c_str());
}

void MongoEditorTab::startQueryExecutionAsync(const std::string& query) {
    if (queryExecutionOp_.isRunning())
        return;

    queryError_.clear();
    lastQueryDuration_ = std::chrono::milliseconds{0};

    if (!node_) {
        StatementResult r;
        r.success = false;
        r.errorMessage = "No database selected";
        queryResult_ = QueryResult{};
        queryResult_.statements.push_back(r);
        return;
    }

    IDatabaseNode* nodePtr = node_;
    queryExecutionOp_.startCancellable([query, nodePtr](const std::stop_token& stopToken) {
        QueryResult result;
        if (stopToken.stop_requested())
            return result;
        result = nodePtr->executeQuery(query);
        if (stopToken.stop_requested())
            return QueryResult{};
        return result;
    });

    // show placeholder while running
    StatementResult r;
    r.success = false;
    r.errorMessage = "Executing...";
    queryResult_ = QueryResult{};
    queryResult_.statements.push_back(r);
}

void MongoEditorTab::checkQueryExecutionStatus() {
    try {
        queryExecutionOp_.check([this](QueryResult result) {
            if (!result.empty() && !result.success()) {
                queryError_ = result.errorMessage();
                SentryUtils::addBreadcrumb("query", "Query error", "error", queryError_, "error");
            }
            lastQueryDuration_ =
                std::chrono::milliseconds{static_cast<long long>(result.executionTimeMs)};
            queryResult_ = std::move(result);
        });
    } catch (const std::exception& e) {
        queryError_ = "Error in async query execution: " + std::string(e.what());
    }
}

void MongoEditorTab::cancelQueryExecution() {
    queryExecutionOp_.cancel();
}

void MongoEditorTab::updateCompletionKeywords() {
    lastCompletionCollectionCount_ = node_ ? node_->getTables().size() : 0;

    static const std::vector<std::string> mongoKeywords = {
        "ObjectId", "ISODate", "NumberLong", "NumberInt", "true", "false", "null",
    };

    std::vector<CI> items;
    items.reserve(mongoKeywords.size());
    for (const auto& kw : mongoKeywords) {
        items.push_back({kw, CK::Keyword});
    }

    editor_.SetCompletionItems(std::move(items));
    editor_.SetCompletionFilter([this](const CompletionRequest& request,
                                       const std::vector<CI>& fallbackItems) {
        static const std::vector<Table> emptyCollections;
        const std::vector<Table>& collections =
            node_ ? node_->getTables() : emptyCollections;
        return filterMongoShellCompletions(request, fallbackItems, collections);
    });
    completionKeywordsSet_ = true;
}

void MongoEditorTab::formatQuery() {
    const std::string text = editor_.GetText();
    if (text.empty()) {
        return;
    }
    const std::string trimmed = [&] {
        std::string s = text;
        s.erase(0, s.find_first_not_of(" \t\n\r"));
        return s;
    }();
    if (!trimmed.empty() && trimmed.front() == '{') {
        std::string formatted = sqleditor::TextEditor::FormatJSON(text);
        if (!formatted.empty()) {
            editor_.SetText(formatted);
            query_ = formatted;
        }
    }
}

void MongoEditorTab::initAIPanel() {
    aiChatState_ = std::make_unique<AIChatState>(node_);
    aiChatPanel_ = std::make_unique<AIChatPanel>(aiChatState_.get());
    aiChatPanel_->setInsertCallback([this](const std::string& json) {
        std::string current = editor_.GetText();
        if (!current.empty() && current.back() != '\n') {
            current += "\n";
        }
        current += json;
        editor_.SetText(current);
        query_ = current;
    });
}

void MongoEditorTab::renderAIToggleStrip(float stripWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild("MongoAIToggleStrip", ImVec2(stripWidth, availableHeight),
                          ImGuiChildFlags_None)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 stripPos = ImGui::GetCursorScreenPos();

        // left borderline
        drawList->AddLine(stripPos, ImVec2(stripPos.x, stripPos.y + availableHeight),
                          ImGui::GetColorU32(colors.overlay0), 1.0f);

        // rotated "Assistant" label as clickable tab
        const char* label = "Assistant";
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        constexpr float padding = 6.0f;
        const float buttonW = stripWidth;
        const float buttonH = textSize.x + padding * 2.0f;

        ImGui::SetCursorScreenPos(ImVec2(stripPos.x, stripPos.y));
        ImGui::InvisibleButton("##mongoToggleAI", ImVec2(buttonW, buttonH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            aiPanelVisible_ = !aiPanelVisible_;
            if (aiPanelVisible_ && !aiChatPanel_) {
                initAIPanel();
            }
        }

        // button background
        const ImVec2 btnMin = stripPos;
        const ImVec2 btnMax(stripPos.x + buttonW, stripPos.y + buttonH);
        if (aiPanelVisible_ || hovered) {
            drawList->AddRectFilled(btnMin, btnMax, ImGui::GetColorU32(colors.surface1));
        }

        // bottom border of button area
        drawList->AddLine(ImVec2(btnMin.x, btnMax.y), btnMax, ImGui::GetColorU32(colors.overlay0),
                          1.0f);

        // draw rotated text centered in the button area
        const float cx = stripPos.x + buttonW * 0.5f;
        const float cy = stripPos.y + buttonH * 0.5f;
        const float textX = cx - textSize.x * 0.5f;
        const float textY = cy - textSize.y * 0.5f;

        drawList->PushClipRectFullScreen();
        const int vtxBegin = drawList->VtxBuffer.Size;
        drawList->AddText(
            ImVec2(textX, textY),
            ImGui::GetColorU32(hovered || aiPanelVisible_ ? colors.text : colors.subtext0), label);
        const int vtxEnd = drawList->VtxBuffer.Size;

        // rotate all text vertices 90 degrees around center
        for (int i = vtxBegin; i < vtxEnd; i++) {
            ImDrawVert& v = drawList->VtxBuffer[i];
            const float dx = v.pos.x - cx;
            const float dy = v.pos.y - cy;
            v.pos.x = cx - dy;
            v.pos.y = cy + dx;
        }
        drawList->PopClipRect();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void MongoEditorTab::renderAIPanel(float panelWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::BeginChild("MongoAIPanel", ImVec2(panelWidth, availableHeight),
                          ImGuiChildFlags_Borders)) {
        // resize handle on the left edge
        {
            constexpr float handleWidth = 4.0f;
            const ImVec2 panelPos = ImGui::GetWindowPos();
            const ImVec2 handleMin(panelPos.x, panelPos.y);

            ImGui::SetCursorScreenPos(handleMin);
            ImGui::InvisibleButton("##mongoAiResizeHandle", ImVec2(handleWidth, availableHeight));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                aiPanelWidth_ -= ImGui::GetIO().MouseDelta.x;
                aiPanelWidth_ = std::clamp(aiPanelWidth_, 250.0f, 600.0f);
            }

            ImGui::SetCursorPos(ImVec2(0, 0));
        }

        if (!aiChatPanel_) {
            initAIPanel();
        }
        if (aiChatState_) {
            aiChatState_->setCurrentSQL(query_);
        }
        if (aiChatPanel_) {
            aiChatPanel_->render();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
