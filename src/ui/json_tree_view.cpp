#include "ui/json_tree_view.hpp"

#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <bsoncxx/array/view.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
    constexpr ImGuiTreeNodeFlags kBranchFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                ImGuiTreeNodeFlags_FramePadding |
                                                ImGuiTreeNodeFlags_DrawLinesFull |
                                                ImGuiTreeNodeFlags_SpanAvailWidth;

    constexpr ImGuiTreeNodeFlags kRootFlags =
        kBranchFlags | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap;

    constexpr ImGuiInputTextFlags kSelectableFlags =
        ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo;

    size_t countDocumentFields(const bsoncxx::document::view& doc) {
        size_t n = 0;
        for (auto&& _ : doc) {
            ++n;
        }
        return n;
    }

    size_t countArrayElements(const bsoncxx::array::view& arr) {
        size_t n = 0;
        for (auto&& _ : arr) {
            ++n;
        }
        return n;
    }

    void pushColor(const unsigned int col) {
        if (col != 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, col);
        }
    }

    void popColor(const unsigned int col) {
        if (col != 0) {
            ImGui::PopStyleColor();
        }
    }

    std::string formatIsoDate(const std::int64_t millis) {
        const auto seconds = millis / 1000;
        const auto time = static_cast<std::time_t>(seconds);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        return buf;
    }

    std::string valueToJson(const bsoncxx::types::bson_value::view& value) {
        switch (value.type()) {
        case bsoncxx::type::k_document:
            return bsoncxx::to_json(value.get_document().value);
        case bsoncxx::type::k_array:
            return bsoncxx::to_json(value.get_array().value);
        default: {
            try {
                auto wrapper = bsoncxx::builder::stream::document{}
                               << "v" << value << bsoncxx::builder::stream::finalize;
                return bsoncxx::to_json(wrapper.view());
            } catch (...) {
                return {};
            }
        }
        }
    }

    std::string valueToClipboard(const bsoncxx::types::bson_value::view& value) {
        switch (value.type()) {
        case bsoncxx::type::k_string:
            return std::string(value.get_string().value);
        case bsoncxx::type::k_int32:
            return std::to_string(value.get_int32().value);
        case bsoncxx::type::k_int64:
            return std::to_string(value.get_int64().value);
        case bsoncxx::type::k_double:
            return std::to_string(value.get_double().value);
        case bsoncxx::type::k_bool:
            return value.get_bool().value ? "true" : "false";
        case bsoncxx::type::k_oid:
            return value.get_oid().value.to_string();
        case bsoncxx::type::k_date:
            return formatIsoDate(value.get_date().value.count());
        case bsoncxx::type::k_null:
            return "null";
        case bsoncxx::type::k_decimal128:
            return value.get_decimal128().value.to_string();
        case bsoncxx::type::k_document:
        case bsoncxx::type::k_array:
            return valueToJson(value);
        default:
            return valueToJson(value);
        }
    }

    std::string formatScalarValue(const bsoncxx::types::bson_value::view& value) {
        switch (value.type()) {
        case bsoncxx::type::k_string:
            return std::format("\"{}\"", std::string(value.get_string().value));
        case bsoncxx::type::k_int32:
            return std::to_string(value.get_int32().value);
        case bsoncxx::type::k_int64:
            return std::to_string(value.get_int64().value);
        case bsoncxx::type::k_double:
            return std::to_string(value.get_double().value);
        case bsoncxx::type::k_bool:
            return value.get_bool().value ? "true" : "false";
        case bsoncxx::type::k_oid:
            return std::format("ObjectId('{}')", value.get_oid().value.to_string());
        case bsoncxx::type::k_date:
            return std::format("ISODate('{}')", formatIsoDate(value.get_date().value.count()));
        case bsoncxx::type::k_null:
            return "null";
        case bsoncxx::type::k_decimal128:
            return value.get_decimal128().value.to_string();
        default:
            return valueToJson(value);
        }
    }

    std::string& stableTextBuffer(const ImGuiID id, const std::string& text) {
        static std::unordered_map<ImGuiID, std::string> buffers;
        auto& stored = buffers[id];
        if (stored.empty()) {
            stored = text;
        } else if (ImGui::GetActiveID() != id && stored != text) {
            stored = text;
        }
        return stored;
    }

    void pushSelectableInputStyle() {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    }

    void popSelectableInputStyle() {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }

    void renderCopyMenu(const std::string& clipboardText, const char* copyLabel = nullptr) {
        if (!ImGui::BeginPopupContextItem()) {
            return;
        }

        const char* label = copyLabel ? copyLabel : ICON_FA_COPY " Copy value";
        if (ImGui::MenuItem(label) && !clipboardText.empty()) {
            ImGui::SetClipboardText(clipboardText.c_str());
        }
        ImGui::EndPopup();
    }

    void renderSelectableText(const std::string& text, const unsigned int color,
                              const std::string& clipboardText) {
        const ImGuiID id = ImGui::GetID("##jsonVal");
        std::string& buffer = stableTextBuffer(id, text);

        pushColor(color);
        pushSelectableInputStyle();

        const float textWidth = ImGui::CalcTextSize(buffer.c_str()).x + 4.0f;
        const float width = std::min(textWidth, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(width);
        ImGui::InputText("##jsonVal", buffer.data(), buffer.size() + 1, kSelectableFlags);

        popSelectableInputStyle();
        popColor(color);

        renderCopyMenu(clipboardText);
    }

    template<typename KeyView>
    void renderKeyLabel(const KeyView& key, const JsonTreeView::Style& style) {
        if (key.empty()) {
            return;
        }

        const std::string keyText(key);
        const ImGuiID id = ImGui::GetID("##jsonKey");
        std::string& buffer = stableTextBuffer(id, keyText);

        pushColor(style.keyColor);
        pushSelectableInputStyle();
        const float keyWidth = ImGui::CalcTextSize(buffer.c_str()).x + 4.0f;
        ImGui::SetNextItemWidth(keyWidth);
        ImGui::InputText("##jsonKey", buffer.data(), buffer.size() + 1, kSelectableFlags);
        popSelectableInputStyle();
        popColor(style.keyColor);

        renderCopyMenu(keyText, ICON_FA_COPY " Copy key");

        ImGui::SameLine(0, 4);
        pushColor(style.punctuationColor);
        ImGui::TextUnformatted(":");
        popColor(style.punctuationColor);
        ImGui::SameLine(0, 6);
    }

    void renderScalarValue(const bsoncxx::types::bson_value::view& value,
                           const JsonTreeView::Style& style) {
        unsigned int color = style.stringColor;
        switch (value.type()) {
        case bsoncxx::type::k_null:
            color = style.nullColor;
            break;
        case bsoncxx::type::k_bool:
            color = style.boolColor;
            break;
        case bsoncxx::type::k_int32:
        case bsoncxx::type::k_int64:
        case bsoncxx::type::k_double:
        case bsoncxx::type::k_decimal128:
            color = style.numberColor;
            break;
        case bsoncxx::type::k_oid:
        case bsoncxx::type::k_date:
            color = style.typeColor;
            break;
        default:
            break;
        }

        const std::string display = formatScalarValue(value);
        const std::string clipboard = valueToClipboard(value);
        renderSelectableText(display, color, clipboard);
    }

    void renderBranchContextMenu(const bsoncxx::types::bson_value::view& value) {
        const std::string json = valueToJson(value);
        renderCopyMenu(json, ICON_FA_COPY " Copy as JSON");
    }

    template<typename KeyView>
    void renderBsonValue(const KeyView& key, const bsoncxx::types::bson_value::view& value,
                         const JsonTreeView::Style& style) {
        switch (value.type()) {
        case bsoncxx::type::k_document: {
            const auto doc = value.get_document().value;
            const size_t fieldCount = countDocumentFields(doc);
            std::string label;
            if (!key.empty()) {
                label = std::format("{} : {{ {} fields }}", std::string_view(key), fieldCount);
            } else {
                label = std::format("{{ {} fields }}", fieldCount);
            }

            if (ImGui::TreeNodeEx(label.c_str(), kBranchFlags)) {
                for (auto&& child : doc) {
                    ImGui::PushID(child.offset());
                    renderBsonValue(child.key(), child.get_value(), style);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            renderBranchContextMenu(value);
            break;
        }
        case bsoncxx::type::k_array: {
            const auto arr = value.get_array().value;
            const size_t elementCount = countArrayElements(arr);
            std::string label;
            if (elementCount == 0) {
                label = !key.empty() ? std::format("{} : Array (empty)", std::string_view(key))
                                     : "Array (empty)";
            } else if (!key.empty()) {
                label = std::format("{} : [ {} elements ]", std::string_view(key), elementCount);
            } else {
                label = std::format("[ {} elements ]", elementCount);
            }

            if (ImGui::TreeNodeEx(label.c_str(), kBranchFlags)) {
                int index = 0;
                for (auto&& item : arr) {
                    ImGui::PushID(index);
                    renderBsonValue(std::to_string(index++), item.get_value(), style);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            renderBranchContextMenu(value);
            break;
        }
        default:
            renderKeyLabel(key, style);
            renderScalarValue(value, style);
            break;
        }
    }
} // namespace

namespace JsonTreeView {
    Style styleFromAppTheme() {
        const auto& colors = Application::getInstance().getCurrentColors();
        Style style;
        style.keyColor = ImGui::ColorConvertFloat4ToU32(colors.blue);
        style.stringColor = ImGui::ColorConvertFloat4ToU32(colors.green);
        style.numberColor = ImGui::ColorConvertFloat4ToU32(colors.peach);
        style.nullColor = ImGui::ColorConvertFloat4ToU32(colors.red);
        style.boolColor = ImGui::ColorConvertFloat4ToU32(colors.lavender);
        style.typeColor = ImGui::ColorConvertFloat4ToU32(colors.red);
        style.punctuationColor = ImGui::ColorConvertFloat4ToU32(colors.subtext0);
        return style;
    }

    bool renderDocument(const bsoncxx::document::view& doc, const ImGuiID seedId,
                        const Style* styleOverride) {
        const Style style = styleOverride ? *styleOverride : styleFromAppTheme();
        ImGui::PushID(static_cast<int>(seedId));

        const size_t fieldCount = countDocumentFields(doc);
        const std::string rootLabel = std::format("{{ {} fields }}", fieldCount);
        const bool open = ImGui::TreeNodeEx(rootLabel.c_str(), kRootFlags);
        if (open) {
            for (auto&& elem : doc) {
                ImGui::PushID(elem.offset());
                renderBsonValue(elem.key(), elem.get_value(), style);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(ICON_FA_COPY " Copy as JSON")) {
                ImGui::SetClipboardText(bsoncxx::to_json(doc).c_str());
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
        return true;
    }

    bool renderJson(const std::string_view json, const ImGuiID seedId, const Style* styleOverride) {
        try {
            const auto value = bsoncxx::from_json(std::string(json));
            if (!value.view().empty()) {
                return renderDocument(value.view(), seedId, styleOverride);
            }
        } catch (const std::exception&) {
        }

        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::PushID(static_cast<int>(seedId));
        ImGui::TextColored(colors.red, "Could not parse document JSON");

        const std::string raw(json);
        const ImGuiID id = ImGui::GetID("##rawJson");
        std::string& buffer = stableTextBuffer(id, raw);
        pushSelectableInputStyle();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextMultiline("##rawJson", buffer.data(), buffer.size() + 1, ImVec2(0, 0),
                                  kSelectableFlags);
        popSelectableInputStyle();
        renderCopyMenu(raw, ICON_FA_COPY " Copy text");

        ImGui::PopID();
        return false;
    }

    void renderDocumentList(const std::vector<std::string>& documents, const ImGuiID seedId,
                            const DocumentListOptions* options, const Style* styleOverride) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::PushID(static_cast<int>(seedId));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 18.0f);

        for (size_t i = 0; i < documents.size(); ++i) {
            const int row = static_cast<int>(i);
            const bool selected = options && options->selectedRow == row;
            const int displayRow =
                (options ? options->rowNumberOffset : 0) + row + 1;
            const std::string header =
                std::format("Document {}  ·  row {}", i + 1, displayRow);

            ImGui::PushID(row);

            const ImVec2 blockMin = ImGui::GetCursorScreenPos();
            const float blockWidth = ImGui::GetContentRegionAvail().x;

            if (options && options->onSelectRow) {
                if (ImGui::Selectable(header.c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    options->onSelectRow(row);
                }
            } else {
                ImGui::TextUnformatted(header.c_str());
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(ICON_FA_COPY " Copy document")) {
                    ImGui::SetClipboardText(documents[i].c_str());
                }
                ImGui::EndPopup();
            }

            ImGui::Spacing();
            renderJson(documents[i], ImGui::GetID("docTree"), styleOverride);

            const ImVec2 blockMax = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRect(ImVec2(blockMin.x, blockMin.y - 2.0f),
                              ImVec2(blockMin.x + blockWidth, blockMax.y + 6.0f),
                              ImGui::ColorConvertFloat4ToU32(colors.surface1), 4.0f);

            if (i + 1 < documents.size()) {
                ImGui::Dummy(ImVec2(0, 6));
            }

            ImGui::PopID();
        }

        ImGui::PopStyleVar(2);
        ImGui::PopID();
    }
} // namespace JsonTreeView
