#pragma once

// The title bar's app buttons' labels and tooltips (#1600), named once so the headless tests check
// what TitleBarLayer draws. Their positions are computeTitleBarButtonLayout() (TitleBarGeometry.h).

#include "UI/IconsFontAwesome6.h"

#include <string>

namespace App::TitleBarButtons
{

/// The circle-i: opens About (Core::OpenAboutEvent).
inline constexpr const char* ABOUT_LABEL = ICON_FA_CIRCLE_INFO "##About";
/// The circle-?: opens the Help window (Core::OpenHelpEvent), as F1 does.
inline constexpr const char* HELP_LABEL = ICON_FA_CIRCLE_QUESTION "##Help";
/// The gear: opens Settings (Core::OpenSettingsEvent), as F2 does.
inline constexpr const char* SETTINGS_LABEL = ICON_FA_GEAR "##Settings";
/// The circled up-arrow (#3373): active while a newer release is out, when it opens that release's page
/// (Core::OpenUpdatePageEvent), as the status bar's notice does; inactive otherwise.
inline constexpr const char* UPDATE_LABEL = ICON_FA_CIRCLE_ARROW_UP "##Update";

// Drawn in the body font after the chrome icon font is popped: that font has no letters (#1200).
// About has no keyboard shortcut, so its tooltip names none.
inline constexpr const char* ABOUT_TOOLTIP = "About TaskSmack";
inline constexpr const char* HELP_TOOLTIP = "Help (F1)";
inline constexpr const char* SETTINGS_TOOLTIP = "Settings (F2)";

// The update button's tooltips (#3373), but for the active one's, Domain::UpdateCheck::noticeText():
// "TaskSmack v0.11.0 is available".
inline constexpr const char* UPDATE_UP_TO_DATE_TOOLTIP = "TaskSmack is up to date";
inline constexpr const char* UPDATE_CHECKING_TOOLTIP = "Checking for updates\xE2\x80\xA6";
inline constexpr const char* UPDATE_OFF_TOOLTIP = "The update check is off: turn on \"Check for updates daily\" in Settings (F2)";
inline constexpr const char* UPDATE_UNKNOWN_TOOLTIP = "TaskSmack couldn't check for updates yet";

/// What the update button shows (#3373): App::UpdateChecker::titleBarButton() decides it, ShellLayer
/// hands it to TitleBarLayer when it changes, and TitleBarView draws it.
struct UpdateButton
{
    std::string tooltip; ///< One of the tooltips above, or the notice's text while active
    std::string url;     ///< The release page the active button opens; empty otherwise
    bool shown = false;  ///< Only where the update check is offered (an HTTPS client, a comparable version)
    bool active = false; ///< A newer release is out: the button can be clicked

    friend bool operator==(const UpdateButton&, const UpdateButton&) = default;
};

} // namespace App::TitleBarButtons
