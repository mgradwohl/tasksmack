#pragma once

#include "UI/IconLoader.h"

#include <string_view>

namespace App::AboutDialog
{

/// The About dialog's popup ID (and its title).
inline constexpr const char* POPUP_ID = "About TaskSmack";

/// Draw the About dialog for this frame: open it when @p openRequested is set (and clear it), then
/// draw it while it is open. OK, Escape or @p closeRequested (cleared here too) closes it.
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
/// @param closeRequested Set by a CloseDialogEvent (TASKSMACK_DEMO's close=dialog, #2827); cleared
///                       here, whether or not the dialog was open.
/// @param icon           The application icon; an invalid texture leaves its space empty.
/// @param linksBusy      An open the caller started for a link is still running (#3155): the links are
///                       drawn disabled, so a second click opens nothing more until it is done.
/// @return The URL of the link the user activated this frame, for the caller to open with the system
///         handler; empty when none was. Returned rather than opened here so the tests can press a
///         link without launching a browser.
[[nodiscard]] std::string_view render(bool& openRequested, bool& closeRequested, const UI::Texture& icon, bool linksBusy = false);

} // namespace App::AboutDialog
