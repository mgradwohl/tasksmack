#pragma once

// The status bar along the bottom of the window (#1547): the limited-data and blocked-network icons,
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

/// What the bar shows this frame.
struct Input
{
    bool reducedPrivileges = false;      ///< The lock icon: some details of other users' processes are unavailable
    bool networkCountersBlocked = false; ///< The network icon: per-process network counters are blocked (#1358)
    std::string_view processText;        ///< "N processes", with its glyph; empty to leave it out
    std::string_view intervalText;       ///< "Updates every 1 s", with its glyph; empty to leave it out
    bool showControls = false;           ///< Settings and Help, when the custom title bar isn't there
    std::optional<float> fps;            ///< The frame-rate readout, while Render Metrics is on
    float frameTimeSeconds = 0.0F;       ///< Shown beside it, in milliseconds
};

/// What the user asked for this frame.
enum class Action : std::uint8_t
{
    None,
    Settings,
    Help,
};

/// Draw the status bar for this frame, fitted to the window: at its narrowest the lower-priority
/// parts are left out, or the process count cut short, rather than drawn over each other (#1207).
[[nodiscard]] Action render(const Input& input);

} // namespace App::StatusBarView
