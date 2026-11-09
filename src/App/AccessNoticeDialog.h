#pragma once

// The Linux startup note (#2728, #3580): a session is Administrator for its whole life or Standard for its whole
// life, so a Standard one is told once, when its window is up, that some data is limited and how to start an
// Administrator session ("TaskSmack (Administrator)", or sudo). "What does Administrator add?" expands over the
// access table (AccessTable.h). OK, or Escape, closes it; "Don't show this again" stops it at startup, and the
// status bar's Standard chip opens it again.
//
// Only draws and reports: ElevationNoticeLayer opens it and saves the setting. Headless-testable
// (tests/App/test_AccessNoticeDialogRender.cpp).

#include "App/AccessChoice.h"

#include <string_view>

namespace App::AccessNoticeDialog
{

/// The note's popup ID (and its title).
inline constexpr const char* POPUP_ID = "TaskSmack is running as Standard";
/// Its one button.
inline constexpr const char* OK_LABEL = "OK";
/// The expander over the access table.
inline constexpr const char* TABLE_HEADER = "What does Administrator add?";
/// What Standard means.
inline constexpr std::string_view STANDARD_TEXT =
    "TaskSmack runs as you, so some of other users' processes and some system details can't be read, and some actions "
    "are refused.";
/// How to start an Administrator session: never from a running one.
inline constexpr std::string_view START_ADMINISTRATOR_TEXT =
    "To see and act on everything, close TaskSmack and start TaskSmack (Administrator) from your applications menu, or "
    "run it with sudo. A session never becomes Administrator after it starts.";

/// What the note keeps between frames.
struct State
{
    bool openRequested = false; ///< Set to open it on the next frame; cleared when it opens
    AccessChoice::AccessPlatform platform = AccessChoice::HOST;
    bool dontShowAgain = false; ///< "Don't show this again": stops it at startup
    // The viewport size it was last centred for, and its height then: either changing re-centres it.
    float centredViewportWidth = 0.0F;
    float centredViewportHeight = 0.0F;
    float dialogHeight = 0.0F;
    float centredDialogHeight = 0.0F;
};

/// Sets @p state up to open on the next frame.
void requestOpen(State& state);

/// Draws the note this frame, opening it when requested; true on the frame OK or Escape closed it (dontShowAgain
/// then says whether to stop showing it).
[[nodiscard]] bool render(State& state);

/// The note's width with a viewport @p viewportWidthPx wide: its authored width, held to the viewport. Needs a
/// frame (it is in ems of the current font).
[[nodiscard]] float computeWidth(float viewportWidthPx);

} // namespace App::AccessNoticeDialog
