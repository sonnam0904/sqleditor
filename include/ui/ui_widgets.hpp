#pragma once

#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <string>

namespace UIWidgets {

inline bool iconToolButton(const char* id, const char* icon, const ImVec4& iconColor,
                           const bool enabled = true) {
    const auto& colors = Application::getInstance().getCurrentColors();
    if (!enabled) {
        ImGui::BeginDisabled();
    }

    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface2);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_Text, iconColor);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f, 7.0f));
    const bool pressed = ImGui::Button(icon);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::PopID();

    if (!enabled) {
        ImGui::EndDisabled();
    }
    return pressed && enabled;
}

inline void beginSegmentedControl(const char* id) {
    const auto& colors = Application::getInstance().getCurrentColors();
    const float height = ImGui::GetFrameHeight() + Theme::Spacing::XS;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    ImGui::BeginChild(id, ImVec2(0, height),
                      ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AlwaysAutoResize);
}

inline void endSegmentedControl() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

inline bool segmentedItem(const char* label, const bool selected) {
    const auto& colors = Application::getInstance().getCurrentColors();
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface2);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface2);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.text);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 5.0f));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    return clicked;
}

inline void statusBadge(const char* text, const ImVec4& bg, const ImVec4& fg) {
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    constexpr float padX = 8.0f;
    constexpr float padY = 3.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(textSize.x + padX * 2.0f, textSize.y + padY * 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(bg), 8.0f);
    dl->AddText(ImVec2(pos.x + padX, pos.y + padY), ImGui::GetColorU32(fg), text);
    ImGui::Dummy(size);
}

} // namespace UIWidgets
