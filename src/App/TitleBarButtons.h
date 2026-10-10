#pragma once

// The title bar's app buttons' labels and tooltips (#1600), named once so the headless tests check
// what TitleBarLayer draws. Their positions are computeTitleBarButtonLayout() (TitleBarGeometry.h).

#include "UI/IconsFontAwesome6.h"

namespace App::TitleBarButtons
{

/// The circle-i: opens About (Core::OpenAboutEvent).
inline constexpr const char* ABOUT_LABEL = ICON_FA_CIRCLE_INFO "##About";
/// The circle-?: opens the Help window (Core::OpenHelpEvent), as F1 does.
inline constexpr const char* HELP_LABEL = ICON_FA_CIRCLE_QUESTION "##Help";
/// The gear: opens Settings (Core::OpenSettingsEvent), as F2 does.
inline constexpr const char* SETTINGS_LABEL = ICON_FA_GEAR "##Settings";

// Drawn in the body font after the chrome icon font is popped: that font has no letters (#1200).
// About has no keyboard shortcut, so its tooltip names none.
inline constexpr const char* ABOUT_TOOLTIP = "About TaskSmack";
inline constexpr const char* HELP_TOOLTIP = "Help (F1)";
inline constexpr const char* SETTINGS_TOOLTIP = "Settings (F2)";

} // namespace App::TitleBarButtons
