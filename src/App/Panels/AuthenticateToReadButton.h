#pragma once

// The "Read as administrator" button (#2309) that Process Details' Connections, Modules, Open files, Memory map,
// Threads and Environment sections draw under a read refused for want of permission, on Windows, through its
// helper (Linux has none since #3580): the lock and the words (AuthorizeWording.h), its tooltip, and the press
// handed to the view, which reads as an administrator on its next update() (LazyBackgroundRead::requestAuthorized()).
// One function for all of them (#2567), so the flow changes in one place; each view still decides when it is
// offered.
// Where there is no helper to read as an administrator (Windows' .zip, or a copy the user can change),
// a refused read offers "Restart as administrator..." instead (#3658, RestartAsAdministratorButton.h).

#include "AuthorizeWording.h"
#include "RefusalOffer.h"
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
///
/// @p idScope names the section (a string literal, such as "modules"), and the button is drawn inside it
/// (ImGui::PushID()): the sections all draw in one Process Details child, and the button's label is the same
/// in each, so without it two refused sections share one ID and a press on the second does nothing (#4268).
template<AuthenticatingView View> void drawAuthenticateToReadButton(View& view, const char* idScope)
{
    ImGui::PushID(idScope);
    // offersAuthenticateToRead() is the helper's say: it offers the read only for a refusal it can redo.
    const bool offersRead = view.offersAuthenticateToRead();
    const RefusalOfferKind kind = decideRefusalOffer(
        view.readRefusedForPermission() || offersRead, /*retryable=*/true, offersRead, RestartAsAdministrator::offered());
    static_cast<void>(drawRefusalOffer(
        kind,
        [&view] { view.requestAuthorizedRead(); },
        RefusalOfferButton{
            .retryLabel = AuthorizeWording::READ_LABEL,
            .retryTooltip = AuthorizeWording::READ_TOOLTIP,
        }));
    ImGui::PopID();
}

} // namespace App::Detail
