#pragma once

#include "database/database_node.hpp"
#include "imgui.h"
#include <string>

namespace TableImporter {
    bool importFromCSV(IDatabaseNode* node, const std::string& tableName);

    inline void renderImportMenu(IDatabaseNode* node, const std::string& tableName) {
        if (ImGui::BeginMenu("Import")) {
            if (ImGui::MenuItem("CSV")) {
                importFromCSV(node, tableName);
            }
            ImGui::EndMenu();
        }
    }
} // namespace TableImporter
