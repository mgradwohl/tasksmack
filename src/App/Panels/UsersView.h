#pragma once

// The Users tab's table (#3076): a row per signed-in user, as Windows Task Manager's Users tab has, with
// their process count and the Σ of their processes' CPU, memory, disk and network rates (the Processes
// table's group totals, ProcessGrouping::Totals), and under it, expanded, their sessions and their
// processes. A session's row menu offers what the platform can do (SessionMenu): Lock and Sign out... on
// Linux, Disconnect... and Sign out... on Windows (UsersPanel runs them).
//
// The rows are built from two published generations, the sessions' (UsersModel) and the processes'
// (ProcessModel), and rebuilt only when either changes. The rows and the session text come from
// UserGrouping (buildUserRows(), sessionPlace(), ...), pure and tested without ImGui (test_UserGrouping.cpp).

#include "App/Panels/UserGrouping.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/UsersModel.h"
#include "Platform/IUserSessions.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

/// What the user did in the table this frame.
struct UsersViewAction
{
    std::int32_t selectPid = 0; ///< A process row was clicked: select it (0 for none)
    std::uint64_t selectStartTimeTicks = 0;
    bool showDetails = false;                        ///< Double-clicked: show it in Process Details too
    std::optional<Platform::UserSession> lock;       ///< The row menu's Lock
    std::optional<Platform::UserSession> signOut;    ///< The row menu's Sign out...
    std::optional<Platform::UserSession> disconnect; ///< The row menu's Disconnect... (Windows)
};

/// The table's state between frames.
struct UsersViewState
{
    std::uint64_t builtSessionsVersion = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t builtProcessesVersion = std::numeric_limits<std::uint64_t>::max();
    std::vector<UserGrouping::UserRow> rows;
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> processes; ///< The generation `rows` index
    /// The session the row menu was opened on, copied at the right-click: the rows can be rebuilt while it
    /// is up (as the Processes table's, #1365).
    std::optional<Platform::UserSession> menuSession;
};

/// Brings @p state's rows up to date with @p publication and @p processes (either may be newer).
void refreshUserRows(const Domain::UsersPublication& publication,
                     const std::shared_ptr<const std::vector<Domain::ProcessSnapshot>>& processes,
                     std::uint64_t processesVersion,
                     UsersViewState& state);

/// Draws the table for @p publication (null: nothing sampled yet). A session's row menu has @p menu's items,
/// and none when it has none.
[[nodiscard]] std::optional<UsersViewAction>
renderUsersView(const Domain::UsersPublication* publication, bool hasSessions, SessionMenu menu, UsersViewState& state);

} // namespace App
