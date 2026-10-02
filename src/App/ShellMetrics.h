#pragma once

// Padding the shell and its panels push over the ImGuiStyle values, authored at the reference
// configuration (the Medium preset on a 1.0 display scale).
//
// #942 made the style itself scale with the font preset and the display's density, but these were
// pushed over it as pixel literals at each call site, so the tab bars, the content gutter and the
// status bar kept one size while everything around them scaled (#971). They live here, in one
// place, so each site multiplies the same authored value by UI::Theme::styleScale() and the main
// tab bar's indent cannot drift from the content gutter it is meant to line up with.

namespace App::ShellMetrics
{

/// Horizontal padding inside a tab, for the main tab bar and the panels' sub-tab bars alike.
inline constexpr float TAB_PADDING_X = 16.0F;

/// Vertical padding inside a main tab. Deliberately more than a sub-tab's, so the main bar reads as
/// the primary level.
inline constexpr float MAIN_TAB_PADDING_Y = 10.0F;

/// Vertical padding inside a sub-tab (system tab: Overview / CPU Cores / ...; process details).
inline constexpr float SUB_TAB_PADDING_Y = 8.0F;

/// Gutter either side of the content area. The main tab bar is indented by the same amount so its
/// first tab lines up with the panel content beneath it.
inline constexpr float CONTENT_PADDING_H = 12.0F;

/// Padding above and below the content area.
inline constexpr float CONTENT_PADDING_V = 4.0F;

/// Space between the title bar and the main tab bar.
inline constexpr float TOP_EDGE_PADDING = 4.0F;

/// Horizontal padding inside the status bar.
inline constexpr float STATUS_BAR_PADDING_X = 8.0F;

} // namespace App::ShellMetrics
