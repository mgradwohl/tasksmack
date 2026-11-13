#pragma once

// "Restart as administrator..." beside a refused action or read where there is no helper (#3658, #2728's
// design). Windows' "Retry as administrator" and "Read as administrator" go through TaskSmackHelper.exe,
// which HelperLauncher::availability() refuses for a copy the user can change (the .zip, a per-user folder)
// and for a build without it. There, a refusal used to offer nothing; now it offers the restart, which
// opens the startup notice (Core::OpenElevationNoticeEvent) and its Restart as administrator..., the same
// restart as the status-bar chip's (Platform/IElevatedRestart.h).
//
// Offered only once main.cpp says so (setOffered()): TaskSmack can restart itself elevated (Windows, not
// elevated, not a synthetic run) and has no helper. Linux never offers it: its helper's own prompt, or
// "Authenticate for this session", is the way there. Everything here runs on the UI thread.

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

/// Its tooltip: why there is no Retry as administrator here, and what the button does.
inline constexpr const char* TOOLTIP = "This copy of TaskSmack has no helper to do it as an administrator (a portable copy, or one "
                                       "you can change). Restarts TaskSmack as administrator: Windows asks for approval (UAC)";

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
} // namespace Detail

/// Offers the button from now on; a press raises Core::OpenElevationNoticeEvent through @p raise. main.cpp
/// sets it once, before the panels draw, when TaskSmack can restart elevated and has no helper; a test sets
/// its own and empties it after. Empty: not offered.
inline void setOffered(RaiseEvent raise)
{
    Detail::offer() = std::move(raise);
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
    ImGui::SetItemTooltip("%s", TOOLTIP);
    if (pressed)
    {
        requestRestart();
    }
    return pressed;
}

} // namespace App::RestartAsAdministrator
