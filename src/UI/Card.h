#pragma once

// A card: a bordered child window in the theme's child background and separator colour. The CPU
// Cores and per-disk chart grids draw each cell as one (ChartGrid.h), and the Process Details
// Overview draws its Identity, Runtime and Actions sections as three (#1537), so every grouped area
// looks the same.

#include "UI/Theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>

namespace UI::Widgets
{

/// A card's child flags: bordered, which also gives it the style's window padding on every side.
inline constexpr ImGuiChildFlags CARD_CHILD_FLAGS = ImGuiChildFlags_Borders;

/// A card's window flags. A card is sized to fit its content and is never meant to scroll: a few
/// pixels of residual layout overhead clip invisibly rather than surface a scrollbar (#823 review).
inline constexpr ImGuiWindowFlags CARD_WINDOW_FLAGS = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

/// Begins a card @p size big (ImGui::BeginChild()'s size rules) with @p extraChildFlags added to
/// CARD_CHILD_FLAGS (ImGuiChildFlags_AutoResizeY for a card as tall as its content). The colours stay
/// pushed until endCard(), as the chart grid's cells have always had them. Returns BeginChild()'s
/// result: whether the card is visible. Call endCard() either way.
[[nodiscard]] inline bool beginCard(const char* id, const ImVec2& size, ImGuiChildFlags extraChildFlags = ImGuiChildFlags_None)
{
    const auto& theme = Theme::get();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);
    return ImGui::BeginChild(id, size, CARD_CHILD_FLAGS | extraChildFlags, CARD_WINDOW_FLAGS);
}

/// Ends the card beginCard() began.
inline void endCard()
{
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

/// beginCard() with the frame padding above and below instead of the window padding, for a section
/// card in a stack of them (System › Overview, Process Details' Overview and Network and I/O): their
/// charts already sit at their minimum height at large fonts in a short pane, so the cards' chrome must
/// not push the last one off. Call endCard() either way.
[[nodiscard]] inline bool beginCompactCard(const char* id, const ImVec2& size, ImGuiChildFlags extraChildFlags = ImGuiChildFlags_None)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, style.FramePadding.y));
    const bool visible = beginCard(id, size, extraChildFlags);
    ImGui::PopStyleVar(); // The child keeps the padding it was begun with
    return visible;
}

/// Begins a full-width compact card around one chart of a fill layout (UI::Widgets::FillPlotLayout),
/// @p plotHeight being the chart's height this frame. Call endChartCard() with the same arguments
/// either way, then FillPlotLayout::addPlot() in the panel's coordinates, so the fill layout counts the
/// card's chrome as non-chart height.
///
/// Not ImGuiChildFlags_AutoResizeY: that sizes a child from its content of the previous frame, so the
/// card lagged its chart by a frame. The fill layout measures what the frame used, so it read that lag
/// as non-chart height and corrected for it, which the next frame's lag undid: the chart heights cycled
/// every six frames and never settled, overflowing the pane in some frames and leaving it short in
/// others. Instead the card is its chrome (padding and heading, everything but the chart) as measured
/// last frame, which does not depend on the chart's height, plus the chart's height this frame. On the
/// first frame, with nothing measured, the chrome is taken to be the usual one: the compact padding and
/// a one-line heading (UI::Widgets::sectionHeader()) above the chart. An auto-sized first frame would
/// have been a card of padding alone, and the fill layout's next frame would have overflowed by every
/// card's heading.
[[nodiscard]] inline bool beginChartCard(const char* id, float plotHeight)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float usualChrome = (2.0F * style.FramePadding.y) + ImGui::GetTextLineHeightWithSpacing();
    const float chrome = ImGui::GetStateStorage()->GetFloat(ImGui::GetID(id), usualChrome);
    return beginCompactCard(id, ImVec2(-FLT_MIN, chrome + plotHeight));
}

/// Ends the card beginChartCard() began, measuring its chrome for the next frame.
inline void endChartCard(const char* id, float plotHeight)
{
    const ImGuiWindow* card = ImGui::GetCurrentWindow();
    const bool drawn = !card->SkipItems; // A card that drew nothing has nothing to measure
    const float content = card->DC.CursorMaxPos.y - card->DC.CursorStartPos.y;
    const float chrome = content + (2.0F * card->WindowPadding.y) - plotHeight;
    endCard();
    if (drawn)
    {
        ImGui::GetStateStorage()->SetFloat(ImGui::GetID(id), std::max(chrome, 0.0F));
    }
}

} // namespace UI::Widgets
