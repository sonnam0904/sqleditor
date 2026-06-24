#include "ui/json_tree_view.hpp"

#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mongodb/mongo_shell.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <bsoncxx/array/view.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/value.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <chrono>
#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {
    constexpr ImGuiTreeNodeFlags kBranchFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                ImGuiTreeNodeFlags_FramePadding |
                                                ImGuiTreeNodeFlags_DrawLinesFull |
                                                ImGuiTreeNodeFlags_SpanAvailWidth;

    constexpr ImGuiTreeNodeFlags kRootFlags =
        kBranchFlags | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap;

    constexpr ImGuiInputTextFlags kSelectableFlags =
        ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo;

    using BsonPath = std::vector<std::string>;

    struct EditContext {
        bsoncxx::document::value* document = nullptr;
        JsonTreeView::EditOptions* edit = nullptr;
        bool modified = false;

        void markModified() {
            modified = true;
            if (edit && edit->onModified) {
                edit->onModified();
            }
        }
    };

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

    std::string escapeShellString(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 4);
        for (const char c : s) {
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

    std::string formatShellKey(std::string_view key) {
        if (key.empty()) {
            return "\"\"";
        }
        for (const char c : key) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '$') {
                return std::format("\"{}\"", escapeShellString(key));
            }
        }
        return std::string(key);
    }

    std::string bsonTypeLabel(const bsoncxx::type type) {
        switch (type) {
        case bsoncxx::type::k_string:
            return "String";
        case bsoncxx::type::k_int32:
            return "Int32";
        case bsoncxx::type::k_int64:
            return "Int64";
        case bsoncxx::type::k_double:
            return "Double";
        case bsoncxx::type::k_bool:
            return "Boolean";
        case bsoncxx::type::k_oid:
            return "ObjectId";
        case bsoncxx::type::k_date:
            return "Date";
        case bsoncxx::type::k_null:
            return "Null";
        case bsoncxx::type::k_document:
            return "Object";
        case bsoncxx::type::k_array:
            return "Array";
        case bsoncxx::type::k_decimal128:
            return "Decimal128";
        case bsoncxx::type::k_binary:
            return "Binary";
        default:
            return "Mixed";
        }
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

    std::string valueToShellLiteral(const bsoncxx::types::bson_value::view& value) {
        switch (value.type()) {
        case bsoncxx::type::k_string:
            return std::format("\"{}\"",
                                escapeShellString(std::string(value.get_string().value)));
        case bsoncxx::type::k_int32:
            return std::to_string(value.get_int32().value);
        case bsoncxx::type::k_int64:
            return std::to_string(value.get_int64().value);
        case bsoncxx::type::k_double:
            return std::to_string(value.get_double().value);
        case bsoncxx::type::k_bool:
            return value.get_bool().value ? "true" : "false";
        case bsoncxx::type::k_oid:
            return std::format("ObjectId(\"{}\")", value.get_oid().value.to_string());
        case bsoncxx::type::k_date:
            return std::format("ISODate(\"{}\")", formatIsoDate(value.get_date().value.count()));
        case bsoncxx::type::k_null:
            return "null";
        case bsoncxx::type::k_decimal128:
            return value.get_decimal128().value.to_string();
        case bsoncxx::type::k_document: {
            std::string out = "{";
            bool first = true;
            for (auto&& elem : value.get_document().value) {
                if (!first) {
                    out += ", ";
                }
                first = false;
                out += formatShellKey(std::string(elem.key()));
                out += ": ";
                out += valueToShellLiteral(elem.get_value());
            }
            out += '}';
            return out;
        }
        case bsoncxx::type::k_array: {
            std::string out = "[";
            bool first = true;
            for (auto&& item : value.get_array().value) {
                if (!first) {
                    out += ", ";
                }
                first = false;
                out += valueToShellLiteral(item.get_value());
            }
            out += ']';
            return out;
        }
        default:
            return valueToJson(value);
        }
    }

    std::string trimLiteral(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.remove_prefix(1);
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    std::optional<bsoncxx::types::bson_value::value>
    parseShellValue(const std::string& literal) {
        const std::string trimmed = trimLiteral(literal);
        if (trimmed.empty()) {
            return bsoncxx::types::bson_value::value{bsoncxx::type::k_null};
        }

        try {
            std::string ext = shellLiteralToExtendedJson(trimmed);
            if (ext.empty()) {
                return std::nullopt;
            }

            const bool wrapped =
                ext.front() != '{' && ext.front() != '[' && ext.front() != '"';
            if (wrapped) {
                ext = std::format(R"({{"v":{}}})", ext);
            }

            auto doc = bsoncxx::from_json(ext);
            const auto view = doc.view();
            if (wrapped && view.length() == 1) {
                auto elem = view["v"];
                if (elem) {
                    return bsoncxx::types::bson_value::value{elem.get_value()};
                }
            }
            return bsoncxx::types::bson_value::value{view};
        } catch (...) {
            return std::nullopt;
        }
    }

    bsoncxx::types::bson_value::value
    rebuildValue(const bsoncxx::types::bson_value::view& node, const BsonPath& path,
                 const size_t depth, const bsoncxx::types::bson_value::view& replacement) {
        if (depth >= path.size()) {
            return bsoncxx::types::bson_value::value{replacement};
        }

        const auto& seg = path[depth];

        if (node.type() == bsoncxx::type::k_document) {
            bsoncxx::builder::basic::document builder;
            for (auto&& elem : node.get_document().value) {
                if (elem.key() == seg) {
                    if (depth + 1 == path.size()) {
                        builder.append(bsoncxx::builder::basic::kvp(elem.key(), replacement));
                    } else {
                        builder.append(bsoncxx::builder::basic::kvp(
                            elem.key(), rebuildValue(elem.get_value(), path, depth + 1, replacement)));
                    }
                } else {
                    builder.append(bsoncxx::builder::basic::kvp(elem.key(), elem.get_value()));
                }
            }
            return bsoncxx::types::bson_value::value{builder.extract()};
        }

        if (node.type() == bsoncxx::type::k_array) {
            const size_t targetIdx = static_cast<size_t>(std::stoull(seg));
            bsoncxx::builder::basic::array builder;
            size_t idx = 0;
            for (auto&& item : node.get_array().value) {
                if (idx == targetIdx) {
                    if (depth + 1 == path.size()) {
                        builder.append(replacement);
                    } else {
                        builder.append(rebuildValue(item.get_value(), path, depth + 1, replacement));
                    }
                } else {
                    builder.append(item.get_value());
                }
                ++idx;
            }
            return bsoncxx::types::bson_value::value{builder.extract()};
        }

        return bsoncxx::types::bson_value::value{node};
    }

    bsoncxx::types::bson_value::value
    removeAtValue(const bsoncxx::types::bson_value::view& node, const BsonPath& path,
                  const size_t depth, const std::string& removeKey) {
        if (depth >= path.size()) {
            return bsoncxx::types::bson_value::value{node};
        }

        const auto& seg = path[depth];

        if (node.type() == bsoncxx::type::k_document) {
            bsoncxx::builder::basic::document builder;
            for (auto&& elem : node.get_document().value) {
                if (depth + 1 == path.size() && elem.key() == removeKey) {
                    continue;
                }
                if (elem.key() == seg) {
                    builder.append(bsoncxx::builder::basic::kvp(
                        elem.key(), removeAtValue(elem.get_value(), path, depth + 1, removeKey)));
                } else {
                    builder.append(bsoncxx::builder::basic::kvp(elem.key(), elem.get_value()));
                }
            }
            return bsoncxx::types::bson_value::value{builder.extract()};
        }

        if (node.type() == bsoncxx::type::k_array) {
            const size_t targetIdx = static_cast<size_t>(std::stoull(seg));
            bsoncxx::builder::basic::array builder;
            size_t idx = 0;
            for (auto&& item : node.get_array().value) {
                if (depth + 1 == path.size() && std::to_string(idx) == removeKey) {
                    ++idx;
                    continue;
                }
                if (idx == targetIdx) {
                    builder.append(removeAtValue(item.get_value(), path, depth + 1, removeKey));
                } else {
                    builder.append(item.get_value());
                }
                ++idx;
            }
            return bsoncxx::types::bson_value::value{builder.extract()};
        }

        return bsoncxx::types::bson_value::value{node};
    }

    bsoncxx::types::bson_value::value
    renameAtValue(const bsoncxx::types::bson_value::view& node, const BsonPath& path,
                  const size_t depth, const std::string& oldKey, const std::string& newKey) {
        if (depth >= path.size()) {
            return bsoncxx::types::bson_value::value{node};
        }

        const auto& seg = path[depth];

        if (node.type() == bsoncxx::type::k_document) {
            bsoncxx::builder::basic::document builder;
            for (auto&& elem : node.get_document().value) {
                std::string key(elem.key());
                if (depth + 1 == path.size() && key == oldKey) {
                    key = newKey;
                }
                if (elem.key() == seg) {
                    builder.append(bsoncxx::builder::basic::kvp(
                        key, renameAtValue(elem.get_value(), path, depth + 1, oldKey, newKey)));
                } else {
                    builder.append(bsoncxx::builder::basic::kvp(key, elem.get_value()));
                }
            }
            return bsoncxx::types::bson_value::value{builder.extract()};
        }

        if (node.type() == bsoncxx::type::k_array) {
            const size_t targetIdx = static_cast<size_t>(std::stoull(seg));
            bsoncxx::builder::basic::array builder;
            size_t idx = 0;
            for (auto&& item : node.get_array().value) {
                if (idx == targetIdx) {
                    builder.append(
                        renameAtValue(item.get_value(), path, depth + 1, oldKey, newKey));
                } else {
                    builder.append(item.get_value());
                }
                ++idx;
            }
            return bsoncxx::types::bson_value::value{builder.extract()};
        }

        return bsoncxx::types::bson_value::value{node};
    }

    bsoncxx::types::bson_value::value
    addAtValue(const bsoncxx::types::bson_value::view& node, const BsonPath& parentPath,
               const size_t depth, const std::string& key,
               const bsoncxx::types::bson_value::view& newValue) {
        if (depth >= parentPath.size()) {
            if (node.type() != bsoncxx::type::k_document) {
                return bsoncxx::types::bson_value::value{node};
            }
            bsoncxx::builder::basic::document builder;
            for (auto&& elem : node.get_document().value) {
                builder.append(bsoncxx::builder::basic::kvp(elem.key(), elem.get_value()));
            }
            builder.append(bsoncxx::builder::basic::kvp(key, newValue));
            return bsoncxx::types::bson_value::value{builder.extract()};
        }

        const auto& seg = parentPath[depth];

        if (node.type() == bsoncxx::type::k_document) {
            bsoncxx::builder::basic::document builder;
            for (auto&& elem : node.get_document().value) {
                if (elem.key() == seg) {
                    builder.append(bsoncxx::builder::basic::kvp(
                        elem.key(),
                        addAtValue(elem.get_value(), parentPath, depth + 1, key, newValue)));
                } else {
                    builder.append(bsoncxx::builder::basic::kvp(elem.key(), elem.get_value()));
                }
            }
            return bsoncxx::types::bson_value::value{builder.extract()};
        }

        if (node.type() == bsoncxx::type::k_array) {
            const size_t targetIdx = static_cast<size_t>(std::stoull(seg));
            bsoncxx::builder::basic::array builder;
            size_t idx = 0;
            for (auto&& item : node.get_array().value) {
                if (idx == targetIdx) {
                    builder.append(
                        addAtValue(item.get_value(), parentPath, depth + 1, key, newValue));
                } else {
                    builder.append(item.get_value());
                }
                ++idx;
            }
            return bsoncxx::types::bson_value::value{builder.extract()};
        }

        return bsoncxx::types::bson_value::value{node};
    }

    bsoncxx::types::bson_value::view asBsonValueView(const bsoncxx::document::view& doc) {
        return bsoncxx::types::bson_value::view{bsoncxx::types::b_document{doc}};
    }

    bsoncxx::document::value setField(bsoncxx::document::value root, const BsonPath& path,
                                      const bsoncxx::types::bson_value::view& newValue) {
        auto rebuilt = rebuildValue(asBsonValueView(root.view()), path, 0, newValue);
        return bsoncxx::document::value{rebuilt.view().get_document().value};
    }

    bsoncxx::document::value removeField(bsoncxx::document::value root, const BsonPath& path) {
        if (path.empty()) {
            return root;
        }
        BsonPath parent(path.begin(), path.end() - 1);
        const std::string removeKey = path.back();
        auto rebuilt = removeAtValue(asBsonValueView(root.view()), parent, 0, removeKey);
        return bsoncxx::document::value{rebuilt.view().get_document().value};
    }

    bsoncxx::document::value renameField(bsoncxx::document::value root, const BsonPath& path,
                                         const std::string& newKey) {
        if (path.empty()) {
            return root;
        }
        BsonPath parent(path.begin(), path.end() - 1);
        const std::string oldKey = path.back();
        auto rebuilt = renameAtValue(asBsonValueView(root.view()), parent, 0, oldKey, newKey);
        return bsoncxx::document::value{rebuilt.view().get_document().value};
    }

    bsoncxx::document::value addField(bsoncxx::document::value root, const BsonPath& parentPath,
                                    const std::string& key,
                                    const bsoncxx::types::bson_value::view& newValue) {
        auto rebuilt = addAtValue(asBsonValueView(root.view()), parentPath, 0, key, newValue);
        return bsoncxx::document::value{rebuilt.view().get_document().value};
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

    std::string& editableTextBuffer(const ImGuiID id, const std::string& text) {
        static std::unordered_map<ImGuiID, std::string> buffers;
        auto& stored = buffers[id];
        if (ImGui::GetActiveID() != id) {
            stored = text;
        }
        if (stored.capacity() < 256) {
            stored.reserve(256);
        }
        return stored;
    }

    int editableResizeCallback(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* str = static_cast<std::string*>(data->UserData);
            str->resize(static_cast<size_t>(data->BufTextLen));
            data->Buf = str->data();
            data->BufSize = static_cast<int>(str->capacity() + 1);
        }
        return 0;
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

    void pushEditableInputStyle() {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 1.0f));
    }

    void popEditableInputStyle() {
        ImGui::PopStyleVar();
    }

    void renderTypeLabel(const bsoncxx::types::bson_value::view& value,
                         const JsonTreeView::Style& style) {
        const std::string label = bsonTypeLabel(value.type());
        const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - labelWidth + ImGui::GetCursorPosX());
        pushColor(style.typeColor);
        ImGui::TextUnformatted(label.c_str());
        popColor(style.typeColor);
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

    bool renderEditableTextInput(const char* label, const ImGuiID id, const std::string& text,
                                 const unsigned int color,
                                 const std::function<void(const std::string&)>& onCommit) {
        std::string& buffer = editableTextBuffer(id, text);
        pushColor(color);
        pushEditableInputStyle();

        const float textWidth = std::max(ImGui::CalcTextSize(buffer.c_str()).x + 16.0f, 48.0f);
        ImGui::SetNextItemWidth(std::min(textWidth, ImGui::GetContentRegionAvail().x - 40.0f));

        ImGui::PushID(static_cast<int>(id));
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize |
                                          ImGuiInputTextFlags_EnterReturnsTrue;
        const bool enterPressed = ImGui::InputText(label, buffer.data(), buffer.capacity() + 1,
                                                   flags, editableResizeCallback, &buffer);
        const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopID();

        popEditableInputStyle();
        popColor(color);

        if ((enterPressed || deactivated) && buffer != text) {
            onCommit(buffer);
            return true;
        }
        return false;
    }

    bool renderDeleteButton(const JsonTreeView::Style& style) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        const bool clicked = ImGui::SmallButton(ICON_FA_XMARK "##del");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove field");
        }
        (void)style;
        return clicked;
    }

    void renderAddFieldPopup(const BsonPath& parentPath, EditContext& ctx) {
        if (!ImGui::BeginPopup("addFieldPopup")) {
            return;
        }

        static std::unordered_map<ImGuiID, std::string> keyBuffers;
        static std::unordered_map<ImGuiID, std::string> valueBuffers;
        const ImGuiID popupId = ImGui::GetID("addField");
        auto& keyBuf = keyBuffers[popupId];
        auto& valueBuf = valueBuffers[popupId];

        if (keyBuf.capacity() < 128) {
            keyBuf.reserve(128);
        }
        if (valueBuf.capacity() < 256) {
            valueBuf.reserve(256);
        }

        ImGui::TextUnformatted("New field");
        ImGui::InputText("Key", keyBuf.data(), keyBuf.capacity() + 1, ImGuiInputTextFlags_CallbackResize,
                         editableResizeCallback, &keyBuf);
        ImGui::InputText("Value", valueBuf.data(), valueBuf.capacity() + 1,
                         ImGuiInputTextFlags_CallbackResize, editableResizeCallback, &valueBuf);
        ImGui::TextDisabled("Shell syntax: ObjectId(\"...\"), ISODate(\"...\"), etc.");

        if (ImGui::Button("Add") && !trimLiteral(keyBuf).empty()) {
            if (auto parsed = parseShellValue(valueBuf)) {
                *ctx.document = addField(std::move(*ctx.document), parentPath, trimLiteral(keyBuf),
                                         parsed->view());
                ctx.markModified();
                keyBuf.clear();
                valueBuf.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void renderObjectContextMenu(const BsonPath& parentPath, EditContext& ctx,
                                 const bsoncxx::types::bson_value::view& value,
                                 const bool editable) {
        if (!ImGui::BeginPopupContextItem()) {
            return;
        }

        if (ImGui::MenuItem(ICON_FA_COPY " Copy as JSON")) {
            ImGui::SetClipboardText(valueToJson(value).c_str());
        }

        if (editable) {
            if (ImGui::MenuItem(ICON_FA_PLUS " Add field")) {
                ImGui::OpenPopup("addFieldPopup");
            }
        }

        ImGui::EndPopup();
        if (editable) {
            renderAddFieldPopup(parentPath, ctx);
        }
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

    template<typename KeyView>
    void renderEditableKeyLabel(const KeyView& key, const BsonPath& path,
                                const JsonTreeView::Style& style, EditContext& ctx) {
        if (key.empty()) {
            return;
        }

        const std::string keyText(key);
        const ImGuiID id = ImGui::GetID("##editKey");
        renderEditableTextInput("##editKey", id, keyText, style.keyColor,
                                [&](const std::string& newKey) {
                                    if (trimLiteral(newKey).empty() || newKey == keyText) {
                                        return;
                                    }
                                    *ctx.document = renameField(std::move(*ctx.document), path, newKey);
                                    ctx.markModified();
                                });

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
        renderTypeLabel(value, style);
    }

    void renderEditableScalarValue(const bsoncxx::types::bson_value::view& value,
                                   const BsonPath& path, const JsonTreeView::Style& style,
                                   EditContext& ctx) {
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
        const ImGuiID id = ImGui::GetID("##editVal");
        renderEditableTextInput("##editVal", id, display, color, [&](const std::string& newText) {
            if (auto parsed = parseShellValue(newText)) {
                *ctx.document = setField(std::move(*ctx.document), path, parsed->view());
                ctx.markModified();
            }
        });

        if (renderDeleteButton(style)) {
            *ctx.document = removeField(std::move(*ctx.document), path);
            ctx.markModified();
        }

        renderTypeLabel(value, style);
        renderCopyMenu(valueToClipboard(value));
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

    template<typename KeyView>
    void renderEditableBsonValue(const KeyView& key, const bsoncxx::types::bson_value::view& value,
                                 const BsonPath& path, const JsonTreeView::Style& style,
                                 EditContext& ctx) {
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
                    BsonPath childPath = path;
                    childPath.emplace_back(child.key());
                    renderEditableBsonValue(child.key(), child.get_value(), childPath, style, ctx);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            if (!path.empty() && renderDeleteButton(style)) {
                *ctx.document = removeField(std::move(*ctx.document), path);
                ctx.markModified();
            }
            renderTypeLabel(value, style);
            renderObjectContextMenu(path, ctx, value, true);
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
                    BsonPath childPath = path;
                    childPath.emplace_back(std::to_string(index));
                    renderEditableBsonValue(std::to_string(index), item.get_value(), childPath,
                                            style, ctx);
                    ++index;
                }
                ImGui::TreePop();
            }

            if (!path.empty() && renderDeleteButton(style)) {
                *ctx.document = removeField(std::move(*ctx.document), path);
                ctx.markModified();
            }
            renderTypeLabel(value, style);
            renderBranchContextMenu(value);
            break;
        }
        default:
            renderEditableKeyLabel(key, path, style, ctx);
            renderEditableScalarValue(value, path, style, ctx);
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

    bool renderEditableJson(std::string& json, const ImGuiID seedId, EditOptions& edit,
                            const Style* styleOverride) {
        if (!edit.enabled) {
            return renderJson(json, seedId, styleOverride);
        }

        const Style style = styleOverride ? *styleOverride : styleFromAppTheme();
        ImGui::PushID(static_cast<int>(seedId));

        std::optional<bsoncxx::document::value> document;
        try {
            document = bsoncxx::from_json(json);
        } catch (const std::exception&) {
            ImGui::PopID();
            return renderJson(json, seedId, styleOverride);
        }

        EditContext ctx;
        ctx.document = &*document;
        ctx.edit = &edit;

        const size_t fieldCount = countDocumentFields(document->view());
        const std::string rootLabel = std::format("{{ {} fields }}", fieldCount);
        const bool open = ImGui::TreeNodeEx(rootLabel.c_str(), kRootFlags);
        if (open) {
            for (auto&& elem : document->view()) {
                ImGui::PushID(elem.offset());
                BsonPath path;
                path.emplace_back(elem.key());
                renderEditableBsonValue(elem.key(), elem.get_value(), path, style, ctx);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        renderObjectContextMenu({}, ctx,
                                bsoncxx::types::bson_value::view{
                                    bsoncxx::types::b_document{document->view()}},
                                true);

        if (ctx.modified) {
            json = bsoncxx::to_json(document->view());
        }

        ImGui::PopID();
        return true;
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
            const int displayRow = (options ? options->rowNumberOffset : 0) + row + 1;
            const bool dirty =
                options && options->documentDirty &&
                row < static_cast<int>(options->documentDirty->size()) &&
                (*options->documentDirty)[static_cast<size_t>(row)];

            std::string header =
                std::format("Document {}  ·  row {}", i + 1, displayRow);
            if (dirty) {
                header += "  *";
            }

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

    void renderEditableDocumentList(std::vector<std::string>& documents, const ImGuiID seedId,
                                    const DocumentListOptions* options,
                                    const Style* styleOverride) {
        const bool editable = options && options->editable;
        if (!editable) {
            renderDocumentList(documents, seedId, options, styleOverride);
            return;
        }

        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::PushID(static_cast<int>(seedId));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 18.0f);

        for (size_t i = 0; i < documents.size(); ++i) {
            const int row = static_cast<int>(i);
            const bool selected = options && options->selectedRow == row;
            const int displayRow = (options ? options->rowNumberOffset : 0) + row + 1;
            const bool dirty =
                options && options->documentDirty &&
                row < static_cast<int>(options->documentDirty->size()) &&
                (*options->documentDirty)[static_cast<size_t>(row)];

            std::string header =
                std::format("Document {}  ·  row {}", i + 1, displayRow);
            if (dirty) {
                header += "  *";
            }

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

            EditOptions edit;
            edit.enabled = true;
            edit.onModified = [options, row]() {
                if (options && options->onDocumentModified) {
                    options->onDocumentModified(row);
                }
            };
            renderEditableJson(documents[i], ImGui::GetID("docTree"), edit, styleOverride);

            const ImVec2 blockMax = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const unsigned int borderColor =
                dirty ? ImGui::ColorConvertFloat4ToU32(colors.peach)
                      : ImGui::ColorConvertFloat4ToU32(colors.surface1);
            drawList->AddRect(ImVec2(blockMin.x, blockMin.y - 2.0f),
                              ImVec2(blockMin.x + blockWidth, blockMax.y + 6.0f), borderColor, 4.0f);

            if (i + 1 < documents.size()) {
                ImGui::Dummy(ImVec2(0, 6));
            }

            ImGui::PopID();
        }

        ImGui::PopStyleVar(2);
        ImGui::PopID();
    }

    std::string documentToShellLiteral(const bsoncxx::document::view& doc) {
        return valueToShellLiteral(
            bsoncxx::types::bson_value::view{bsoncxx::types::b_document{doc}});
    }
} // namespace JsonTreeView
