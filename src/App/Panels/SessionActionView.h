#pragma once

// A signed-in session's menu and its confirm (#3076), drawn by every view that lists sessions: the Users tab
// and the Processes table's Group: User (#3377). The menu's items are what the platform can do (SessionMenu):
// Lock and Sign out... on Linux, Disconnect... and Sign out... on Windows. Lock runs at once; Sign out and
// Disconnect ask first, through SessionActionControl, in the confirm drawn here.

#include "App/Panels/SessionActionControl.h"
#include "Platform/IUserSessions.h"

#include <optional>

namespace App
{

/// Which items a session's row menu has: what the platform's IUserSessions::canAct() allows.
struct SessionMenu
{
    bool lock = false;       ///< Linux
    bool disconnect = false; ///< Windows
    bool signOut = false;

    [[nodiscard]] bool any() const noexcept
    {
        return lock || disconnect || signOut;
    }
};

/// The item chosen in a session's menu, for the session it was opened on; none set when none was.
struct SessionMenuChoice
{
    std::optional<Platform::UserSession> lock;       ///< Lock
    std::optional<Platform::UserSession> signOut;    ///< Sign out...
    std::optional<Platform::UserSession> disconnect; ///< Disconnect... (Windows)

    [[nodiscard]] bool any() const noexcept
    {
        return lock.has_value() || signOut.has_value() || disconnect.has_value();
    }
};

/// Inside an open popup: the items @p menu allows for @p session, Lock, Disconnect... (greyed for a session
/// already disconnected) and Sign out..., each with its tooltip. What was chosen, if anything.
[[nodiscard]] SessionMenuChoice renderSessionMenuItems(SessionMenu menu, const Platform::UserSession& session);

/// Hands @p choice to @p actions: Lock runs at once; Sign out and Disconnect open their confirm.
void startSessionChoice(SessionActionControl& actions, const SessionMenuChoice& choice);

/// Sign out's or Disconnect's confirm while one is pending, and the action once confirmed or cancelled.
/// Sign out loses unsaved work, so its button is the destructive one; a Disconnect keeps every program.
void renderSessionConfirm(SessionActionControl& actions);

} // namespace App
