#pragma once

// The startup notice, Standard or Administrator (#2728): TaskSmack always starts as Standard, and this
// says so once its window is up, offers Administrator in the platform's own words, and expands "What
// does Administrator add?" over the access table (AccessTable.h). Continue as Standard is the default,
// and Escape chooses it. The Administrator option is disabled, with the reason, where the decision
// (AccessChoice::decideNotice()) says so, and while its prompt is up.
//
// Only draws and reports: ElevationNoticeLayer runs the choice (on Linux, the session-held helper's
// password prompt, on a worker) and reports how it went with finishAdministrator(). Headless-testable
// (tests/App/test_AccessNoticeDialogRender.cpp).

#include "App/AccessChoice.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace App::AccessNoticeDialog
{

/// The notice's popup ID (and its title).
inline constexpr const char* POPUP_ID = "Standard or Administrator";
/// Its default button.
inline constexpr const char* CONTINUE_LABEL = "Continue as Standard";
/// The expander over the access table.
inline constexpr const char* TABLE_HEADER = "What does Administrator add?";
/// Shown while the Administrator prompt is up.
inline constexpr std::string_view WAITING_TEXT = "Waiting for the administrator's approval...";
/// Shown when the prompt was dismissed or refused (the issue's "one-line note"), before the reason.
inline constexpr std::string_view DECLINED_TEXT = "Still Standard: ";

/// What the notice keeps between frames.
struct State
{
    bool openRequested = false; ///< Set to open it on the next frame; cleared when it opens
    AccessChoice::AccessPlatform platform = AccessChoice::HOST;
    AccessChoice::NoticeDecision decision;
    bool dontShowAgain = false;  ///< "Don't show this again": hides the notice; TaskSmack still starts as Standard
    bool authenticating = false; ///< The Administrator prompt is up; the choices wait for it
    bool closeRequested = false; ///< finishAdministrator() succeeded: closed on the next frame
    std::string note;            ///< The one-line note under the choices: why Administrator didn't happen
    // The viewport size it was last centred for, and its height then: either changing re-centres it.
    float centredViewportWidth = 0.0F;
    float centredViewportHeight = 0.0F;
    float dialogHeight = 0.0F;
    float centredDialogHeight = 0.0F;
};

enum class Choice : std::uint8_t
{
    None,          ///< Nothing this frame
    Standard,      ///< Continue as Standard (or Escape): closed; dontShowAgain says whether to stop showing it
    Administrator, ///< The Administrator button: the caller starts it and reports with finishAdministrator()
};

/// Sets @p state up to open on the next frame with @p decision, forgetting any earlier note.
void requestOpen(State& state, const AccessChoice::NoticeDecision& decision);

/// Draws the notice this frame, opening it when requested; what the user chose.
[[nodiscard]] Choice render(State& state);

/// How the Administrator choice went: succeeded closes the notice; otherwise it stays open as
/// Standard, with "Still Standard: " and @p why as its note.
void finishAdministrator(State& state, bool succeeded, std::string_view why);

/// The notice's width with a viewport @p viewportWidthPx wide: its authored width, held to the
/// viewport. Needs a frame (it is in ems of the current font).
[[nodiscard]] float computeWidth(float viewportWidthPx);

} // namespace App::AccessNoticeDialog
