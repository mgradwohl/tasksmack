#pragma once

#include <imgui.h>

namespace UI::Widgets
{

/// Draw right-aligned text overlay on the previous ImGui item (e.g., plot, progress bar).
/// Renders text with a shadow for visibility against varying backgrounds.
/// @param text The text to display (null or empty is a no-op)
/// @param paddingX Distance from the right edge in pixels (default: 8.0)
inline void drawRightAlignedOverlayText(const char* text, float paddingX = 8.0F)
{
    if (text == nullptr || text[0] == '\0')
    {
        return;
    }

    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    const ImVec2 textSize = ImGui::CalcTextSize(text);

    const float x = rectMax.x - paddingX - textSize.x;
    const float y = rectMin.y + ((rectMax.y - rectMin.y - textSize.y) * 0.5F);
    const ImVec2 pos(x, y);

    const ImU32 shadowCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 1.0F, pos.y + 1.0F), shadowCol, text);
    ImGui::GetWindowDrawList()->AddText(pos, textCol, text);
}

} // namespace UI::Widgets
