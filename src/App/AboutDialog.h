#pragma once

#include "UI/IconLoader.h"

#include <string_view>

namespace App::AboutDialog
{

/// The About dialog's popup ID (and its title).
inline constexpr const char* POPUP_ID = "About TaskSmack";

/// Draw the About dialog for this frame: open it when @p openRequested is set (and clear it), then
/// draw it while it is open. OK or Escape closes it.
///
/// Split out of AboutLayer, which keeps only the event wiring and the icon's texture, so the dialog
/// can be rendered headless in the tests (CONTRIBUTING.md, "Testing App/UI code that needs a live
/// ImGui context").
///
/// Besides the version and credits it shows the user guide and "Report a problem" links, the same ones
/// as the Help window (#1600).
///
/// @param openRequested  Set by an OpenAboutEvent (the title bar's "i", Help's "About TaskSmack..."
///                       link, Settings); cleared here.
/// @param icon           The application icon; an invalid texture leaves its space empty.
/// @return The URL of the link the user activated this frame, for the caller to open with the system
///         handler; empty when none was. Returned rather than opened here so the tests can press a
///         link without launching a browser.
[[nodiscard]] std::string_view render(bool& openRequested, const UI::Texture& icon);

} // namespace App::AboutDialog
