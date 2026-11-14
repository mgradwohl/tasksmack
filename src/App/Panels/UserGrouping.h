#pragma once

// Who a process or a session belongs to (#3377): the account matching the Users tab's rows (#3076, #3310)
// and the Processes table's Group: User share, and the text a session is shown and asked about with. On
// Windows an account is its domain and user name, compared ignoring ASCII case as Windows compares them, so
// PC\alice and CORP\alice are two accounts; on Linux, with no domain, it is the user name, as before.
//
// Pure, free of ImGui and of the panels' state, so it is tested on its own (test_UserGrouping.cpp). The Users
// tab draws these rows (UsersView), and the table's Group: User is built here (ProcessGrouping::buildGroups()
// calls buildUserGroups()), each signed-in account's sessions under its header, as the Users tab had them.

#include "App/Panels/ProcessGroupTotals.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/IUserSessions.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace App::UserGrouping
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
    /// Indices into the process generation, busiest first. On Windows a process names its owner's domain too,
    /// so PC\\alice's and CORP\\alice's processes go to their own rows (#3310). A process that names only a
    /// short name goes to the one row with that name, and to neither when two rows share it.
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

/// The account @p proc names, as Group: User keys it: "pc\\alice" (domain and user, ASCII case folded, as
/// Windows compares them) for a process that names its owner's domain, else its user name as it is (Linux,
/// or a Windows owner whose domain wasn't read); empty when its user couldn't be read.
[[nodiscard]] std::string processAccountKey(const Domain::ProcessSnapshot& proc);

/// The short names two or more accounts in @p snapshots share (#3328): "alice" when PC\\alice and CORP\\alice
/// both have a process. Accounts and names compare ignoring ASCII case, as buildUserGroups() counts them, and
/// only a process that names its domain counts, so on Linux there are none. Sorted ignoring ASCII case, for
/// isSharedUserName(); the Processes table's User column writes DOMAIN\\user for these names only.
[[nodiscard]] std::vector<std::string> sharedUserNames(const std::vector<Domain::ProcessSnapshot>& snapshots);

/// Whether @p user is one of @p shared (sharedUserNames()), ignoring ASCII case.
[[nodiscard]] bool isSharedUserName(const std::vector<std::string>& shared, std::string_view user) noexcept;

/// The key of Group: User's group of sessions whose user couldn't be read (Windows, #3377): no account's name
/// starts with a control character, so no account's group can have it.
inline constexpr std::string_view NOT_READABLE_GROUP_KEY = "\x1Fnot readable";

/// What the row under a session-only group says while the Processes filter is on (#3377 decision): the account
/// still shows, its header and its sessions, with this under them.
inline constexpr std::string_view NO_MATCHING_PROCESSES = "No processes match the filter";

/// Group: User's groups of @p order (indices into @p snapshots, already filtered and sorted), as
/// ProcessGrouping::buildGroups() builds the other modes': a group per account (processAccountKey()), so
/// PC\\alice's and CORP\\alice's processes are two groups (#3377). A process that names no domain joins the
/// one account with its name, as buildUserRows() gives it to the one row, and is grouped by its name alone
/// when none or two have it. A group is labelled with the short name, or DOMAIN\\user when two groups share
/// the name, spelled as its lowest-numbered process spells it. Names compare ignoring ASCII case wherever an
/// account has a domain, so the table's order never changes the groups (#3377 review). A member at or past @p liveCount is an exited row
/// (#3060): listed, not counted.
///
/// The fold of the Users tab (#3377): each account with a sign-in session in @p sessions (isSignInSession())
/// carries them, in their order, and has a group even when none of its processes is in @p order (one the
/// filter hides, or one TaskSmack can't see): so a filter never hides who is signed in. An account found
/// only through a session is labelled as the session names it. Sessions whose user couldn't be read go to one
/// "Not readable" group (NOT_READABLE_GROUP_KEY). Ordered by ProcessGrouping::sortGroups(): the signed-in
/// accounts first, then the others, then Not readable, then Ungrouped (a user not read).
[[nodiscard]] std::vector<ProcessGrouping::Group> buildUserGroups(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                                                  const std::vector<std::size_t>& order,
                                                                  std::size_t liveCount,
                                                                  ProcessColumn sortColumn,
                                                                  bool ascending,
                                                                  const std::vector<Platform::UserSession>& sessions);

/// What a Group: User header's name says (#3377): its label, and "  (you)" when one of its sessions is
/// TaskSmack's own.
[[nodiscard]] std::string groupHeaderName(const ProcessGrouping::Group& group);

/// A Group: User header's Status cell at @p nowUsec (#3377): its one session's sessionStatus(), "2 sessions",
/// or empty for an account with none.
[[nodiscard]] std::string groupStatus(const ProcessGrouping::Group& group, std::uint64_t nowUsec);

/// A group header's tooltip (#3377): "Expand" or "Collapse" (@p collapsed), and for a Group: User account
/// with sessions that its Σ totals are of the processes TaskSmack can see, or for Not readable, why each
/// session's user couldn't be read.
/// @p othersHidden: /proc is mounted with hidepid (Platform::MissingPrivilege::PROCESSES_HIDDEN), so another signed-in
/// account's processes aren't listed: its header says so, rather than showing 0 as if it ran none (#3377 PR 4).
[[nodiscard]] std::string groupHeaderTooltip(const ProcessGrouping::Group& group, bool collapsed, bool othersHidden);

/// The session a Group: User header's menu acts on (#3377): the account's one sign-in session, readable;
/// null with two or more (each has its own row's menu), or none.
[[nodiscard]] const Platform::UserSession* headerMenuSession(const ProcessGrouping::Group& group) noexcept;

/// A session row's name (#3377, as the Users tab's): its place (sessionPlace()), or "Session 3 (not
/// readable)", then "  (this session)" for TaskSmack's own.
[[nodiscard]] std::string sessionRowName(const Platform::UserSession& session);

/// When @p session was signed in to, as the Start time column writes a date and time; empty when the
/// platform didn't say.
[[nodiscard]] std::string sessionSignInTime(const Platform::UserSession& session);

/// Where and how a session is: "seat0 · tty2 · Wayland", "pts/0 · from 10.0.0.9".
[[nodiscard]] std::string sessionPlace(const Platform::UserSession& session);

/// What a session is doing at @p nowUsec (CLOCK_REALTIME µs): "Active", "Idle for 12 min", "Signed in",
/// "Closing", and on Windows "Disconnected" and "Connecting".
[[nodiscard]] std::string sessionStatus(const Platform::UserSession& session, std::uint64_t nowUsec);

/// A session row's tooltip: the user with their domain (Windows), when they signed in, and the session's
/// ID: "PC\alice\nSigned in 09:12:04\nSession 2".
[[nodiscard]] std::string sessionTooltip(const Platform::UserSession& session);

/// The Sign out confirm's question for @p session.
[[nodiscard]] std::string signOutQuestion(const Platform::UserSession& session);

/// The Disconnect confirm's question for @p session (#3076, Windows).
[[nodiscard]] std::string disconnectQuestion(const Platform::UserSession& session);

} // namespace App::UserGrouping
