#pragma once

// "Restart as administrator..." beside a refused action or read where there is no helper (#3658, #2728's
// design). Windows' "Retry as administrator" and "Read as administrator" go through TaskSmackHelper.exe,
// which HelperLauncher::availability() refuses for a copy the user can change (the .zip, a per-user folder)
// and for a build without it. There, a refusal used to offer nothing; now it offers the restart, which
// opens the startup notice (Core::OpenElevationNoticeEvent) and its Restart as administrator..., the same
// restart as the status-bar chip's (Platform/IElevatedRestart.h).
//
// Offered only once main.cpp says so (setOffered()): TaskSmack can restart itself elevated (Windows, not
// elevated, not a synthetic run) and has no helper. Linux never offers it: nothing restarts a Linux session
// (#3580), so starting TaskSmack (Administrator) is the way there. Everything here runs on the UI thread.
// A refusal that could also offer "Retry as administrator" decides between the two, and draws them, through
// RefusalOffer.h (#3812).

#include "App/Panels/AuthorizeWording.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "UI/IconsFontAwesome6.h"

#include <imgui.h>

#include <functional>
#include <string_view>
#include <utility>

namespace App::RestartAsAdministrator
{

/// The button's label: the lock, then the platform wording's switch (AuthorizeWording::WINDOWS_WORDING).
inline constexpr const char* LABEL = ICON_FA_LOCK "  Restart as administrator...";
static_assert(std::string_view(LABEL).ends_with(AuthorizeWording::WINDOWS_WORDING.switchSession),
              "the per-item button says what the chip's notice says");

// NOLINTBEGIN(cppcoreguidelines-macro-usage) - a string literal the two tooltips are joined from
#define TASKSMACK_RESTART_TOOLTIP_INTRO                                                                                                    \
    "This copy of TaskSmack has no helper to do it as an administrator (a portable copy, or one you can change). Restarts "                \
    "TaskSmack as administrator: "
// NOLINTEND(cppcoreguidelines-macro-usage)

/// Its tooltip: why there is no Retry as administrator here, and what the button does, then the prompt line: approval
/// (UAC), or for a standard user an administrator's name and password (#4108).
inline constexpr const char* TOOLTIP = TASKSMACK_RESTART_TOOLTIP_INTRO "Windows asks for approval (UAC)";
inline constexpr const char* CREDENTIALS_TOOLTIP =
    TASKSMACK_RESTART_TOOLTIP_INTRO "Windows asks for an administrator's name and password (UAC)";
static_assert(std::string_view(TOOLTIP).ends_with(AuthorizeWording::WINDOWS_WORDING.promptTooltip) &&
                  std::string_view(CREDENTIALS_TOOLTIP).ends_with(AuthorizeWording::WINDOWS_WORDING.credentialsPromptTooltip),
              "the per-item button says what the notice's Restart button says");
#undef TASKSMACK_RESTART_TOOLTIP_INTRO

/// What a press is handed: Core::OpenElevationNoticeEvent, for the application to raise.
using RaiseEvent = std::function<void(Core::Event&)>;

namespace Detail
{
/// The offer: set by setOffered(), empty while it isn't offered.
[[nodiscard]] inline RaiseEvent& offer()
{
    static RaiseEvent raise;
    return raise;
}

/// Whether the prompt asks for an administrator's name and password: set by setCredentials().
[[nodiscard]] inline bool& credentialsPrompt()
{
    static bool credentials = false;
    return credentials;
}
} // namespace Detail

/// Offers the button from now on; a press raises Core::OpenElevationNoticeEvent through @p raise. main.cpp
/// sets it once, before the panels draw, when TaskSmack can restart elevated and has no helper; a test sets
/// its own and empties it after. Empty: not offered.
inline void setOffered(RaiseEvent raise)
{
    Detail::offer() = std::move(raise);
}

/// Says what the restart's prompt will ask for (#4108): an administrator's name and password for a standard user,
/// else approval. ElevationNoticeLayer sets it once, from the restart's prompt() it reads for the notice's own
/// note; the tooltip only reads it, so nothing is asked of Windows while drawing.
inline void setCredentials(bool credentials) noexcept
{
    Detail::credentialsPrompt() = credentials;
}

/// The tooltip for a prompt that asks for credentials (@p credentials), else for approval.
[[nodiscard]] constexpr const char* tooltipFor(bool credentials) noexcept
{
    return credentials ? CREDENTIALS_TOOLTIP : TOOLTIP;
}

/// The tooltip for the prompt setCredentials() was told of.
[[nodiscard]] inline const char* tooltip() noexcept
{
    return tooltipFor(Detail::credentialsPrompt());
}

/// Whether a refused item offers the button: this TaskSmack can restart elevated and has no helper.
[[nodiscard]] inline bool offered()
{
    return static_cast<bool>(Detail::offer());
}

/// Whether a refusal offers it (#3658): refused for want of permission, the helper can't redo it (there is
/// none), and the restart is @p restartOffered (offered()).
[[nodiscard]] constexpr bool offersRestart(bool refusedForPermission, bool helperCanRedo, bool restartOffered) noexcept
{
    return refusedForPermission && !helperCanRedo && restartOffered;
}

/// The press: Core::OpenElevationNoticeEvent, raised through the offer. Does nothing when it isn't offered.
inline void requestRestart()
{
    if (const RaiseEvent& raise = Detail::offer(); raise)
    {
        Core::OpenElevationNoticeEvent open;
        raise(open);
    }
}

/// Draws the button, with its tooltip, and raises the notice when it is pressed. The caller decides when it
/// is offered (offersRestart()). True when it was pressed this frame.
inline bool drawButton()
{
    const bool pressed = ImGui::SmallButton(LABEL);
    ImGui::SetItemTooltip("%s", tooltip());
    if (pressed)
    {
        requestRestart();
    }
    return pressed;
}

} // namespace App::RestartAsAdministrator
