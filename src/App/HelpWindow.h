#pragma once

#include "App/HelpContent.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace App::HelpWindow
{

/// The window's title and ImGui ID.
inline constexpr const char* WINDOW_ID = "Help###TaskSmackHelp";
/// The footer link that opens About (#172).
inline constexpr const char* ABOUT_LINK_LABEL = "About TaskSmack\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
/// The shortcut filter's ImGui ID, inside the window's body child.
inline constexpr const char* FILTER_ID = "##ShortcutFilter";
/// The scrolling body's ImGui ID.
inline constexpr const char* BODY_ID = "##HelpBody";

/// What the user asked for this frame beyond the window itself.
enum class Action : std::uint8_t
{
    None,
    OpenAbout, ///< "About TaskSmack..." was clicked
};

/// The window's state, owned by HelpLayer.
struct State
{
    bool open = false;
    bool focusRequested = false;
    std::array<char, 128> filter{};
    /// The shortcuts the filter keeps: recomputed only when the filter is edited, not every frame.
    HelpContent::ShortcutMask visible = HelpContent::matchingShortcuts({});
    std::size_t visibleCount = KeyboardShortcuts::SHORTCUT_HELP.size();
    /// Shortcut rows drawn on the last frame the window was visible (for the headless tests).
    std::size_t shortcutRowsDrawn = 0;
};

/// Opens the window (F1, the ? buttons) and brings it to the front, or only to the front when it is
/// already open; its size, position and filter are kept for the session.
void requestOpen(State& state) noexcept;

/// Draws the Help window for this frame when it is open: a resizable, non-modal window with the
/// keyboard shortcuts (grouped by area, with a filter), the tab overview, the Processes column
/// reference and links, and a footer link to About. Its title bar's close button or Escape (while it
/// has focus and no text field is being typed in) closes it.
///
/// Rendered headless in the tests (test_HelpWindowRender.cpp), as the About dialog is.
[[nodiscard]] Action render(State& state);

} // namespace App::HelpWindow
