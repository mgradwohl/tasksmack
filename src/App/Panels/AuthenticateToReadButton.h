#pragma once

// The "Authenticate to read" button (#2309) that Process Details' Connections, Modules, Open files, Threads and
// Environment sections draw under a read refused for want of permission: the lock and the words
// (AuthorizeWording.h), its tooltip, and the press handed to the view, which reads as an
// administrator on its next update() (LazyBackgroundRead::requestAuthorized()). One helper for the
// four (#2567), so the flow changes in one place; each view still decides when it is offered.
// Where there is no helper to read as an administrator (Windows' .zip, or a copy the user can change),
// a refused read offers "Restart as administrator..." instead (#3658, RestartAsAdministratorButton.h).

#include "AuthorizeWording.h"
#include "RestartAsAdministratorButton.h"

#include <imgui.h>

#include <concepts>

namespace App::Detail
{

/// A section view that can offer "Authenticate to read": it says when the button is offered, takes
/// the press, and says when its last read was refused for want of permission (for the restart, #3658).
template<typename View>
concept AuthenticatingView = requires(View& view, const View& constView) {
    { constView.offersAuthenticateToRead() } -> std::convertible_to<bool>;
    { constView.readRefusedForPermission() } -> std::convertible_to<bool>;
    view.requestAuthorizedRead();
};

/// Draws the button when @p view offers it (offersAuthenticateToRead(): the last read was refused for
/// want of permission, the reader can read as an administrator, and no read is in flight), and hands
/// the press to @p view (requestAuthorizedRead()). A refused read with no helper to read it offers
/// "Restart as administrator..." instead, where RestartAsAdministrator::offered() (#3658). Draws nothing
/// otherwise.
template<AuthenticatingView View> void drawAuthenticateToReadButton(View& view)
{
    const bool offersRead = view.offersAuthenticateToRead();
    if (RestartAsAdministrator::offersRestart(view.readRefusedForPermission(), offersRead, RestartAsAdministrator::offered()))
    {
        static_cast<void>(RestartAsAdministrator::drawButton());
        return;
    }
    if (!offersRead)
    {
        return;
    }
    if (ImGui::SmallButton(AuthorizeWording::READ_LABEL))
    {
        view.requestAuthorizedRead();
    }
    ImGui::SetItemTooltip("%s", AuthorizeWording::READ_TOOLTIP);
}

} // namespace App::Detail
