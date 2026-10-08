#pragma once

#include "UI/IconLoader.h"

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
/// @param openRequested  Set by an OpenAboutEvent (F1, the title bar's ? button); cleared here.
/// @param icon           The application icon; an invalid texture leaves its space empty.
void render(bool& openRequested, const UI::Texture& icon);

} // namespace App::AboutDialog
