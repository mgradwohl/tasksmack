#pragma once

#include "UI/ColorContrast.h"

#include <imgui.h>

#include <algorithm>

namespace UI::Widgets
{

/// Minimum height in pixels for bar fill rendering.
/// Ensures at least a 1px marker remains visible even when the value is 0%,
/// providing visual feedback that the bar exists and is capable of showing data.
constexpr float MIN_BAR_FILL_HEIGHT = 1.0F;

/// The three fills a filledButton() is drawn with.
struct ButtonFills
{
    ImVec4 resting;
    ImVec4 hovered;
    ImVec4 pressed;
};

/// A button with its own fill colours whose label stays readable in every state.
///
/// ImGui::Button() takes one text colour for all three of its states, but a themed fill moves
/// between them -- and not consistently: some themes lighten the fill on hover, others darken it.
/// One label colour therefore cannot be right for all three; measured across the bundled themes,
/// sixteen of twenty had at least one state where the Apply label was under 3:1 contrast, several
/// of them around 1.1 on hover, the state the user is in when about to click (#969).
///
/// So the button is submitted without a label and the label is drawn afterwards, in whichever of
/// the two candidate colours reads better on the fill actually showing (see
/// ColorContrast::readableTextOn()).
///
/// @param label           Visible label; also the button's ID. Drawn verbatim, so no "##" suffix.
/// @param size            As for ImGui::Button(). A zero width fits the label.
/// @param fills           The button's fill in each state.
/// @param textPreferred   Label colour to use unless the alternate is clearly more readable.
/// @param textAlternate   The other candidate; the theme's window background is a good choice.
/// @return true when clicked, as ImGui::Button() does.
inline bool
filledButton(const char* label, const ImVec2& size, const ButtonFills& fills, const ImVec4& textPreferred, const ImVec4& textAlternate)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    // The button is submitted with no visible label, so a zero size is resolved here against the
    // real label, the way ImGui::Button() would have: a zero width would otherwise collapse, and
    // the height should not depend on what ImGui measures for an empty string.
    const ImVec2 buttonSize((size.x != 0.0F) ? size.x : (labelSize.x + (style.FramePadding.x * 2.0F)),
                            (size.y != 0.0F) ? size.y : (labelSize.y + (style.FramePadding.y * 2.0F)));

    ImGui::PushStyleColor(ImGuiCol_Button, fills.resting);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fills.hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, fills.pressed);
    ImGui::PushID(label);
    const bool clicked = ImGui::Button("##filled", buttonSize);
    ImGui::PopID();
    ImGui::PopStyleColor(3);

    // The same rule ImGui::Button() uses to pick its fill, so the label is judged against the
    // colour that was actually drawn.
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec4& shown = (held && hovered) ? fills.pressed : (hovered ? fills.hovered : fills.resting);

    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    const ImVec2 textPos(rectMin.x + (((rectMax.x - rectMin.x) - labelSize.x) * 0.5F),
                         rectMin.y + (((rectMax.y - rectMin.y) - labelSize.y) * 0.5F));
    // GetColorU32(ImVec4) applies the style's alpha, so a button inside BeginDisabled() gets a
    // dimmed label like any other.
    const ImU32 textColor = ImGui::GetColorU32(ColorContrast::readableTextOn(shown, textPreferred, textAlternate));
    // Clipped to the button, as ImGui::Button() clips its own label: a caller may cap the width
    // below the label's (the priority panel does, on a narrow pane), and an unclipped label would
    // then be drawn over whatever is beside the button.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(rectMin, rectMax, true);
    drawList->AddText(textPos, textColor, label);
    drawList->PopClipRect();

    return clicked;
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
