#pragma once

#include "database/db_interface.hpp"
#include "database/table_data_provider.hpp"
#include "imgui.h"
#include <functional>
#include <string>
#include <vector>

enum class ExportFormat { CSV, JSON, SQL };

namespace TableExporter {

struct TableExportOptions {
    std::string whereClause;
    std::string orderByClause;
    /** When set, JSON export writes native MongoDB documents instead of flat columns. */
    std::function<std::vector<std::string>(int limit, int offset)> fetchJsonDocuments;
};

bool exportTables(ITableDataProvider* provider, const std::vector<const Table*>& tables,
                  ExportFormat format, DatabaseType dbType = DatabaseType::SQLITE);

bool exportTable(ITableDataProvider* provider, const Table& table, ExportFormat format,
                 DatabaseType dbType, const TableExportOptions& options = {});

inline void renderExportMenu(ITableDataProvider* provider, const Table& table,
                             DatabaseType dbType = DatabaseType::SQLITE) {
    if (ImGui::BeginMenu("Export")) {
        if (ImGui::MenuItem("CSV")) {
            exportTables(provider, {&table}, ExportFormat::CSV, dbType);
        }
        if (ImGui::MenuItem("JSON")) {
            exportTables(provider, {&table}, ExportFormat::JSON, dbType);
        }
        if (ImGui::MenuItem("SQL")) {
            exportTables(provider, {&table}, ExportFormat::SQL, dbType);
        }
        ImGui::EndMenu();
    }
}

} // namespace TableExporter
