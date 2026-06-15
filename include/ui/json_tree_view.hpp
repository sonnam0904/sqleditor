#pragma once

#include <bsoncxx/document/view.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using ImGuiID = unsigned int;

namespace JsonTreeView {
    struct Style {
        unsigned int keyColor = 0;
        unsigned int stringColor = 0;
        unsigned int numberColor = 0;
        unsigned int nullColor = 0;
        unsigned int boolColor = 0;
        unsigned int typeColor = 0;
        unsigned int punctuationColor = 0;
    };

    struct DocumentListOptions {
        int selectedRow = -1;
        std::function<void(int row)> onSelectRow;
        int rowNumberOffset = 0;
    };

    Style styleFromAppTheme();

    // Renders a BSON document as an expandable/collapsible tree (Compass-style).
    bool renderDocument(const bsoncxx::document::view& doc, ImGuiID seedId,
                        const Style* style = nullptr);

    // Parses extended/strict JSON then renders the tree. Returns false on parse error.
    bool renderJson(std::string_view json, ImGuiID seedId, const Style* style = nullptr);

    // Renders a scrollable list of documents (Compass JSON view).
    void renderDocumentList(const std::vector<std::string>& documents, ImGuiID seedId,
                            const DocumentListOptions* options = nullptr,
                            const Style* style = nullptr);
} // namespace JsonTreeView
