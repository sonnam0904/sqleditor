#include "ui/tab/table_viewer_tab.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/ddl_utils.hpp"
#include "database/table_data_provider.hpp"
#include "database/mongodb/mongo_bson_format.hpp"
#include "database/mongodb/mongo_filter.hpp"
#include "database/mongodb/mongo_shell.hpp"
#include "database/mongodb/mongodb_database_node.hpp"
#include "database/mongodb_old/mongodb_old_database_node.hpp"
#include "database/sql_builder.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/json_tree_view.hpp"
#include "ui/ui_widgets.hpp"
#include "ui/query_history.hpp"
#include "utils/spinner.hpp"
#include "utils/table_exporter.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <iostream>
#include <spdlog/spdlog.h>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

    bool isNumericColumnType(const std::string& type) {
        std::string lower;
        lower.reserve(type.size());
        for (char c : type)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // strip parameterized suffix like "decimal(10,2)"
        if (const auto paren = lower.find('('); paren != std::string::npos)
            lower.resize(paren);
        while (!lower.empty() && std::isspace(static_cast<unsigned char>(lower.back())))
            lower.pop_back();

        static constexpr std::string_view numeric[] = {
            "int",        "integer",    "smallint",    "bigint",      "tinyint",
            "mediumint",  "varint",     "int2",        "int4",        "int8",
            "int16",      "int32",      "int64",       "uint16",      "uint32",
            "uint64",     "numeric",    "decimal",     "decimal128",  "real",
            "float",      "float32",    "float64",     "double",      "double precision",
            "money",      "smallmoney", "counter",     "number",
            "serial",     "bigserial",  "smallserial",
        };
        for (auto t : numeric) {
            if (lower == t)
                return true;
        }
        return false;
    }

    std::string formatSqlLiteral(const Column& col, const std::string& val) {
        if (isNullSentinel(val))
            return "NULL";
        if (isBoolSentinel(val))
            return boolSentinelValue(val) ? "TRUE" : "FALSE";
        if (isNumericColumnType(col.type))
            return val;
        return "'" + ddl_utils::escapeSingleQuotes(val) + "'";
    }

    bool isObjectIdHex(const std::string_view s) {
        if (s.size() != 24) {
            return false;
        }
        return std::ranges::all_of(s, [](const char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        });
    }

    std::string mongoTypeKey(const Column& col) {
        std::string lower;
        lower.reserve(col.type.size());
        for (const char c : col.type) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lower;
    }

    std::string escapeMongoShellString(const std::string& val) {
        std::string out;
        out.reserve(val.size() + 4);
        for (const char c : val) {
            switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
    }

    std::string formatMongoFieldName(const std::string_view name) {
        if (name.empty()) {
            return "\"\"";
        }
        for (const char c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '$') {
                return std::format("\"{}\"", name);
            }
        }
        return std::string(name);
    }

    std::string formatMongoShellValue(const Column& col, const std::string& val) {
        if (isNullSentinel(val)) {
            return "null";
        }

        const std::string type = mongoTypeKey(col);
        if (type == "objectid" || (col.name == "_id" && isObjectIdHex(val))) {
            return std::format("ObjectId(\"{}\")", val);
        }

        if (type == "bool" || type == "boolean") {
            if (isBoolSentinel(val)) {
                return boolSentinelValue(val) ? "true" : "false";
            }
            if (val == "true" || val == "false") {
                return val;
            }
        }

        if (type == "int32" || type == "int64" || type == "double" || type == "decimal128" ||
            isNumericColumnType(col.type)) {
            return val;
        }

        if (type == "date") {
            std::string iso = val;
            std::replace(iso.begin(), iso.end(), ' ', 'T');
            if (iso.find('T') != std::string::npos && iso.back() != 'Z') {
                iso += "Z";
            }
            return std::format("ISODate(\"{}\")", iso);
        }

        if (type == "document" || type == "array" ||
            (!val.empty() && (val.front() == '{' || val.front() == '['))) {
            return val;
        }

        return std::format("\"{}\"", escapeMongoShellString(val));
    }

    std::string buildMongoFilter(const Table& table, const std::vector<std::string>& rowValues) {
        std::vector<std::string> pkColumns;
        for (const auto& col : table.columns) {
            if (col.isPrimaryKey) {
                pkColumns.push_back(col.name);
            }
        }

        std::vector<std::string> parts;
        if (!pkColumns.empty()) {
            for (const auto& pkCol : pkColumns) {
                const auto colIt = std::ranges::find(table.columns, pkCol, &Column::name);
                if (colIt == table.columns.end()) {
                    continue;
                }
                const int colIdx =
                    static_cast<int>(std::distance(table.columns.begin(), colIt));
                if (colIdx < 0 || colIdx >= static_cast<int>(rowValues.size())) {
                    continue;
                }
                parts.push_back(std::format("{}: {}", formatMongoFieldName(pkCol),
                                            formatMongoShellValue(*colIt, rowValues[colIdx])));
            }
        } else {
            for (int colIdx = 0; colIdx < static_cast<int>(table.columns.size()) &&
                                 colIdx < static_cast<int>(rowValues.size());
                 ++colIdx) {
                parts.push_back(
                    std::format("{}: {}", formatMongoFieldName(table.columns[colIdx].name),
                                formatMongoShellValue(table.columns[colIdx], rowValues[colIdx])));
            }
        }

        std::string filter = "{ ";
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) {
                filter += ", ";
            }
            filter += parts[i];
        }
        filter += " }";
        return filter;
    }

    std::string mongoCollectionRef(const std::string& collectionName) {
        for (const char c : collectionName) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                return std::format("db[\"{}\"]", collectionName);
            }
        }
        return std::format("db.{}", collectionName);
    }

    std::string sqlColumnTypeLabel(std::string type) {
        while (!type.empty() && std::isspace(static_cast<unsigned char>(type.front()))) {
            type.erase(type.begin());
        }
        while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) {
            type.pop_back();
        }
        if (type.empty()) {
            return type;
        }

        std::string params;
        if (const auto paren = type.find('('); paren != std::string::npos) {
            params = type.substr(paren);
            type.resize(paren);
        }

        std::string lower;
        lower.reserve(type.size());
        for (char c : type) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        static const std::unordered_map<std::string, std::string> aliases = {
            {"character varying", "varchar"},
            {"character", "char"},
            {"timestamp without time zone", "timestamp"},
            {"timestamp with time zone", "timestamptz"},
            {"time without time zone", "time"},
            {"time with time zone", "timetz"},
            {"double precision", "float8"},
            {"boolean", "bool"},
            {"bit varying", "varbit"},
        };

        if (const auto it = aliases.find(lower); it != aliases.end()) {
            return it->second + params;
        }
        return type + params;
    }

    std::string columnTypeDisplayLabel(const Column& col, bool mongo) {
        if (mongo) {
            return mongo_bson::mongoTypeDisplayLabel(col.type);
        }
        return sqlColumnTypeLabel(col.type);
    }

} // namespace

TableViewerTab::TableViewerTab(const std::string& name, std::string databasePath, Table table,
                               IDatabaseNode* node)
    : Tab(name, TabType::TABLE_VIEWER), databasePath(std::move(databasePath)),
      table_(std::move(table)), node_(node) {
    initializeTableRenderer();
    initializeFilterAutoComplete();
    loadDataAsync();
}

void TableViewerTab::render() {
    const auto& colors = Application::getInstance().getCurrentColors();

    checkAsyncLoadStatus();

    // Cmd/Ctrl+S to save — only when no cell input is active
    if (hasChanges && !tableRenderer->isEditing() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        (ImGui::GetIO().KeyMods & ImGuiMod_Shortcut) && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveChanges();
    }

    renderToolbar(colors);

    // Show loading error if any
    if (hasLoadingError) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        ImGui::TextWrapped("Error loading data: %s", loadingError.c_str());
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            const std::string errorText = "Error loading data: " + loadingError;
            ImGui::SetClipboardText(errorText.c_str());
        }
    }

    // Reserve space for bottom controls (pagination bar)
    const float bottomControlsHeight = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::L * 2;
    const float availableHeight = ImGui::GetContentRegionAvail().y - bottomControlsHeight;

    // Horizontal layout: table area | panel content (when open) | toggle strip (always)
    constexpr float toggleStripWidth = 28.0f;
    const float panelContentWidth = rightPanelOpen ? rightPanelWidth : 0.0f;
    const float totalAvailableWidth = ImGui::GetContentRegionAvail().x;
    float tableAreaWidth = totalAvailableWidth - toggleStripWidth - panelContentWidth;
    tableAreaWidth = std::max(200.0f, tableAreaWidth);

    // Table display in a child window to prevent cutoff
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    if (ImGui::BeginChild("TableArea", ImVec2(tableAreaWidth, availableHeight),
                          ImGuiChildFlags_Borders)) {
        if (dataLoadOp.isRunning()) {
            ImGui::Text("Loading table data...");
        } else if (isMongoCollection() && mongoViewMode_ == MongoCollectionViewMode::Json &&
                   !mongoDocumentJson_.empty()) {
            renderMongoJsonView(tableAreaWidth, availableHeight);
        } else if (!table_.columns.empty() && !tableData.empty()) {
            // Update table renderer with current data
            tableRenderer->setColumns(table_.columns);
            tableRenderer->setData(tableData);
            tableRenderer->setCellEditedStatus(editedCells);
            tableRenderer->setSelectedCell(selectedRow, selectedCol);
            tableRenderer->setRowNumberOffset(currentPage * rowsPerPage);
            tableRenderer->setSortColumn(sortColumn, sortDirection);

            tableRenderer->render("TableData");
        } else {
            if (!currentFilter.empty()) {
                ImGui::Text("No rows match the filter: %s", currentFilter.c_str());
                ImGui::TextColored(
                    ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                    "Try a different filter condition or click 'Clear Filter' to see all data.");
            } else if (hasLoadingError) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error loading data");
            } else if (isMongoCollection() && mongoViewMode_ == MongoCollectionViewMode::Json) {
                ImGui::Text("No documents to display.");
            } else {
                ImGui::Text("No data to display. Execute a query to see results here.");
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // Panel content (when open)
    if (rightPanelOpen) {
        ImGui::SameLine(0, 0);
        renderRightPanel(panelContentWidth, availableHeight);
    }

    // Toggle strip on the far right (always visible)
    ImGui::SameLine(0, 0);
    renderRightPanelToggleStrip(toggleStripWidth, availableHeight);

    renderPaginationBar(colors);

    // Check async SQL execution status
    checkSQLExecutionStatus();

    // Show save confirmation dialog if needed
    showSaveConfirmationDialog();
}

void TableViewerTab::renderToolbar(const Theme::Colors& colors) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild("TableToolbar", ImVec2(-1, 0), ImGuiChildFlags_AutoResizeY);

    const bool mongo = isMongoCollection();
    const ImVec4 titleColor = mongo ? colors.green : colors.blue;
    ImGui::PushStyleColor(ImGuiCol_Text, titleColor);
    ImGui::Text("%s  %s", mongo ? ICON_FK_TABLE : ICON_FK_TABLE, table_.name.c_str());
    ImGui::PopStyleColor();

    const std::string rowBadge =
        mongo && mongoTotalRowsKnown ? std::format("~{} rows", totalRows)
        : mongo                      ? std::string{}
                                     : std::format("{} rows", totalRows);
    if (!mongo || mongoTotalRowsKnown) {
        const ImVec2 badgeTextSize = ImGui::CalcTextSize(rowBadge.c_str());
        const float badgeWidth = badgeTextSize.x + 16.0f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - badgeWidth +
                        ImGui::GetStyle().ItemSpacing.x);
        UIWidgets::statusBadge(rowBadge.c_str(),
                               ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.18f),
                               colors.blue);
    } else {
        const char* countLabel = mongoCountOp.isRunning() ? "Counting..." : "Count total";
        const ImVec2 labelSize = ImGui::CalcTextSize(countLabel);
        const float buttonWidth = labelSize.x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttonWidth +
                        ImGui::GetStyle().ItemSpacing.x);
        const bool canCount = !mongoCountOp.isRunning() && !dataLoadOp.isRunning();
        if (!canCount) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(countLabel)) {
            startMongoTotalCount();
        }
        if (!canCount) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Estimate total documents via collection metadata (fast)");
        }
    }

    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));

    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::Text(ICON_FA_FILTER);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, Theme::Spacing::S);

    if (filterAutoComplete) {
        const float filterWidth =
            std::max(280.0f, ImGui::GetContentRegionAvail().x * 0.42f);
        filterAutoComplete->setWidth(filterWidth);
        if (filterAutoComplete->render("##filter", filterBuffer, sizeof(filterBuffer))) {
            applyFilter();
        }
    }

    ImGui::SameLine(0, Theme::Spacing::S);
    if (UIWidgets::iconToolButton("refresh", ICON_FA_ARROWS_ROTATE, colors.blue)) {
        refreshData();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Refresh");
    }

    const bool canExport = !dataLoadOp.isRunning() && !filterParseError;
    ImGui::SameLine();
    ImGui::PushID("export");
    if (UIWidgets::iconToolButton("btn", ICON_FA_FILE_ARROW_DOWN, colors.teal, canExport)) {
        ImGui::OpenPopup("TableExportPopup");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!currentFilter.empty()) {
            ImGui::SetTooltip("Export filtered data");
        } else {
            ImGui::SetTooltip("Export");
        }
    }
    if (ImGui::BeginPopup("TableExportPopup")) {
        if (ImGui::MenuItem("CSV")) {
            exportFilteredData(ExportFormat::CSV);
        }
        if (ImGui::MenuItem("JSON")) {
            exportFilteredData(ExportFormat::JSON);
        }
        if (!mongo && ImGui::MenuItem("SQL")) {
            exportFilteredData(ExportFormat::SQL);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();

    ImGui::SameLine();
    if (UIWidgets::iconToolButton("save", ICON_FA_FLOPPY_DISK, colors.green, hasChanges)) {
        saveChanges();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Save");
    }

    ImGui::SameLine();
    if (UIWidgets::iconToolButton("reject", ICON_FA_XMARK, colors.red, hasChanges)) {
        cancelChanges();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Reject changes");
    }

    ImGui::SameLine();
    if (UIWidgets::iconToolButton("add", ICON_FA_PLUS, colors.text)) {
        addRow();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add row");
    }

    const bool hasRowSelected =
        selectedRow >= 0 && selectedRow < static_cast<int>(tableData.size());

    ImGui::SameLine();
    if (UIWidgets::iconToolButton("duplicate", ICON_FA_CLONE, colors.blue, hasRowSelected)) {
        duplicateRow(selectedRow);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Duplicate row");
    }

    ImGui::SameLine();
    if (UIWidgets::iconToolButton("delete", ICON_FA_TRASH_CAN, colors.red, hasRowSelected)) {
        deleteRow(selectedRow);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Delete row");
    }

    if (hasChanges) {
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::TextColored(colors.peach, ICON_FA_CIRCLE " Unsaved changes");
    }

    if (isMongoCollection()) {
        const float toggleWidth = ImGui::CalcTextSize(ICON_FA_TABLE " Table").x +
                                  ImGui::CalcTextSize(ICON_FA_CODE " JSON").x + 56.0f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - toggleWidth +
                        ImGui::GetStyle().ItemSpacing.x);
        UIWidgets::beginSegmentedControl("MongoViewToggle");
        if (UIWidgets::segmentedItem(ICON_FA_TABLE " Table",
                                     mongoViewMode_ == MongoCollectionViewMode::Table)) {
            mongoViewMode_ = MongoCollectionViewMode::Table;
        }
        ImGui::SameLine();
        if (UIWidgets::segmentedItem(ICON_FA_CODE " JSON",
                                     mongoViewMode_ == MongoCollectionViewMode::Json)) {
            mongoViewMode_ = MongoCollectionViewMode::Json;
        }
        UIWidgets::endSegmentedControl();
    }

    if (!currentFilter.empty()) {
        ImGui::Dummy(ImVec2(0, Theme::Spacing::XS));
        if (filterParseError) {
            ImGui::TextColored(colors.red, ICON_FA_TRIANGLE_EXCLAMATION " Invalid filter: %s",
                               currentFilter.c_str());
            if (mongoTotalRowsKnown) {
                ImGui::SameLine();
                ImGui::TextColored(colors.subtext0, "(ignored — showing all ~%d rows)", totalRows);
            }
        } else {
            ImGui::TextColored(colors.green, ICON_FA_FILTER " Active filter: %s",
                               currentFilter.c_str());
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
}

void TableViewerTab::renderPaginationBar(const Theme::Colors& colors) {
    ImGui::Dummy(ImVec2(0, Theme::Spacing::XS));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    ImGui::BeginChild("TablePagination", ImVec2(-1, 0), ImGuiChildFlags_AutoResizeY);

    const bool mongoUnknownTotal = isMongoCollection() && !mongoTotalRowsKnown;
    const int totalPages =
        mongoUnknownTotal ? 0 : std::max(1, (totalRows + rowsPerPage - 1) / rowsPerPage);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    if (ImGui::Button(ICON_FA_ANGLES_LEFT) && currentPage > 0) {
        firstPage();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("First page");
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ANGLE_LEFT) && currentPage > 0) {
        previousPage();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Previous page");
    }

    ImGui::SameLine(0, Theme::Spacing::M);
    ImGui::AlignTextToFramePadding();
    if (mongoUnknownTotal) {
        ImGui::Text("Page %d", currentPage + 1);
    } else {
        ImGui::Text("Page %d of %d", currentPage + 1, totalPages);
        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        if (isMongoCollection()) {
            ImGui::Text("(~%d rows)", totalRows);
        } else {
            ImGui::Text("(%d rows)", totalRows);
        }
        ImGui::PopStyleColor();
    }

    ImGui::SameLine(0, Theme::Spacing::M);
    if (!canGoToNextPage()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(ICON_FA_ANGLE_RIGHT) && canGoToNextPage()) {
        nextPage();
    }
    if (!canGoToNextPage()) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Next page");
    }

    ImGui::SameLine();
    if (!canGoToLastPage()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(ICON_FA_ANGLES_RIGHT) && canGoToLastPage()) {
        lastPage();
    }
    if (!canGoToLastPage()) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Last page");
    }

    ImGui::SameLine(0, Theme::Spacing::L);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::Text("Rows per page");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, Theme::Spacing::S);
    ImGui::SetNextItemWidth(80.0f);

    static const int pageSizeOptions[] = {10, 25, 50, 100, 200, 500};
    static const char* pageSizeLabels[] = {"10", "25", "50", "100", "200", "500"};
    int currentSizeIndex = -1;
    for (int i = 0; i < IM_ARRAYSIZE(pageSizeOptions); i++) {
        if (pageSizeOptions[i] == rowsPerPage) {
            currentSizeIndex = i;
            break;
        }
    }

    std::string customSizeLabel;
    const char* currentSizeLabel;
    if (currentSizeIndex >= 0) {
        currentSizeLabel = pageSizeLabels[currentSizeIndex];
    } else {
        customSizeLabel = std::to_string(rowsPerPage);
        currentSizeLabel = customSizeLabel.c_str();
    }

    if (ImGui::BeginCombo("##pagesize", currentSizeLabel)) {
        for (int i = 0; i < IM_ARRAYSIZE(pageSizeOptions); i++) {
            const bool isSelected = (pageSizeOptions[i] == rowsPerPage);
            if (ImGui::Selectable(pageSizeLabels[i], isSelected)) {
                if (pageSizeOptions[i] != rowsPerPage) {
                    const int oldRowsPerPage = rowsPerPage;
                    const int firstRowOnCurrentPage = currentPage * oldRowsPerPage;
                    rowsPerPage = pageSizeOptions[i];
                    currentPage = firstRowOnCurrentPage / rowsPerPage;
                    loadDataAsync();
                }
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if (dataLoadOp.isRunning()) {
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        ImGui::TextUnformatted("Loading...");
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void TableViewerTab::nextPage() {
    if (!canGoToNextPage() || dataLoadOp.isRunning()) {
        return;
    }
    currentPage++;
    if (selectedRow >= 0 && selectedCol >= 0) {
        selectedRow = 0;
    }
    loadDataAsync();
}

void TableViewerTab::previousPage() {
    if (currentPage > 0 && !dataLoadOp.isRunning()) {
        currentPage--;
        // When moving to previous page from keyboard navigation, select last row
        if (selectedRow >= 0 && selectedCol >= 0) {
            // We'll set this to the last row after data loads
            selectedRow =
                rowsPerPage - 1; // This will be adjusted in checkAsyncLoadStatus if needed
        }
        loadDataAsync();
    }
}

void TableViewerTab::firstPage() {
    if (!dataLoadOp.isRunning()) {
        currentPage = 0;
        loadDataAsync();
    }
}

void TableViewerTab::lastPage() {
    if (!canGoToLastPage() || dataLoadOp.isRunning()) {
        return;
    }

    const int totalPages = std::max(1, (totalRows + rowsPerPage - 1) / rowsPerPage);
    currentPage = totalPages - 1;
    loadDataAsync();
}

void TableViewerTab::refreshData() {
    if (!dataLoadOp.isRunning()) {
        // Reset selection state
        selectedRow = -1;
        selectedCol = -1;
        hasChanges = false;
        deletedRows.clear();
        hasLoadingError = false;
        loadingError.clear();

        // Clear edited cells tracking
        for (auto& row : editedCells) {
            std::fill(row.begin(), row.end(), false);
        }

        loadDataAsync();
    }
}

void TableViewerTab::saveChanges() {
    if (!hasChanges) {
        return;
    }

    // Generate SQL statements for the changes
    pendingUpdateSQL = generateUpdateSQL();

    if (pendingUpdateSQL.empty()) {
        // No valid SQL generated, just clear changes
        hasChanges = false;
        deletedRows.clear();
        originalData = tableData;
        isNewRow.assign(tableData.size(), false);
        for (auto& row : editedCells) {
            std::fill(row.begin(), row.end(), false);
        }
        return;
    }

    // Show confirmation dialog
    showSaveDialog = true;
}

void TableViewerTab::cancelChanges() {
    // Remove newly added rows (iterate backwards to preserve indices)
    for (int i = static_cast<int>(isNewRow.size()) - 1; i >= 0; i--) {
        if (isNewRow[i]) {
            tableData.erase(tableData.begin() + i);
            originalData.erase(originalData.begin() + i);
            editedCells.erase(editedCells.begin() + i);
            isNewRow.erase(isNewRow.begin() + i);
        }
    }

    std::ranges::sort(deletedRows, {}, &DeletedRow::index);
    for (const auto& deleted : deletedRows) {
        const int insertIdx = std::clamp(deleted.index, 0, static_cast<int>(originalData.size()));
        originalData.insert(originalData.begin() + insertIdx, deleted.values);
        editedCells.insert(editedCells.begin() + insertIdx,
                           std::vector<bool>(table_.columns.size(), false));
        isNewRow.insert(isNewRow.begin() + insertIdx, false);
    }
    deletedRows.clear();

    // Restore original data for remaining rows
    tableData = originalData;
    hasChanges = false;

    // Clear edited cells tracking
    for (auto& row : editedCells) {
        std::fill(row.begin(), row.end(), false);
    }

    // Reset selection state
    selectedRow = -1;
    selectedCol = -1;
}

void TableViewerTab::deleteRow(int row) {
    if (row < 0 || row >= static_cast<int>(tableData.size())) {
        return;
    }

    if (row < static_cast<int>(isNewRow.size()) && isNewRow[row]) {
        tableData.erase(tableData.begin() + row);
        originalData.erase(originalData.begin() + row);
        editedCells.erase(editedCells.begin() + row);
        isNewRow.erase(isNewRow.begin() + row);
    } else {
        int originalIndex = row;
        for (const auto& deleted : deletedRows) {
            if (deleted.index <= originalIndex)
                originalIndex++;
        }
        deletedRows.push_back({originalIndex, originalData[row]});
        tableData.erase(tableData.begin() + row);
        originalData.erase(originalData.begin() + row);
        editedCells.erase(editedCells.begin() + row);
        isNewRow.erase(isNewRow.begin() + row);
        if (!isMongoCollection() || mongoTotalRowsKnown) {
            totalRows = std::max(0, totalRows - 1);
        }
    }

    if (tableData.empty()) {
        selectedRow = -1;
        selectedCol = -1;
    } else {
        selectedRow = std::min(row, static_cast<int>(tableData.size()) - 1);
        selectedCol = table_.columns.empty()
                          ? -1
                          : std::clamp(selectedCol, 0, static_cast<int>(table_.columns.size()) - 1);
    }
    hasChanges = hasPendingChanges();
}

void TableViewerTab::duplicateRow(int row) {
    if (row < 0 || row >= static_cast<int>(tableData.size()) || table_.columns.empty())
        return;

    const int insertIdx = row + 1;
    std::vector<std::string> newRow = tableData[row];

    tableData.insert(tableData.begin() + insertIdx, newRow);
    originalData.insert(originalData.begin() + insertIdx,
                        std::vector<std::string>(table_.columns.size(), ""));
    editedCells.insert(editedCells.begin() + insertIdx,
                       std::vector<bool>(table_.columns.size(), true));
    isNewRow.insert(isNewRow.begin() + insertIdx, true);

    hasChanges = true;
    selectedRow = insertIdx;
    selectedCol = 0;
}

void TableViewerTab::addRow() {
    if (table_.columns.empty())
        return;

    // Insert below selected row, or at end
    int insertIdx = (selectedRow >= 0) ? selectedRow + 1 : static_cast<int>(tableData.size());

    // Create empty row
    std::vector<std::string> newRow(table_.columns.size(), "");

    tableData.insert(tableData.begin() + insertIdx, newRow);
    originalData.insert(originalData.begin() + insertIdx,
                        std::vector<std::string>(table_.columns.size(), ""));
    editedCells.insert(editedCells.begin() + insertIdx,
                       std::vector<bool>(table_.columns.size(), true));
    isNewRow.insert(isNewRow.begin() + insertIdx, true);

    hasChanges = true;

    // Select first cell of new row
    selectedRow = insertIdx;
    selectedCol = 0;
}

void TableViewerTab::selectCell(const int row, const int col) {
    const int tableSize = static_cast<int>(tableData.size());
    const int totalCols = static_cast<int>(table_.columns.size());
    if (row >= 0 && row < tableSize && col >= 0 && col < totalCols) {
        selectedRow = row;
        selectedCol = col;
    }
}

void TableViewerTab::loadDataAsync() {
    hasLoadingError = false;
    loadingError.clear();

    // Clear current data to show loading state if filter changed
    if (filterChanged) {
        tableData.clear();
        if (!isMongoCollection()) {
            totalRows = 0;
        }
        filterChanged = false;
        spdlog::debug("Cleared previous filtered data, starting fresh load");
    }

    // Build ORDER BY clause from sort state
    const std::string orderByClause = buildOrderByClause();

    dataLoadOp.start([this, orderByClause]() -> bool {
        try {
            const int offset = currentPage * rowsPerPage;
            tableData =
                node_->getTableData(table_, rowsPerPage, offset, currentFilter, orderByClause);

            mongoDocumentJson_.clear();
            if (isMongoCollection()) {
                if (auto* mongoNode = dynamic_cast<MongoDBDatabaseNode*>(node_)) {
                    mongoDocumentJson_ = mongoNode->getCollectionDocumentsAsJson(
                        table_, rowsPerPage, offset, currentFilter, orderByClause);
                } else if (auto* mongoOldNode = dynamic_cast<MongoDBOldDatabaseNode*>(node_)) {
                    mongoDocumentJson_ = mongoOldNode->getCollectionDocumentsAsJson(
                        table_, rowsPerPage, offset, currentFilter, orderByClause);
                }
            }

            if (!isMongoCollection()) {
                totalRows = node_->getRowCount(table_, currentFilter);
            }

            originalData = tableData;
            hasChanges = false;
            deletedRows.clear();

            editedCells = std::vector<std::vector<bool>>(
                tableData.size(), std::vector<bool>(table_.columns.size(), false));
            isNewRow = std::vector<bool>(tableData.size(), false);
        } catch (const std::exception& e) {
            hasLoadingError = true;
            loadingError = e.what();
        }
        return true;
    });
}

void TableViewerTab::checkAsyncLoadStatus() {
    dataLoadOp.check([this](bool) {
        // Auto-select first cell on initial load
        if (!initialSelectionDone && !tableData.empty() && !table_.columns.empty()) {
            selectedRow = 0;
            selectedCol = 0;
            initialSelectionDone = true;
        }

        // update filter autocomplete with column names
        if (!table_.columns.empty() && filterAutoComplete) {
            std::vector<std::string> colNames;
            colNames.reserve(table_.columns.size());
            for (const auto& c : table_.columns)
                colNames.push_back(c.name);
            filterAutoComplete->addKeywords(colNames);
        }

        // Add to query history if load was successful
        if (!hasLoadingError && !tableData.empty()) {
            const auto builder = createSQLBuilder(node_->getDatabaseType());
            const int offset = currentPage * rowsPerPage;
            std::string query =
                std::format("SELECT * FROM {}", builder->quoteIdentifier(table_.name));
            if (!currentFilter.empty()) {
                query += std::format(" WHERE {}", currentFilter);
            }
            if (sortColumn >= 0 && sortDirection != SortDirection::None &&
                !sortColumnName.empty()) {
                query += std::format(" ORDER BY {} {}", builder->quoteIdentifier(sortColumnName),
                                     sortDirection == SortDirection::Ascending ? "ASC" : "DESC");
            }
            query += std::format(" LIMIT {} OFFSET {}", rowsPerPage, offset);

            QueryHistory::instance().add(query, static_cast<int>(tableData.size()));
        }
    });

    mongoCountOp.check([this](int count) {
        totalRows = std::max(0, count);
        mongoTotalRowsKnown = true;
    });
}

std::vector<std::string> TableViewerTab::getPrimaryKeyColumns() const {
    std::vector<std::string> pkColumns;
    for (const auto& column : table_.columns) {
        if (column.isPrimaryKey) {
            pkColumns.push_back(column.name);
        }
    }
    return pkColumns;
}

std::string TableViewerTab::buildTableRef() const {
    const auto builder = createSQLBuilder(node_->getDatabaseType());
    if (const auto dotPos = table_.name.find('.'); dotPos != std::string::npos) {
        const std::string schemaName = table_.name.substr(0, dotPos);
        const std::string tableNameOnly = table_.name.substr(dotPos + 1);
        return std::format("{}.{}", builder->quoteIdentifier(schemaName),
                           builder->quoteIdentifier(tableNameOnly));
    }
    return builder->quoteIdentifier(table_.name);
}

std::string TableViewerTab::buildRowWhere(const std::vector<std::string>& rowValues) const {
    const std::vector<std::string> pkColumns = getPrimaryKeyColumns();
    const auto builder = createSQLBuilder(node_->getDatabaseType());
    std::vector<std::string> conditions;

    if (!pkColumns.empty()) {
        for (const auto& pkCol : pkColumns) {
            auto pkColIt = std::ranges::find(table_.columns, pkCol, &Column::name);
            if (pkColIt == table_.columns.end())
                continue;
            const int pkColIdx = static_cast<int>(std::distance(table_.columns.begin(), pkColIt));
            if (pkColIdx < 0 || pkColIdx >= static_cast<int>(rowValues.size()))
                continue;
            const std::string quotedCol = builder->quoteIdentifier(pkCol);
            const std::string& pkValue = rowValues[pkColIdx];
            if (isNullSentinel(pkValue)) {
                conditions.push_back(std::format("{} IS NULL", quotedCol));
            } else {
                conditions.push_back(
                    std::format("{} = {}", quotedCol, formatSqlLiteral(*pkColIt, pkValue)));
            }
        }
    } else {
        for (int colIdx = 0; colIdx < static_cast<int>(table_.columns.size()) &&
                             colIdx < static_cast<int>(rowValues.size());
             colIdx++) {
            const std::string quotedCol = builder->quoteIdentifier(table_.columns[colIdx].name);
            const std::string& condValue = rowValues[colIdx];
            if (isNullSentinel(condValue)) {
                conditions.push_back(std::format("{} IS NULL", quotedCol));
            } else {
                conditions.push_back(std::format(
                    "{} = {}", quotedCol, formatSqlLiteral(table_.columns[colIdx], condValue)));
            }
        }
    }

    std::string expr;
    for (size_t i = 0; i < conditions.size(); ++i) {
        if (i > 0)
            expr += " AND ";
        expr += conditions[i];
    }
    return expr;
}

bool TableViewerTab::hasPendingChanges() const {
    if (!deletedRows.empty())
        return true;
    if (std::ranges::any_of(isNewRow, [](bool value) { return value; }))
        return true;
    return std::ranges::any_of(editedCells, [](const auto& row) {
        return std::ranges::any_of(row, [](bool value) { return value; });
    });
}

std::vector<std::string> TableViewerTab::generateUpdateSQL() {
    if (isMongoCollection()) {
        return generateMongoShellCommands();
    }

    std::vector<std::string> sqlStatements;

    const auto builder = createSQLBuilder(node_->getDatabaseType());

    std::cout << "Generating UPDATE SQL for table: " << table_.name << std::endl;
    const std::vector<std::string> pkColumns = getPrimaryKeyColumns();
    std::cout << "Primary key columns: ";
    for (const auto& pk : pkColumns) {
        std::cout << pk << " ";
    }
    std::cout << std::endl;

    const std::string tableRef = buildTableRef();

    for (const auto& deleted : deletedRows) {
        const std::string whereExpr = buildRowWhere(deleted.values);
        if (!whereExpr.empty()) {
            sqlStatements.push_back(builder->deleteRow(tableRef, whereExpr) + ";");
        }
    }

    // Process each edited cell
    for (int rowIdx = 0; rowIdx < editedCells.size(); rowIdx++) {
        // Generate INSERT for newly added rows
        if (rowIdx < static_cast<int>(isNewRow.size()) && isNewRow[rowIdx]) {
            std::vector<std::string> insertCols;
            std::vector<std::string> insertVals;
            insertCols.reserve(table_.columns.size());
            insertVals.reserve(table_.columns.size());
            for (size_t colIdx = 0; colIdx < table_.columns.size(); colIdx++) {
                insertCols.push_back(table_.columns[colIdx].name);
                const std::string& val = tableData[rowIdx][colIdx];
                insertVals.push_back(val.empty() ? "NULL"
                                                 : formatSqlLiteral(table_.columns[colIdx], val));
            }
            sqlStatements.push_back(builder->insertRow(tableRef, insertCols, insertVals) + ";");
            continue;
        }

        for (int colIdx = 0; colIdx < editedCells[rowIdx].size(); colIdx++) {
            if (!editedCells[rowIdx][colIdx]) {
                continue;
            }
            const std::string& columnName = table_.columns[colIdx].name;
            const std::string& newValue = tableData[rowIdx][colIdx];
            const std::vector<std::pair<std::string, std::string>> assignments = {
                {columnName, formatSqlLiteral(table_.columns[colIdx], newValue)}};
            sqlStatements.push_back(
                builder->updateRow(tableRef, assignments, buildRowWhere(originalData[rowIdx])) +
                ";");
        }
    }

    return sqlStatements;
}

std::vector<std::string> TableViewerTab::generateMongoShellCommands() const {
    std::vector<std::string> commands;
    const std::string collRef = mongoCollectionRef(table_.name);

    for (const auto& deleted : deletedRows) {
        const std::string filter = buildMongoFilter(table_, deleted.values);
        commands.push_back(std::format("{}.deleteOne({})", collRef, filter));
    }

    for (int rowIdx = 0; rowIdx < static_cast<int>(editedCells.size()); ++rowIdx) {
        if (rowIdx < static_cast<int>(isNewRow.size()) && isNewRow[rowIdx]) {
            std::vector<std::string> docParts;
            docParts.reserve(table_.columns.size());
            for (size_t colIdx = 0; colIdx < table_.columns.size(); ++colIdx) {
                const auto& col = table_.columns[colIdx];
                const std::string& val = tableData[rowIdx][colIdx];
                if (col.name == "_id" && (val.empty() || isNullSentinel(val))) {
                    continue;
                }
                docParts.push_back(std::format(
                    "{}: {}", formatMongoFieldName(col.name), formatMongoShellValue(col, val)));
            }
            if (!docParts.empty()) {
                std::string document = "{ ";
                for (size_t i = 0; i < docParts.size(); ++i) {
                    if (i > 0) {
                        document += ", ";
                    }
                    document += docParts[i];
                }
                document += " }";
                commands.push_back(std::format("{}.insertOne({})", collRef, document));
            }
            continue;
        }

        std::vector<std::string> setParts;
        for (int colIdx = 0; colIdx < static_cast<int>(editedCells[rowIdx].size()); ++colIdx) {
            if (!editedCells[rowIdx][colIdx]) {
                continue;
            }
            const auto& col = table_.columns[colIdx];
            setParts.push_back(std::format(
                "{}: {}", formatMongoFieldName(col.name),
                formatMongoShellValue(col, tableData[rowIdx][colIdx])));
        }
        if (setParts.empty()) {
            continue;
        }

        std::string setClause;
        for (size_t i = 0; i < setParts.size(); ++i) {
            if (i > 0) {
                setClause += ", ";
            }
            setClause += setParts[i];
        }

        const std::string filter = buildMongoFilter(table_, originalData[rowIdx]);
        commands.push_back(
            std::format("{}.updateOne({}, {{ $set: {{ {} }} }})", collRef, filter, setClause));
    }

    return commands;
}

void TableViewerTab::showSaveConfirmationDialog() {
    if (!showSaveDialog) {
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();
    const bool dark = Application::getInstance().isDarkTheme();

    // Only open popup once when dialog first shows
    if (!dialogOpened) {
        // Join all statements into one editable block
        std::string combined;
        for (size_t i = 0; i < pendingUpdateSQL.size(); i++) {
            if (i > 0)
                combined += "\n\n";
            combined += pendingUpdateSQL[i];
        }
        if (isMongoCollection()) {
            combined = sqleditor::TextEditor::FormatMongoShell(combined);
            saveDialogEditor_.SetLanguage(sqleditor::TextEditor::Language::MongoShell);
        } else {
            combined = sqleditor::TextEditor::FormatSQL(combined);
            saveDialogEditor_.SetLanguage(sqleditor::TextEditor::Language::SQL);
        }
        saveDialogEditor_.SetShowLineNumbers(true);
        saveDialogEditor_.SetReadOnly(false);
        saveDialogEditor_.SetText(combined);
        saveDialogEditor_.SetFocus();

        ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_Always);
        ImGui::OpenPopup("Confirm Save Changes");
        dialogOpened = true;
    }

    // Sync palette with current theme every frame
    saveDialogEditor_.SetPalette(
        sqleditor::TextEditor::FromTheme(dark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.base);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.55f));

    if (ImGui::BeginPopupModal("Confirm Save Changes", nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        // Header
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::TextUnformatted(isMongoCollection() ? "Review and edit the MongoDB commands before "
                                                     "executing."
                                                   : "Review and edit the SQL before executing.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));

        // Editor area
        const float footerH = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::M * 2;
        const float editorH = ImGui::GetContentRegionAvail().y - footerH;
        saveDialogEditor_.Render("##save_sql_editor", ImVec2(-1, editorH), true);

        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));

        // Buttons
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::L, Theme::Spacing::S));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

        if (sqlExecutionOp.isRunning()) {
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.4f));
            ImGui::PushStyleColor(ImGuiCol_Text, colors.base);
            ImGui::Button(ICON_FA_PLAY " Execute");
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();

            ImGui::SameLine(0, Theme::Spacing::M);
            UIUtils::Spinner("##spinner", 7.0f, 2, ImGui::GetColorU32(colors.blue));
            ImGui::SameLine(0, Theme::Spacing::S);
            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
            ImGui::TextUnformatted("Executing...");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(colors.green.x, colors.green.y, colors.green.z, 1.0f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive,
                ImVec4(colors.green.x * 0.8f, colors.green.y * 0.8f, colors.green.z * 0.8f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, colors.base);
            if (ImGui::Button(ICON_FA_PLAY " Execute")) {
                const std::string editedSQL = saveDialogEditor_.GetText();
                const bool isMongo = node_ && isMongoDbType(node_->getDatabaseType());
                sqlExecutionOp.start([node = node_, editedSQL, isMongo]() -> std::pair<bool, std::string> {
                    if (!node) {
                        return {false, "Error: Database does not support query execution"};
                    }

                    const auto runOne = [&](const std::string& statement) -> std::pair<bool, std::string> {
                        spdlog::debug("Executing query: {}", statement);
                        const auto result = node->executeQuery(statement);
                        if (!result.success()) {
                            spdlog::error("Query execution failed: {}", result.errorMessage());
                            return {false, "Error: " + result.errorMessage()};
                        }
                        return {true, {}};
                    };

                    if (isMongo) {
                        const auto statements = splitMongoShellCommands(editedSQL);
                        if (statements.empty()) {
                            return {false, "Error: No MongoDB shell commands to execute"};
                        }
                        for (const auto& statement : statements) {
                            if (auto outcome = runOne(statement); !outcome.first) {
                                return outcome;
                            }
                        }
                        return {true, {}};
                    }

                    return runOne(editedSQL);
                });
            }
            ImGui::PopStyleColor(4);

            ImGui::SameLine(0, Theme::Spacing::M);

            ImGui::PushStyleColor(ImGuiCol_Button, colors.surface1);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface2);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.overlay0);
            if (ImGui::Button(ICON_FA_XMARK " Cancel")) {
                showSaveDialog = false;
                pendingUpdateSQL.clear();
                dialogOpened = false;
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::PopStyleVar(3);
        ImGui::EndPopup();
    } else if (!ImGui::IsPopupOpen("Confirm Save Changes")) {
        showSaveDialog = false;
        pendingUpdateSQL.clear();
        dialogOpened = false;
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void TableViewerTab::checkSQLExecutionStatus() {
    sqlExecutionOp.check([this](std::pair<bool, std::string> result) {
        auto [success, errorMessage] = result;

        if (success) {
            hasChanges = false;
            originalData = tableData;
            deletedRows.clear();
            isNewRow.assign(tableData.size(), false);
            for (auto& row : editedCells) {
                std::fill(row.begin(), row.end(), false);
            }
            showSaveDialog = false;
            pendingUpdateSQL.clear();
            dialogOpened = false;
        } else {
            std::cerr << "Failed to execute SQL statements: " << errorMessage << std::endl;
        }
    });
}

std::string TableViewerTab::buildOrderByClause() const {
    if (sortColumn < 0 || sortDirection == SortDirection::None || sortColumnName.empty()) {
        return {};
    }
    const auto builder = createSQLBuilder(node_->getDatabaseType());
    return std::format("{} {}", builder->quoteIdentifier(sortColumnName),
                       sortDirection == SortDirection::Ascending ? "ASC" : "DESC");
}

void TableViewerTab::exportFilteredData(const ExportFormat format) {
    auto* provider = dynamic_cast<ITableDataProvider*>(node_);
    if (!provider) {
        spdlog::warn("Cannot export: node does not provide table data");
        return;
    }

    TableExporter::TableExportOptions options;
    options.whereClause = currentFilter;
    options.orderByClause = buildOrderByClause();

    if (isMongoCollection() && format == ExportFormat::JSON) {
        const std::string filter = currentFilter;
        const std::string orderBy = options.orderByClause;
        options.fetchJsonDocuments = [this, filter, orderBy](const int limit, const int offset) {
            if (auto* mongoNode = dynamic_cast<MongoDBDatabaseNode*>(node_)) {
                return mongoNode->getCollectionDocumentsAsJson(table_, limit, offset, filter,
                                                                 orderBy);
            }
            if (auto* mongoOldNode = dynamic_cast<MongoDBOldDatabaseNode*>(node_)) {
                return mongoOldNode->getCollectionDocumentsAsJson(table_, limit, offset, filter,
                                                                  orderBy);
            }
            return std::vector<std::string>{};
        };
    }

    TableExporter::exportTable(provider, table_, format, node_->getDatabaseType(), options);
}

void TableViewerTab::applyFilter() {
    std::string newFilter = std::string(filterBuffer);

    // Trim whitespace
    newFilter.erase(0, newFilter.find_first_not_of(" \t\n\r"));
    newFilter.erase(newFilter.find_last_not_of(" \t\n\r") + 1);

    // Check if filter actually changed
    if (newFilter == currentFilter) {
        return;
    }

    filterParseError = false;
    if (isMongoDbType(node_->getDatabaseType()) && !newFilter.empty()) {
        filterParseError = !isValidMongoFilter(newFilter);
    }

    currentFilter = newFilter;
    filterChanged = true;

    // Reset to first page when filter changes
    currentPage = 0;

    // Clear selection when filter changes
    selectedRow = -1;
    selectedCol = -1;

    // Clear any error states
    hasLoadingError = false;
    loadingError.clear();

    // Reload data with new filter
    loadDataAsync();
}

void TableViewerTab::initializeTableRenderer() {
    // Initialize table renderer with editable configuration
    TableRenderer::Config config;
    config.allowEditing = true;
    config.showRowNumbers = true;
    config.minHeight = 200.0f;
    config.showColumnTypes = true;

    const bool mongo = isMongoCollection();
    tableRenderer = std::make_unique<TableRenderer>(config);
    tableRenderer->setFormatColumnType([mongo](const std::string& rawType) {
        if (mongo) {
            return mongo_bson::mongoTypeDisplayLabel(rawType);
        }
        return sqlColumnTypeLabel(rawType);
    });

    // Set up callbacks
    tableRenderer->setOnCellEdit([this](int row, int col, const std::string& newValue) {
        // empty edit on a null cell means no change
        if (newValue.empty() && isNullSentinel(tableData[row][col])) {
            return;
        }
        if (newValue != tableData[row][col]) {
            tableData[row][col] = newValue;
            hasChanges = true;

            // Mark cell as edited
            if (row < editedCells.size() && col < editedCells[row].size()) {
                editedCells[row][col] = true;
            }
        }
    });

    tableRenderer->setOnCellSelect([this](int row, int col) { selectCell(row, col); });

    // Sort callback
    tableRenderer->setOnSortChanged(
        [this](int col, const std::string& colName, SortDirection direction) {
            sortColumn = col;
            sortColumnName = colName;
            sortDirection = direction;

            // Reset to first page when sort changes
            currentPage = 0;

            // Clear selection
            selectedRow = -1;
            selectedCol = -1;

            // Reload data with new sort
            loadDataAsync();
        });

    // nullable check for "Set NULL" context menu
    tableRenderer->setColumnNullableCallback([this](int col) -> bool {
        if (col < 0 || col >= static_cast<int>(table_.columns.size()))
            return false;
        return !table_.columns[col].isNotNull;
    });

    tableRenderer->setOnSetNull([this](int row, int col) {
        if (row < 0 || row >= static_cast<int>(tableData.size()))
            return;
        if (isNullSentinel(tableData[row][col]))
            return;

        tableData[row][col] = std::string(NULL_SENTINEL);
        hasChanges = true;
        if (row < static_cast<int>(editedCells.size()) &&
            col < static_cast<int>(editedCells[row].size())) {
            editedCells[row][col] = true;
        }
    });

    tableRenderer->setOnFilterByValue([this](int row, int col, const std::string& value) {
        if (row < 0 || row >= static_cast<int>(tableData.size()) || col < 0 ||
            col >= static_cast<int>(table_.columns.size())) {
            return;
        }

        const auto builder = createSQLBuilder(node_->getDatabaseType());
        const std::string quotedCol = builder->quoteIdentifier(table_.columns[col].name);
        std::string predicate;
        if (isNullSentinel(value)) {
            predicate = std::format("{} IS NULL", quotedCol);
        } else {
            const std::string literal =
                isMongoCollection()
                    ? formatMongoShellValue(table_.columns[col], value)
                    : formatSqlLiteral(table_.columns[col], value);
            predicate = std::format("{} = {}", quotedCol, literal);
        }

        const std::string nextFilter = currentFilter.empty()
                                           ? predicate
                                           : std::format("({}) AND ({})", currentFilter, predicate);
        std::strncpy(filterBuffer, nextFilter.c_str(), sizeof(filterBuffer) - 1);
        filterBuffer[sizeof(filterBuffer) - 1] = '\0';
        applyFilter();
    });

    tableRenderer->setOnDeleteRow([this](int row) { deleteRow(row); });
}

void TableViewerTab::initializeFilterAutoComplete() {
    AutoCompleteInput::Config config;
    config.hint = "e.g. id = 1 and name LIKE 'john%'";
    config.width = 400.0f;
    config.onSubmit = [this]() { applyFilter(); };
    config.endIcon = ICON_FA_MAGNIFYING_GLASS;
    config.onEndIconClick = [this]() { applyFilter(); };

    // Initialize with SQL keywords
    config.keywords = {"AND", "OR",  "NOT",  "IN",   "LIKE",  "BETWEEN", "IS",   "NULL",  "EXISTS",
                       "ALL", "ANY", "SOME", "TRUE", "FALSE", "ASC",     "DESC", "LIMIT", "OFFSET"};

    // Add column names when they become available
    if (!table_.columns.empty()) {
        for (const auto& c : table_.columns)
            config.keywords.push_back(c.name);
    }

    filterAutoComplete = std::make_unique<AutoCompleteInput>(config);
}

void TableViewerTab::renderRightPanelToggleStrip(float stripWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild("PanelToggleStrip", ImVec2(stripWidth, availableHeight),
                          ImGuiChildFlags_None)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 stripPos = ImGui::GetCursorScreenPos();

        // Draw left borderline for the strip
        drawList->AddLine(stripPos, ImVec2(stripPos.x, stripPos.y + availableHeight),
                          ImGui::GetColorU32(colors.overlay0), 1.0f);

        // "Panels" rotated label as a compact clickable tab at the top
        const auto label = "Inspector";
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        constexpr float padding = 6.0f;
        // After rotation, text width becomes button height, text height becomes button width
        const float buttonW = stripWidth;
        const float buttonH = textSize.x + padding * 2.0f;

        // Clickable area for the "Panels" button
        ImGui::SetCursorScreenPos(ImVec2(stripPos.x, stripPos.y));
        ImGui::InvisibleButton("##toggle_inspector", ImVec2(buttonW, buttonH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            rightPanelOpen = !rightPanelOpen;
        }

        // Button background
        const ImVec2 btnMin = stripPos;
        const ImVec2 btnMax(stripPos.x + buttonW, stripPos.y + buttonH);
        if (rightPanelOpen) {
            drawList->AddRectFilled(btnMin, btnMax, ImGui::GetColorU32(colors.surface1));
        } else if (hovered) {
            drawList->AddRectFilled(btnMin, btnMax, ImGui::GetColorU32(colors.surface1));
        }

        // Bottom border of the button area
        drawList->AddLine(ImVec2(btnMin.x, btnMax.y), btnMax, ImGui::GetColorU32(colors.overlay0),
                          1.0f);

        // Draw rotated text centered in the button area
        // Push full-screen clip rect so text isn't clipped before rotation
        const float cx = stripPos.x + buttonW * 0.5f;
        const float cy = stripPos.y + buttonH * 0.5f;
        const float textX = cx - textSize.x * 0.5f;
        const float textY = cy - textSize.y * 0.5f;

        drawList->PushClipRectFullScreen();
        const int vtxBegin = drawList->VtxBuffer.Size;
        drawList->AddText(ImVec2(textX, textY),
                          ImGui::GetColorU32(hovered ? colors.text : colors.subtext0), label);
        const int vtxEnd = drawList->VtxBuffer.Size;

        // Rotate all text vertices 90 degrees (top-to-bottom reading) around center
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

void TableViewerTab::renderRightPanel(float panelWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::BeginChild("RightPanel", ImVec2(panelWidth, availableHeight),
                          ImGuiChildFlags_Borders)) {
        // Resize handle on the left edge of the panel
        {
            constexpr float handleWidth = 4.0f;
            const ImVec2 panelPos = ImGui::GetWindowPos();
            const ImVec2 handleMin(panelPos.x, panelPos.y);

            ImGui::SetCursorScreenPos(handleMin);
            ImGui::InvisibleButton("##resize_handle", ImVec2(handleWidth, availableHeight));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                rightPanelWidth -= ImGui::GetIO().MouseDelta.x;
                rightPanelWidth = std::clamp(rightPanelWidth, 200.0f, 600.0f);
            }

            // Reset cursor to after the handle for normal content
            ImGui::SetCursorPos(ImVec2(0, 0));
        }

        if (ImGui::BeginTabBar("##panel_tabs")) {
            if (ImGui::BeginTabItem("Value")) {
                activeRightPanelTab = 0;
                renderValueTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Metadata")) {
                activeRightPanelTab = 1;
                renderMetadataTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void TableViewerTab::syncValuePanelBuffer() {
    // Check if selection changed
    bool selectionChanged = (selectedRow != lastSyncedRow || selectedCol != lastSyncedCol);

    // Also detect if the cell value was edited externally (e.g., inline editing in table)
    bool valueChanged = false;
    if (!selectionChanged && selectedRow >= 0 && selectedCol >= 0 &&
        selectedRow < static_cast<int>(tableData.size()) &&
        selectedCol < static_cast<int>(table_.columns.size())) {
        const std::string& currentValue = tableData[selectedRow][selectedCol];
        const std::string bufStr(valuePanelBuffer);
        // empty buffer matches null sentinel; bool sentinels always match (handled separately)
        bool matches = (bufStr == currentValue) ||
                       (bufStr.empty() && isNullSentinel(currentValue)) ||
                       isBoolSentinel(currentValue);
        if (!valuePanelBufferDirty && !matches) {
            valueChanged = true;
        }
    }

    if (selectionChanged || valueChanged) {
        lastSyncedRow = selectedRow;
        lastSyncedCol = selectedCol;
        valuePanelBufferDirty = false;

        if (selectedRow >= 0 && selectedCol >= 0 &&
            selectedRow < static_cast<int>(tableData.size()) &&
            selectedCol < static_cast<int>(table_.columns.size())) {
            const std::string& value = tableData[selectedRow][selectedCol];
            if (isNullSentinel(value)) {
                valuePanelBuffer[0] = '\0';
            } else {
                std::strncpy(valuePanelBuffer, value.c_str(), sizeof(valuePanelBuffer) - 1);
                valuePanelBuffer[sizeof(valuePanelBuffer) - 1] = '\0';
            }
        } else {
            valuePanelBuffer[0] = '\0';
        }
    }
}

void TableViewerTab::renderValueTab() {
    const auto& colors = Application::getInstance().getCurrentColors();

    if (isMongoCollection() && mongoViewMode_ == MongoCollectionViewMode::Json) {
        if (selectedRow < 0 || selectedRow >= static_cast<int>(mongoDocumentJson_.size())) {
            ImGui::TextColored(colors.subtext0, "Select a document to view its JSON.");
            return;
        }

        ImGui::TextColored(colors.blue, "Document %d",
                           currentPage * rowsPerPage + selectedRow + 1);
        ImGui::Separator();

        JsonTreeView::renderJson(mongoDocumentJson_[static_cast<size_t>(selectedRow)],
                                 ImGui::GetID("valuePanelTree"));
        return;
    }

    syncValuePanelBuffer();

    if (selectedRow < 0 || selectedCol < 0 || selectedRow >= static_cast<int>(tableData.size()) ||
        selectedCol >= static_cast<int>(table_.columns.size())) {
        ImGui::TextColored(colors.subtext0, "Select a cell to view its value.");
        return;
    }

    // Header: column name and row info
    ImGui::TextColored(colors.blue, "%s", table_.columns[selectedCol].name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(colors.subtext0, "(Row %d)", currentPage * rowsPerPage + selectedRow + 1);
    ImGui::Separator();

    // boolean cells: show a checkbox instead of the text editor
    const std::string& cellValue = tableData[selectedRow][selectedCol];
    if (isBoolSentinel(cellValue)) {
        bool checked = boolSentinelValue(cellValue);
        if (ImGui::Checkbox("##value_panel_bool", &checked)) {
            tableData[selectedRow][selectedCol] =
                std::string(checked ? BOOL_TRUE_SENTINEL : BOOL_FALSE_SENTINEL);
            if (selectedRow < static_cast<int>(editedCells.size()) &&
                selectedCol < static_cast<int>(editedCells[selectedRow].size())) {
                editedCells[selectedRow][selectedCol] = true;
            }
            hasChanges = true;
        }
        ImGui::SameLine();
        ImGui::Text("%s", checked ? "true" : "false");
        return;
    }

    // Multiline text editor for the cell value
    const float availH = ImGui::GetContentRegionAvail().y -
                         (valuePanelBufferDirty ? ImGui::GetFrameHeightWithSpacing() + 4.0f : 0.0f);
    if (ImGui::InputTextMultiline("##value_panel_edit", valuePanelBuffer, sizeof(valuePanelBuffer),
                                  ImVec2(-1, availH))) {
        // Check if user modified the buffer
        const std::string& currentValue = tableData[selectedRow][selectedCol];
        const std::string bufStr(valuePanelBuffer);
        // empty buffer on a null cell means no change
        if (bufStr.empty() && isNullSentinel(currentValue)) {
            valuePanelBufferDirty = false;
        } else {
            valuePanelBufferDirty = (bufStr != currentValue);
        }
    }

    // Apply / Revert buttons when buffer is dirty
    if (valuePanelBufferDirty) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colors.green.x * 0.3f, colors.green.y * 0.3f,
                                                      colors.green.z * 0.3f, 1.0f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4(colors.green.x * 0.5f, colors.green.y * 0.5f, colors.green.z * 0.5f, 1.0f));
        if (ImGui::Button("Apply")) {
            tableData[selectedRow][selectedCol] = std::string(valuePanelBuffer);
            if (selectedRow < static_cast<int>(editedCells.size()) &&
                selectedCol < static_cast<int>(editedCells[selectedRow].size())) {
                editedCells[selectedRow][selectedCol] = true;
            }
            hasChanges = true;
            valuePanelBufferDirty = false;
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colors.red.x * 0.3f, colors.red.y * 0.3f,
                                                      colors.red.z * 0.3f, 1.0f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4(colors.red.x * 0.5f, colors.red.y * 0.5f, colors.red.z * 0.5f, 1.0f));
        if (ImGui::Button("Revert")) {
            // Restore from current cell value
            const std::string& currentValue = tableData[selectedRow][selectedCol];
            if (isNullSentinel(currentValue)) {
                valuePanelBuffer[0] = '\0';
            } else {
                std::strncpy(valuePanelBuffer, currentValue.c_str(), sizeof(valuePanelBuffer) - 1);
                valuePanelBuffer[sizeof(valuePanelBuffer) - 1] = '\0';
            }
            valuePanelBufferDirty = false;
        }
        ImGui::PopStyleColor(2);
    }
}

void TableViewerTab::renderMetadataTab() {
    const auto& colors = Application::getInstance().getCurrentColors();

    if (table_.columns.empty()) {
        ImGui::TextColored(colors.subtext0, "No metadata available.");
        return;
    }

    // Filter input
    ImGui::SetNextItemWidth(-1);
    char filterBuf[128];
    std::strncpy(filterBuf, metadataFilter.c_str(), sizeof(filterBuf) - 1);
    filterBuf[sizeof(filterBuf) - 1] = '\0';
    if (ImGui::InputTextWithHint("##meta_filter", "Filter columns...", filterBuf,
                                 sizeof(filterBuf))) {
        metadataFilter = filterBuf;
    }

    ImGui::Separator();

    // Column metadata table
    if (ImGui::BeginTable("##MetadataColumns", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Null", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("PK", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();

        // Convert filter to lowercase for case-insensitive matching
        std::string filterLower = metadataFilter;
        std::ranges::transform(filterLower, filterLower.begin(), ::tolower);

        for (const auto& col : table_.columns) {
            // Apply filter
            if (!filterLower.empty()) {
                std::string nameLower = col.name;
                std::ranges::transform(nameLower, nameLower.begin(), ::tolower);
                if (nameLower.find(filterLower) == std::string::npos) {
                    continue;
                }
            }

            ImGui::TableNextRow();

            // Name column - highlight if it matches selected column
            ImGui::TableSetColumnIndex(0);
            bool isSelectedColumn =
                (selectedCol >= 0 && selectedCol < static_cast<int>(table_.columns.size()) &&
                 table_.columns[selectedCol].name == col.name);
            if (isSelectedColumn) {
                ImGui::TextColored(colors.blue, "%s", col.name.c_str());
            } else {
                ImGui::Text("%s", col.name.c_str());
            }

            // Type column
            ImGui::TableSetColumnIndex(1);
            const std::string typeLabel =
                columnTypeDisplayLabel(col, isMongoCollection());
            ImGui::TextColored(colors.subtext0, "%s", typeLabel.c_str());

            // Nullable column
            ImGui::TableSetColumnIndex(2);
            if (col.isNotNull) {
                ImGui::TextColored(colors.red, "NOT NULL");
            } else {
                ImGui::TextColored(colors.green, "NULL");
            }

            // PK column
            ImGui::TableSetColumnIndex(3);
            if (col.isPrimaryKey) {
                ImGui::TextColored(colors.yellow, "PK");
            }
        }

        ImGui::EndTable();
    }
}

bool TableViewerTab::isMongoCollection() const {
    return node_ && isMongoDbType(node_->getDatabaseType());
}

void TableViewerTab::startMongoTotalCount() {
    if (!isMongoCollection() || mongoCountOp.isRunning()) {
        return;
    }

    mongoCountOp.start([this]() -> int {
        if (auto* mongoNode = dynamic_cast<MongoDBDatabaseNode*>(node_)) {
            return mongoNode->getEstimatedDocumentCount(table_);
        }
        if (auto* mongoOldNode = dynamic_cast<MongoDBOldDatabaseNode*>(node_)) {
            return mongoOldNode->getEstimatedDocumentCount(table_);
        }
        return -1;
    });
}

bool TableViewerTab::mongoHasMorePages() const {
    return static_cast<int>(tableData.size()) >= rowsPerPage;
}

bool TableViewerTab::canGoToNextPage() const {
    if (isMongoCollection() && !mongoTotalRowsKnown) {
        return mongoHasMorePages();
    }
    const int totalPages = std::max(1, (totalRows + rowsPerPage - 1) / rowsPerPage);
    return currentPage < totalPages - 1;
}

bool TableViewerTab::canGoToLastPage() const {
    if (isMongoCollection() && !mongoTotalRowsKnown) {
        return false;
    }
    const int totalPages = std::max(1, (totalRows + rowsPerPage - 1) / rowsPerPage);
    return currentPage < totalPages - 1;
}

void TableViewerTab::renderMongoJsonView(float /*width*/, float height) {
    if (!ImGui::BeginChild("MongoJsonView", ImVec2(0, height), ImGuiChildFlags_None)) {
        return;
    }

    JsonTreeView::DocumentListOptions options;
    options.selectedRow = selectedRow;
    options.rowNumberOffset = currentPage * rowsPerPage;
    options.onSelectRow = [this](int row) { selectCell(row, 0); };

    JsonTreeView::renderDocumentList(mongoDocumentJson_, ImGui::GetID("mongoDocs"), &options);

    ImGui::EndChild();
}
