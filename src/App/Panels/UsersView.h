#pragma once

// The Users tab's table (#3076): a row per signed-in user, as Windows Task Manager's Users tab has, with
// their process count and the Σ of their processes' CPU, memory, disk and network rates (the Processes
// table's group totals, ProcessGrouping::Totals), and under it, expanded, their sessions and their
// processes. A session's row menu offers what the platform can do (SessionMenu): Lock and Sign out... on
// Linux, Disconnect... and Sign out... on Windows (UsersPanel runs them).
//
// The rows are built from two published generations, the sessions' (UsersModel) and the processes'
// (ProcessModel), and rebuilt only when either changes. buildUserRows() and the text helpers are pure and
// tested without ImGui (test_Users.cpp).

#include "App/Panels/ProcessGrouping.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/UsersModel.h"
#include "Platform/IUserSessions.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace App
{

/// One signed-in user: their sessions, and their processes' totals.
struct UserRow
{
    std::string user;  ///< The short name the Processes table's User column has; empty for a session not readable
    std::string key;   ///< What the row groups by (Platform::accountKey()): "PC\\alice" and "CORP\\alice" are two rows
    std::string label; ///< What the row says: the short name, "CORP\\alice" when two rows share it, or "Not readable"
    std::uint32_t uid = 0;
    std::vector<Platform::UserSession> sessions; ///< In the publication's order
    ProcessGrouping::Totals totals;              ///< Over every process running as them
    /// Indices into the process generation, busiest first. A process names only its user's short name, so
    /// when two rows share one (PC\\alice and CORP\\alice) neither gets that name's processes: they can't be
    /// told apart until the process probe keeps the domain (#3310).
    std::vector<std::size_t> processes;
    bool current = false; ///< One of their sessions is TaskSmack's own
};

/// Whether @p session is a person's sign-in (logind class "user"), not a greeter, lock screen or the user
/// manager's own background session, which the tab leaves out.
[[nodiscard]] bool isSignInSession(const Platform::UserSession& session);

/// A row per account with a sign-in session (by domain and user name on Windows), sorted by its label, each
/// with the processes running as them. A session whose user couldn't be read is a row of its own.
[[nodiscard]] std::vector<UserRow> buildUserRows(const std::vector<Platform::UserSession>& sessions,
                                                 const std::vector<Domain::ProcessSnapshot>& processes);

/// Where and how a session is: "seat0 · tty2 · Wayland", "pts/0 · from 10.0.0.9".
[[nodiscard]] std::string sessionPlace(const Platform::UserSession& session);

/// What a session is doing at @p nowUsec (CLOCK_REALTIME µs): "Active", "Idle for 12 min", "Signed in",
/// "Closing", and on Windows "Disconnected" and "Connecting".
[[nodiscard]] std::string sessionStatus(const Platform::UserSession& session, std::uint64_t nowUsec);

/// A session row's tooltip: the user with their domain (Windows), when they signed in, and the session's
/// ID: "PC\alice\nSigned in 09:12:04\nSession 2".
[[nodiscard]] std::string sessionTooltip(const Platform::UserSession& session);

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
    std::vector<UserRow> rows;
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
