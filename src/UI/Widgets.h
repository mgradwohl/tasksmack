#pragma once

#include <imgui.h>

#include <algorithm>

namespace UI::Widgets
{

/// Minimum height in pixels for bar fill rendering.
/// Ensures at least a 1px marker remains visible even when the value is 0%,
/// providing visual feedback that the bar exists and is capable of showing data.
constexpr float MIN_BAR_FILL_HEIGHT = 1.0F;

/// Draw right-aligned text overlay on the previous ImGui item (e.g., plot, progress bar).
/// Note: ImGui requires null-terminated const char*; std::string_view wouldn't add value here.
/// Shadow-free to avoid double-vision; relies on theme contrast instead.
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

    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::GetWindowDrawList()->AddText(pos, textCol, text);
}

/// Draw a vertical bar (bottom-up fill) with the value and optional label centered underneath.
/// The overall allocated height stays equal to barHeight; the bar shrinks to leave room for text.
/// Colors must be provided by the caller (theme-sourced).
inline void drawVerticalBarWithValue(const char* id,
                                     float value01,
                                     const ImVec4& color,
                                     float barHeight,
                                     float barWidth,
                                     const char* valueText,
                                     const char* labelText = nullptr,
                                     const char* tooltipText = nullptr)
{
    value01 = std::clamp(value01, 0.0F, 1.0F);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float valueTextH = (valueText != nullptr && valueText[0] != '\0') ? ImGui::GetTextLineHeight() : 0.0F;
    const float labelTextH = (labelText != nullptr && labelText[0] != '\0') ? ImGui::GetTextLineHeight() : 0.0F;
    const float textBlockH = valueTextH + labelTextH + ((valueTextH > 0.0F && labelTextH > 0.0F) ? style.ItemInnerSpacing.y : 0.0F);
    const float availableBarH = (textBlockH > 0.0F) ? std::max(0.0F, barHeight - textBlockH - style.ItemInnerSpacing.y) : barHeight;

    const ImVec2 barSize(barWidth, availableBarH);
    const ImVec2 barPos = ImGui::GetCursorScreenPos();
    const ImVec2 barEnd(barPos.x + barSize.x, barPos.y + barSize.y);

    ImGui::InvisibleButton(id, ImVec2(barWidth, barHeight));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bgCol = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 barCol = ImGui::ColorConvertFloat4ToU32(color);

    dl->AddRectFilled(barPos, barEnd, bgCol, style.FrameRounding);
    const ImU32 borderCol = ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddRect(barPos, barEnd, borderCol, style.FrameRounding);

    if (barSize.y > 0.0F)
    {
        const float filledH = barSize.y * value01;
        const ImVec2 filledMin(barPos.x, barEnd.y - filledH);
        const float clampedFilledH = std::max(filledH, MIN_BAR_FILL_HEIGHT);
        const ImVec2 visibleMin(barPos.x, barEnd.y - clampedFilledH);
        dl->AddRectFilled(visibleMin, barEnd, barCol, style.FrameRounding, ImDrawFlags_RoundCornersBottom);
    }

    float textY = barEnd.y + style.ItemInnerSpacing.y;
    if (valueText != nullptr && valueText[0] != '\0')
    {
        const ImVec2 sz = ImGui::CalcTextSize(valueText);
        const float x = barPos.x + ((barWidth - sz.x) * 0.5F);
        dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(ImGuiCol_Text), valueText);
        textY += valueTextH + style.ItemInnerSpacing.y;
    }

    if (labelText != nullptr && labelText[0] != '\0')
    {
        const ImVec2 sz = ImGui::CalcTextSize(labelText);
        const float x = barPos.x + ((barWidth - sz.x) * 0.5F);
        dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), labelText);
    }

    const char* tooltip = (tooltipText != nullptr && tooltipText[0] != '\0') ? tooltipText : valueText;
    if (tooltip != nullptr && tooltip[0] != '\0' && ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }
}

inline void drawVerticalBarWithValue(const char* id,
                                     double value01,
                                     const ImVec4& color,
                                     float barHeight,
                                     float barWidth,
                                     const char* valueText,
                                     const char* labelText = nullptr,
                                     const char* tooltipText = nullptr)
{
    const double clamped = std::clamp(value01, 0.0, 1.0);
    drawVerticalBarWithValue(id,
                             static_cast<float>(clamped), // Narrowing: UI geometry uses float; value is clamped to [0,1]
                             color,
                             barHeight,
                             barWidth,
                             valueText,
                             labelText,
                             tooltipText);
}

} // namespace UI::Widgets
