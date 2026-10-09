#pragma once

// A card: a bordered child window in the theme's child background and separator colour. The CPU
// Cores and per-disk chart grids draw each cell as one (ChartGrid.h), and the Process Details
// Overview draws its Identity, Runtime and Actions sections as three (#1537), so every grouped area
// looks the same.

#include "UI/Theme.h"

#include <imgui.h>

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

} // namespace UI::Widgets
