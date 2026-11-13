#pragma once

// The startup notice, Standard or Administrator (#2728). TaskSmack always starts as Standard; the notice says so
// once its window is up, and the status bar's Standard chip opens it again. "What does Administrator add?"
// expands over the access table (AccessTable.h), and "Don't show this again" stops it at startup.
//
//  - Linux (#3580): a session is Administrator for its whole life or Standard for its whole life, so the note
//    says how to start an Administrator session ("TaskSmack (Administrator)", or sudo). OK, or Escape, closes it.
//  - Windows: Administrator is TaskSmack restarted elevated. Continue as Standard (the default, and Escape)
//    closes it; Restart as administrator... asks for it (Action::RestartAsAdministrator), with a note on what
//    the UAC prompt will ask for. While Windows asks the button waits; a declined prompt, or a restart that
//    failed, leaves the notice up, as Standard, with a line saying so. A run that can't restart (a synthetic
//    one) shows the button disabled, with the reason.
//
// Only draws and reports: ElevationNoticeLayer opens it, restarts TaskSmack and saves the setting.
// Headless-testable (tests/App/test_AccessNoticeDialogRender.cpp).

#include "App/AccessChoice.h"
#include "App/Panels/AuthorizeWording.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace App::AccessNoticeDialog
{

/// The note's popup ID (and its title).
inline constexpr const char* POPUP_ID = "TaskSmack is running as Standard";
/// Linux: its one button.
inline constexpr const char* OK_LABEL = "OK";
/// Windows: its default button, and its Administrator one.
inline constexpr const char* CONTINUE_LABEL = "Continue as Standard";
inline constexpr const char* RESTART_LABEL = AuthorizeWording::WINDOWS_WORDING.switchSession;
/// The expander over the access table.
inline constexpr const char* TABLE_HEADER = "What does Administrator add?";
/// What Standard means.
inline constexpr std::string_view STANDARD_TEXT =
    "TaskSmack runs as you, so some of other users' processes and some system details can't be read, and some actions "
    "are refused.";
/// Linux: how to start an Administrator session: never from a running one.
inline constexpr std::string_view START_ADMINISTRATOR_TEXT =
    "To see and act on everything, close TaskSmack and start TaskSmack (Administrator) from your applications menu, or "
    "run it with sudo. A session never becomes Administrator after it starts.";
/// Windows: what Administrator does.
inline constexpr std::string_view RESTART_ADMINISTRATOR_TEXT =
    "Administrator restarts TaskSmack elevated, with your settings, so nothing is limited. TaskSmack always starts "
    "as Standard.";
/// Windows: what the prompt will ask for.
inline constexpr std::string_view CONSENT_NOTE = "Windows asks for approval (UAC).";
inline constexpr std::string_view CREDENTIALS_NOTE =
    "Windows asks for approval (UAC): an administrator's name and password will be needed.";
/// Windows: while the prompt is up.
inline constexpr std::string_view WAITING_NOTE = "Waiting for Windows' approval...";
/// Windows: the prompt was dismissed.
inline constexpr std::string_view DECLINED_NOTE =
    "Windows didn't restart TaskSmack as administrator, so it carries on as Standard. Nothing is lost.";
/// Windows: a run that can't restart itself.
inline constexpr const char* RESTART_UNAVAILABLE = "This run of TaskSmack can't restart itself as administrator.";

/// Windows: the line for a restart that failed with @p detail (a sentence fragment, or a system message, whose
/// closing full stop and white space are dropped).
[[nodiscard]] inline std::string restartFailedNote(std::string_view detail)
{
    while (!detail.empty() && (detail.back() == '.' || detail.back() == ' ' || detail.back() == '\r' || detail.back() == '\n'))
    {
        detail.remove_suffix(1);
    }
    return std::string("TaskSmack couldn't restart as administrator: ").append(detail).append(". It carries on as Standard.");
}

/// Windows: where the Administrator choice stands.
enum class Restart : std::uint8_t
{
    Unavailable, ///< This run can't restart (a synthetic run): the button is disabled, with the reason
    Ready,       ///< Offered
    Waiting,     ///< Asked for: Windows' prompt is up
};

/// What the note keeps between frames.
struct State
{
    bool openRequested = false; ///< Set to open it on the next frame; cleared when it opens
    AccessChoice::AccessPlatform platform = AccessChoice::HOST;
    bool dontShowAgain = false; ///< "Don't show this again": stops it at startup
    // Windows' Administrator choice.
    Restart restart = Restart::Unavailable;
    bool credentials = false; ///< The prompt asks for an administrator's name and password (a standard user)
    std::string outcome;      ///< The last restart's: declined, or why it failed; empty for none
    // The viewport size it was last centred for, and its height then: either changing re-centres it.
    float centredViewportWidth = 0.0F;
    float centredViewportHeight = 0.0F;
    float dialogHeight = 0.0F;
    float centredDialogHeight = 0.0F;
};

/// What the user did this frame.
enum class Action : std::uint8_t
{
    None,
    Closed,                 ///< OK, Continue as Standard or Escape: dontShowAgain says whether to stop showing it
    RestartAsAdministrator, ///< Windows: Restart as administrator... (the notice stays up while Windows asks)
};

/// Sets @p state up to open on the next frame, its last outcome cleared.
void requestOpen(State& state);

/// Draws the note this frame, opening it when requested.
[[nodiscard]] Action render(State& state);

/// The note's width with a viewport @p viewportWidthPx wide: its authored width, held to the viewport. Needs a
/// frame (it is in ems of the current font).
[[nodiscard]] float computeWidth(float viewportWidthPx);

} // namespace App::AccessNoticeDialog
