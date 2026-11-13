#pragma once

// The status bar along the bottom of the window (#1547): the access chip and the blocked-network icon,
// the process count and update interval, Settings and Help when native decorations replace the
// custom title bar (#745), and the frame-rate readout while Render Metrics is on. ShellLayer gathers
// the inputs and carries out the returned action, so the bar can be rendered headless in the tests.

#include "UI/IconsFontAwesome6.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace App::StatusBarView
{

/// The Settings and Help buttons' labels (the glyph, and an ID of its own), named once: their widths
/// are measured before they are drawn, so the text beside them can make room (#1200).
inline constexpr const char* SETTINGS_LABEL = ICON_FA_GEAR "##StatusBarSettings";
inline constexpr const char* HELP_LABEL = ICON_FA_CIRCLE_QUESTION "##StatusBarHelp";

/// The access chip (#2728): what the session is, Standard or Administrator.
enum class AccessChip : std::uint8_t
{
    Standard,      ///< A lock and "Standard": clicking it offers Administrator (Action::Access)
    Administrator, ///< A shield and "Administrator": nothing is limited
};

/// The chip's labels, glyph and words, each with an ID of its own.
inline constexpr const char* STANDARD_CHIP_LABEL = ICON_FA_LOCK " Standard##AccessChip";
inline constexpr const char* ADMINISTRATOR_CHIP_LABEL = ICON_FA_SHIELD_HALVED " Administrator##AccessChip";

/// The update notice's ID (#3093): its label is "<glyph>  TaskSmack vX.Y.Z is available" followed by this,
/// so the button keeps one ID whatever version it names. Its Dismiss button, beside it.
inline constexpr const char* UPDATE_NOTICE_ID = "###UpdateNotice";
inline constexpr const char* DISMISS_UPDATE_LABEL = ICON_FA_XMARK "##DismissUpdate";

/// What the bar shows this frame.
struct Input
{
    /// The access chip (#2728), which both platforms show; nullopt draws none (a test's bar).
    std::optional<AccessChip> access;
    bool networkCountersBlocked = false; ///< The network icon: per-process network counters are blocked (#1358)
    std::string_view processText;        ///< "N processes", with its glyph; empty to leave it out
    std::string_view intervalText;       ///< "Updates every 1 s", with its glyph; empty to leave it out
    bool showControls = false;           ///< Settings and Help, when the custom title bar isn't there
    std::optional<float> fps;            ///< The frame-rate readout, while Render Metrics is on
    float frameTimeSeconds = 0.0F;       ///< Shown beside it, in milliseconds
    /// The update notice's label (#3093), ending in UPDATE_NOTICE_ID; null for none. Clicking it is
    /// Action::OpenUpdate, its X Action::DismissUpdate.
    const char* updateNotice = nullptr;
    /// The release page an elevated TaskSmack didn't open (#3658, BackgroundOpen::withheld()): the notice's
    /// tooltip names it, and its click (Action::OpenUpdate) copies it. Empty for none.
    std::string_view updateWithheld;
};

/// What the user asked for this frame.
enum class Action : std::uint8_t
{
    None,
    Settings,
    Help,
    Access,        ///< The Standard chip: offer Administrator (the startup notice again)
    OpenUpdate,    ///< The update notice (#3093): open its release page
    DismissUpdate, ///< The notice's X: don't offer that version again
};

/// Draw the status bar for this frame, fitted to the window: at its narrowest the lower-priority
/// parts are left out, or the process count cut short, rather than drawn over each other (#1207).
[[nodiscard]] Action render(const Input& input);

} // namespace App::StatusBarView
