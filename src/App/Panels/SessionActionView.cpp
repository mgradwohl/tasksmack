#include "SessionActionView.h"

#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/SessionActionControl.h"
#include "Platform/IUserSessions.h"
#include "UI/IconsFontAwesome6.h"

#include <imgui.h>

namespace App
{

SessionMenuChoice renderSessionMenuItems(SessionMenu menu, const Platform::UserSession& session)
{
    SessionMenuChoice choice;
    // While an action runs a second one is refused (#4260), so the items are greyed, with the reason.
    const bool enabled = !menu.busy;
    if (menu.lock && ImGui::MenuItem(ICON_FA_LOCK "  Lock", nullptr, false, enabled))
    {
        choice.lock = session;
    }
    if (menu.lock)
    {
        ImGui::SetItemTooltip(enabled ? "Lock the session's screen; its user signs back in to unlock it"
                                      : SessionActionControl::BUSY_TEXT.data());
    }
    // Windows' Disconnect, in Lock's place (#3076): the session stays signed in, its programs running.
    const bool disconnected = session.state == "disconnected";
    if (menu.disconnect && ImGui::MenuItem(ICON_FA_LINK_SLASH "  Disconnect...", nullptr, false, enabled && !disconnected))
    {
        choice.disconnect = session;
    }
    if (menu.disconnect)
    {
        const char* const idle = disconnected ? "The session is already disconnected"
                                              : "Detach the session from its screen; its programs keep running until its user "
                                                "signs back in";
        ImGui::SetItemTooltip(enabled ? idle : SessionActionControl::BUSY_TEXT.data());
    }
    if (menu.signOut && ImGui::MenuItem(ICON_FA_RIGHT_FROM_BRACKET "  Sign out...", nullptr, false, enabled))
    {
        choice.signOut = session;
    }
    if (menu.signOut)
    {
        ImGui::SetItemTooltip(enabled ? "End the session and every program in it" : SessionActionControl::BUSY_TEXT.data());
    }
    return choice;
}

void startSessionChoice(SessionActionControl& actions, const SessionMenuChoice& choice)
{
    if (choice.lock.has_value())
    {
        actions.lock(*choice.lock);
    }
    if (choice.signOut.has_value())
    {
        actions.requestSignOut(*choice.signOut);
    }
    if (choice.disconnect.has_value())
    {
        actions.requestDisconnect(*choice.disconnect);
    }
}

void renderSessionConfirm(SessionActionControl& actions)
{
    if (!actions.pending().has_value())
    {
        return;
    }
    const bool signOut = actions.pendingAction() == Platform::SessionAction::SignOut;
    const char* const title = sessionActionWords(actions.pendingAction()).title;
    // Its own ID scope: the Processes table's row actions use the same modal, which must not take this one
    // for theirs (#3377).
    ImGui::PushID("##SessionConfirm");
    const ProcessActionConfirm::Outcome outcome =
        ProcessActionConfirm::renderLabelled(actions.showConfirm(), title, signOut, title, actions.question());
    ImGui::PopID();
    switch (outcome)
    {
    case ProcessActionConfirm::Outcome::Confirmed:
        actions.confirm();
        break;
    case ProcessActionConfirm::Outcome::Cancelled:
        actions.cancel();
        break;
    case ProcessActionConfirm::Outcome::None:
        break;
    }
}

} // namespace App
