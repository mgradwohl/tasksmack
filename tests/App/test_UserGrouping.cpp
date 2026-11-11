/// @file test_UserGrouping.cpp
/// @brief Who a process or a session belongs to (#3377): the Users tab's rows, one per signed-in account
/// with its sessions and its processes' totals (#3076, #3310), the session text, and the Processes table's
/// Group: User, keyed by account so PC\alice and CORP\alice are two groups.

#include "App/Panels/ProcessGroupTotals.h"
#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/UserGrouping.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/IUserSessions.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace App::UserGrouping
{
namespace
{

using Platform::UserSession;

[[nodiscard]] UserSession session(std::string id, std::string user, std::uint32_t uid, std::string state = "active")
{
    UserSession s;
    s.id = std::move(id);
    s.user = std::move(user);
    s.uid = uid;
    s.state = std::move(state);
    s.sessionClass = "user";
    return s;
}

[[nodiscard]] Domain::ProcessSnapshot process(std::int32_t pid, std::string user, double cpu, std::uint64_t memory)
{
    Domain::ProcessSnapshot p;
    p.pid = pid;
    p.name = "proc" + std::to_string(pid);
    p.user = std::move(user);
    p.cpuPercent = cpu;
    p.memoryBytes = memory;
    p.ioReadBytesPerSec = 10.0;
    p.ioWriteBytesPerSec = 5.0;
    p.netSentBytesPerSec = 2.0;
    p.netReceivedBytesPerSec = 3.0;
    return p;
}

TEST(UserRowsTest, OneRowPerSignedInUserWithTheirSessionsAndProcessTotals)
{
    UserSession greeter = session("c1", "gdm", 120);
    greeter.sessionClass = "greeter";
    UserSession aliceDesktop = session("2", "alice", 1000);
    aliceDesktop.current = true;
    const std::vector<UserSession> sessions{aliceDesktop, session("7", "alice", 1000, "online"), session("5", "Bob", 1001), greeter};
    const std::vector<Domain::ProcessSnapshot> processes{
        process(10, "alice", 5.0, 100),
        process(11, "alice", 20.0, 300),
        process(12, "Bob", 1.0, 50),
        process(13, "root", 50.0, 9000),
    };

    const std::vector<UserRow> rows = buildUserRows(sessions, processes);
    ASSERT_EQ(rows.size(), 2U) << "the greeter and root (no sign-in) have no row";
    EXPECT_EQ(rows[0].user, "alice");
    EXPECT_EQ(rows[0].uid, 1000U);
    EXPECT_EQ(rows[0].sessions.size(), 2U);
    EXPECT_TRUE(rows[0].current);
    EXPECT_EQ(rows[0].totals.processes, 2U);
    EXPECT_DOUBLE_EQ(rows[0].totals.cpuPercent, 25.0);
    EXPECT_EQ(rows[0].totals.resident, 400U);
    EXPECT_DOUBLE_EQ(rows[0].totals.ioRead + rows[0].totals.ioWrite, 30.0);
    EXPECT_DOUBLE_EQ(rows[0].totals.netSent + rows[0].totals.netReceived, 10.0);
    EXPECT_EQ(rows[0].processes, (std::vector<std::size_t>{1, 0})) << "busiest first";
    EXPECT_EQ(rows[1].user, "Bob") << "by name, ignoring case";
    EXPECT_FALSE(rows[1].current);
    EXPECT_EQ(rows[1].processes, (std::vector<std::size_t>{2}));

    EXPECT_TRUE(isSignInSession(session("1", "x", 1)));
    UserSession unclassed = session("1", "x", 1);
    unclassed.sessionClass.clear();
    EXPECT_TRUE(isSignInSession(unclassed));
    unclassed.sessionClass = "manager";
    EXPECT_FALSE(isSignInSession(unclassed));
}

TEST(UserRowsTest, TheSessionTextSaysWhereHowAndWhatItIsDoing)
{
    UserSession desktop = session("2", "alice", 1000);
    desktop.seat = "seat0";
    desktop.tty = "tty2";
    desktop.type = "wayland";
    EXPECT_EQ(sessionPlace(desktop), "seat0 \xC2\xB7 tty2 \xC2\xB7 Wayland");
    UserSession ssh = session("5", "bob", 1001);
    ssh.tty = "pts/0";
    ssh.type = "tty";
    ssh.remote = true;
    ssh.remoteHost = "10.0.0.9";
    EXPECT_EQ(sessionPlace(ssh), "pts/0 \xC2\xB7 Console \xC2\xB7 from 10.0.0.9");
    ssh.remoteHost.clear();
    ssh.type = "x11";
    EXPECT_EQ(sessionPlace(ssh), "pts/0 \xC2\xB7 X11 \xC2\xB7 remote");
    UserSession bare = session("9", "carol", 1002);
    bare.type = "unspecified";
    EXPECT_EQ(sessionPlace(bare), "Session 9");
    bare.type = "mir";
    EXPECT_EQ(sessionPlace(bare), "Mir");
    bare.type = "custom";
    EXPECT_EQ(sessionPlace(bare), "custom");

    constexpr std::uint64_t MINUTE = 60'000'000;
    const std::uint64_t now = 1'000 * MINUTE;
    EXPECT_EQ(sessionStatus(session("1", "a", 1, "active"), now), "Active");
    EXPECT_EQ(sessionStatus(session("1", "a", 1, "online"), now), "Signed in");
    EXPECT_EQ(sessionStatus(session("1", "a", 1, "closing"), now), "Closing");
    UserSession idle = session("1", "a", 1);
    idle.idle = true;
    EXPECT_EQ(sessionStatus(idle, now), "Idle") << "no idle-since";
    idle.idleSinceUsec = now - (12 * MINUTE);
    EXPECT_EQ(sessionStatus(idle, now), "Idle for 12 min");
    idle.idleSinceUsec = now - (125 * MINUTE);
    EXPECT_EQ(sessionStatus(idle, now), "Idle for 2 h 5 min");
    idle.idleSinceUsec = now - 1000;
    EXPECT_EQ(sessionStatus(idle, now), "Idle") << "under a minute";
    idle.idleSinceUsec = now + MINUTE;
    EXPECT_EQ(sessionStatus(idle, now), "Idle") << "a clock that went back";
}

// #3076: Windows' sessions say where they are (the console, a Remote Desktop and its client), the WTS states
// it adds read as words, and the tooltip names the domain, the sign-in and the session.
TEST(UserRowsTest, WindowsSessionsSayWhereTheyAreAndWhatTheyAreDoing)
{
    UserSession console = session("1", "alice", 0);
    console.type = "console";
    EXPECT_EQ(sessionPlace(console), "Console");
    UserSession rdp = session("3", "bob", 0);
    rdp.seat = "RDP-Tcp#0";
    rdp.type = "rdp";
    rdp.remote = true;
    rdp.remoteHost = "LAPTOP";
    EXPECT_EQ(sessionPlace(rdp), "RDP-Tcp#0 \xC2\xB7 Remote Desktop \xC2\xB7 from LAPTOP");
    UserSession away = session("4", "carol", 0, "disconnected");
    EXPECT_EQ(sessionPlace(away), "Session 4") << "a disconnected session has no client to say";

    EXPECT_EQ(sessionStatus(away, 0), "Disconnected");
    EXPECT_EQ(sessionStatus(session("5", "dave", 0, "connecting"), 0), "Connecting");

    EXPECT_EQ(sessionTooltip(session("2", "alice", 1000)), "alice\nSession 2") << "Linux: no domain, no sign-in time";
    UserSession signedIn = session("1", "alice", 0);
    signedIn.domain = "PC";
    signedIn.logonUsec = 1'700'000'000'000'000ULL;
    const std::string tooltip = sessionTooltip(signedIn);
    EXPECT_TRUE(tooltip.starts_with("PC\\alice\nSigned in ")) << tooltip;
    EXPECT_TRUE(tooltip.ends_with("\nSession 1")) << tooltip;
}

// #3076: two accounts with one short name (PC\\alice and CORP\\alice) are two rows, each labelled with its
// domain. A process that names only the short name can't be told apart, so neither row gets it; a name only
// one row has keeps its processes.
TEST(UserRowsTest, AccountsOfTwoDomainsAreTwoRows)
{
    UserSession local = session("1", "alice", 0);
    local.domain = "PC";
    UserSession corp = session("2", "alice", 0);
    corp.domain = "CORP";
    UserSession bob = session("3", "bob", 0);
    bob.domain = "PC";
    UserSession again = session("4", "bob", 0);
    again.domain = "PC";
    const std::vector<Domain::ProcessSnapshot> processes{process(10, "alice", 5.0, 100), process(11, "bob", 1.0, 50)};
    const std::vector<UserRow> rows = buildUserRows({local, corp, bob, again}, processes);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].label, "bob") << "a short name only one row has";
    EXPECT_EQ(rows[0].key, "PC\\bob");
    EXPECT_EQ(rows[0].sessions.size(), 2U) << "both of PC\\bob's sessions";
    EXPECT_EQ(rows[0].processes, (std::vector<std::size_t>{1}));
    EXPECT_EQ(rows[1].label, "CORP\\alice");
    EXPECT_EQ(rows[2].label, "PC\\alice");
    EXPECT_TRUE(rows[1].processes.empty()) << "an alice process could be either alice's";
    EXPECT_TRUE(rows[2].processes.empty());
    EXPECT_EQ(rows[1].user, "alice");
}

/// A Windows process, which names its owner's domain too (#3310).
[[nodiscard]] Domain::ProcessSnapshot processOf(std::int32_t pid, std::string domain, std::string user, double cpu)
{
    Domain::ProcessSnapshot p = process(pid, std::move(user), cpu, 100);
    p.userDomain = std::move(domain);
    return p;
}

// #3310: a Windows process names its owner's domain, so PC\\alice's and CORP\\alice's processes each go to
// their own row, whatever the case of the names. One with no domain still goes only to a name one row has.
TEST(UserRowsTest, AProcessWithItsDomainGoesToThatAccountsRow)
{
    UserSession local = session("1", "alice", 0);
    local.domain = "PC";
    UserSession corp = session("2", "alice", 0);
    corp.domain = "CORP";
    UserSession bob = session("3", "bob", 0);
    bob.domain = "PC";
    const std::vector<Domain::ProcessSnapshot> processes{
        processOf(10, "PC", "alice", 5.0),
        processOf(11, "CORP", "alice", 7.0),
        processOf(12, "corp", "ALICE", 1.0), // Windows compares account names without case
        processOf(13, "NT AUTHORITY", "SYSTEM", 50.0),
        processOf(14, "OTHER", "alice", 9.0), // A third alice, signed in nowhere
        process(15, "alice", 3.0, 100),       // No domain: either alice's
        process(16, "bob", 2.0, 100),         // No domain, a name one row has
        processOf(17, "CORP", "bob", 4.0),    // CORP\\bob isn't PC\\bob
    };
    const std::vector<UserRow> rows = buildUserRows({local, corp, bob}, processes);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].key, "PC\\bob");
    EXPECT_EQ(rows[0].processes, (std::vector<std::size_t>{6}));
    EXPECT_EQ(rows[1].key, "CORP\\alice");
    EXPECT_EQ(rows[1].processes, (std::vector<std::size_t>{1, 2})) << "busiest first";
    EXPECT_EQ(rows[1].totals.processes, 2U);
    EXPECT_EQ(rows[2].key, "PC\\alice");
    EXPECT_EQ(rows[2].processes, (std::vector<std::size_t>{0}));
}

// #3310: a row whose sessions gave no domain takes a process with one when only that row has its name (a
// session WTS named no domain for can't be told from the process's account). Linux's rows and processes,
// with no domain on either side, match by name as they did.
TEST(UserRowsTest, ARowWithNoDomainTakesItsNamesProcesses)
{
    const std::vector<Domain::ProcessSnapshot> processes{processOf(10, "PC", "alice", 5.0), process(11, "alice", 1.0, 100)};
    const std::vector<UserRow> rows = buildUserRows({session("1", "alice", 1000)}, processes);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].processes, (std::vector<std::size_t>{0, 1}));

    UserSession corp = session("2", "alice", 0);
    corp.domain = "CORP";
    const std::vector<UserRow> two = buildUserRows({session("1", "alice", 1000), corp}, processes);
    ASSERT_EQ(two.size(), 2U);
    EXPECT_TRUE(two[0].processes.empty()) << "alice has two rows: PC\\alice's process could be the bare row's";
    EXPECT_TRUE(two[1].processes.empty());
}

// #3310: Windows names ignore case, so a process with a domain falls back to a row with none by its name in
// any case, and only when no other row has that name in any case. Without a domain (Linux) names keep their case.
TEST(UserRowsTest, TheNoDomainFallbackIgnoresCase)
{
    UserSession corp = session("2", "ALICE", 0);
    corp.domain = "CORP";
    const std::vector<UserRow> ambiguous = buildUserRows({session("1", "alice", 1000), corp}, {processOf(10, "PC", "alice", 5.0)});
    ASSERT_EQ(ambiguous.size(), 2U);
    EXPECT_TRUE(ambiguous[0].processes.empty()) << "alice and CORP\\ALICE are one name to Windows: either could be PC\\alice";
    EXPECT_TRUE(ambiguous[1].processes.empty());

    const std::vector<Domain::ProcessSnapshot> processes{processOf(10, "PC", "ALICE", 5.0), process(11, "ALICE", 1.0, 100)};
    const std::vector<UserRow> sole = buildUserRows({session("1", "alice", 1000)}, processes);
    ASSERT_EQ(sole.size(), 1U);
    EXPECT_EQ(sole[0].processes, (std::vector<std::size_t>{0})) << "PC\\ALICE is the sole alice row's; ALICE without a domain isn't";
}

// #3076: a Windows session whose user couldn't be read is a row of its own, said to be not readable, with no
// processes and its reason in the tooltip.
TEST(UserRowsTest, ASessionNotReadableIsItsOwnRow)
{
    UserSession hidden = session("5", "", 0, "disconnected");
    hidden.unreadable = "Access is denied";
    const std::vector<UserRow> rows = buildUserRows({hidden, session("1", "alice", 1000)}, {process(10, "", 1.0, 1)});
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[1].label, "Not readable");
    EXPECT_TRUE(rows[1].user.empty());
    EXPECT_TRUE(rows[1].processes.empty());
    EXPECT_EQ(sessionTooltip(hidden), "Its user couldn't be read: Access is denied\nSession 5");
}

// Group: User's groups, as the Processes table builds them, by CPU busiest first.
[[nodiscard]] std::vector<ProcessGrouping::Group> userGroups(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    std::vector<std::size_t> order(snapshots.size());
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    return ProcessGrouping::buildGroups(
        snapshots, order, ProcessGrouping::GroupBy::User, snapshots.size(), ProcessColumn::CpuPercent, false);
}

// #3377: on Windows PC\\alice and CORP\\alice are two accounts with one short name, so Group: User has a
// group for each, labelled with its domain, as the Users tab's rows are; before, both were one "alice" group.
TEST(UserGroupsTest, TwoAccountsWithOneShortNameAreTwoGroupsLabelledWithTheirDomains)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        processOf(10, "PC", "alice", 5.0),
        processOf(11, "CORP", "alice", 7.0),
        processOf(12, "PC", "bob", 1.0),
    };
    const std::vector<ProcessGrouping::Group> groups = userGroups(snapshots);
    ASSERT_EQ(groups.size(), 3U) << "PC\\alice and CORP\\alice are two accounts";
    EXPECT_EQ(groups[0].label, "CORP\\alice") << "7 %, busiest first";
    EXPECT_EQ(groups[0].members, (std::vector<std::size_t>{1}));
    EXPECT_DOUBLE_EQ(groups[0].totals.cpuPercent, 7.0);
    EXPECT_EQ(groups[1].label, "PC\\alice");
    EXPECT_EQ(groups[1].members, (std::vector<std::size_t>{0}));
    EXPECT_EQ(groups[2].label, "bob") << "a short name only one account has";
    EXPECT_NE(groups[0].key, groups[1].key);
}

// #3377: one account's processes are one group, whatever the case of its names (Windows compares account
// names without it), labelled as its lowest-numbered process spells it; and a process whose domain wasn't read joins
// the one account with its name, as the Users tab gives it to the one row.
TEST(UserGroupsTest, OneAccountsProcessesAreOneGroup)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        processOf(10, "PC", "alice", 5.0),
        processOf(11, "pc", "ALICE", 3.0),
        processOf(12, "PC", "alice", 1.0),
        process(13, "alice", 2.0, 100), // Its domain wasn't read: PC\\alice is the only alice
        processOf(14, "NT AUTHORITY", "SYSTEM", 0.5),
        process(15, "", 0.1, 100), // Its user wasn't read: Ungrouped
    };
    const std::vector<ProcessGrouping::Group> groups = userGroups(snapshots);
    ASSERT_EQ(groups.size(), 3U);
    EXPECT_EQ(groups[0].label, "alice");
    EXPECT_EQ(groups[0].key, "pc\\alice");
    EXPECT_EQ(groups[0].members, (std::vector<std::size_t>{0, 1, 2, 3})) << "in the table's order";
    EXPECT_EQ(groups[0].totals.processes, 4U);
    EXPECT_DOUBLE_EQ(groups[0].totals.cpuPercent, 11.0);
    EXPECT_EQ(groups[1].label, "SYSTEM");
    EXPECT_TRUE(groups[2].key.empty()) << "Ungrouped, last";
    EXPECT_EQ(groups[2].members, (std::vector<std::size_t>{5}));

    // With two alice accounts, a process with no domain could be either's: it is grouped by its name alone.
    const std::vector<ProcessGrouping::Group> two =
        userGroups({processOf(10, "PC", "alice", 5.0), processOf(11, "CORP", "alice", 3.0), process(12, "alice", 1.0, 100)});
    ASSERT_EQ(two.size(), 3U);
    EXPECT_EQ(two[0].label, "PC\\alice");
    EXPECT_EQ(two[1].label, "CORP\\alice");
    EXPECT_EQ(two[2].label, "alice");
    EXPECT_EQ(two[2].members, (std::vector<std::size_t>{2}));
}

// #3377: Linux's processes name no domain, so Group: User keys and labels them by user name as it did, case
// and all, and an exited row (#3060) is listed but not counted.
TEST(UserGroupsTest, ProcessesWithNoDomainGroupByUserNameAsBefore)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        process(10, "alice", 20.0, 300),
        process(11, "root", 1.0, 10),
        process(12, "alice", 2.0, 20),
        process(13, "Alice", 0.5, 5),
    };
    EXPECT_EQ(ProcessGrouping::groupKey(snapshots[0], ProcessGrouping::GroupBy::User), "alice");
    const std::vector<std::size_t> order{0, 1, 2, 3};
    const std::vector<ProcessGrouping::Group> groups =
        ProcessGrouping::buildGroups(snapshots, order, ProcessGrouping::GroupBy::User, 2, ProcessColumn::CpuPercent, false);
    ASSERT_EQ(groups.size(), 3U) << "alice and Alice are two Linux users";
    EXPECT_EQ(groups[0].key, "alice");
    EXPECT_EQ(groups[0].label, "alice");
    EXPECT_EQ(groups[0].members, (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(groups[0].totals.processes, 1U) << "row 2 is past the live count: an exited row";
    EXPECT_EQ(groups[1].label, "root");
    EXPECT_EQ(groups[2].label, "Alice");
    EXPECT_EQ(groups[2].totals.processes, 0U);
}

/// Group: User's groups of @p snapshots taken in @p order, sorted by CPU as @p ascending says.
[[nodiscard]] std::vector<ProcessGrouping::Group>
userGroupsInOrder(const std::vector<Domain::ProcessSnapshot>& snapshots, const std::vector<std::size_t>& order, bool ascending)
{
    return ProcessGrouping::buildGroups(
        snapshots, order, ProcessGrouping::GroupBy::User, snapshots.size(), ProcessColumn::CpuPercent, ascending);
}

/// The group keyed @p key in @p groups, or null.
[[nodiscard]] const ProcessGrouping::Group* groupKeyed(const std::vector<ProcessGrouping::Group>& groups, const std::string& key)
{
    for (const ProcessGrouping::Group& group : groups)
    {
        if (group.key == key)
        {
            return &group;
        }
    }
    return nullptr;
}

/// @p group's members, sorted, so groups built in two orders compare.
[[nodiscard]] std::vector<std::size_t> sortedMembers(const ProcessGrouping::Group& group)
{
    std::vector<std::size_t> members = group.members;
    std::ranges::sort(members);
    return members;
}

// #3377 review: Windows compares account names without case, so whichever spelling of an account the table
// meets first, "alice" and "ALICE" are one name. A process whose domain wasn't read joins the one alice
// account, labelled as the lowest-numbered process spells it, whether the table is sorted up or down.
TEST(UserGroupsTest, ABareNameJoinsTheOneAccountWithItInEitherOrder)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        processOf(10, "PC", "alice", 5.0),
        processOf(11, "pc", "ALICE", 3.0),
        process(12, "alice", 1.0, 100), // Its domain wasn't read
    };
    for (const bool ascending : {false, true})
    {
        for (const std::vector<std::size_t>& order : {std::vector<std::size_t>{0, 1, 2}, std::vector<std::size_t>{2, 1, 0}})
        {
            const std::vector<ProcessGrouping::Group> groups = userGroupsInOrder(snapshots, order, ascending);
            ASSERT_EQ(groups.size(), 1U) << "order " << order[0] << ", ascending " << ascending;
            EXPECT_EQ(groups[0].key, "pc\\alice");
            EXPECT_EQ(groups[0].label, "alice") << "order " << order[0] << ", ascending " << ascending;
            EXPECT_EQ(sortedMembers(groups[0]), (std::vector<std::size_t>{0, 1, 2}));
            EXPECT_DOUBLE_EQ(groups[0].totals.cpuPercent, 9.0);
        }
    }
}

// #3377 review: with PC\alice and CORP\ALICE, two accounts Windows names alike, a process whose domain wasn't
// read could be either's: it keeps a group of its own, in either order, and neither account's totals take it.
TEST(UserGroupsTest, ABareNameTwoAccountsShareStaysItsOwnGroupInEitherOrder)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        processOf(10, "PC", "alice", 5.0),
        processOf(11, "CORP", "ALICE", 3.0),
        process(12, "alice", 1.0, 100), // Its domain wasn't read
    };
    for (const bool ascending : {false, true})
    {
        for (const std::vector<std::size_t>& order : {std::vector<std::size_t>{0, 1, 2}, std::vector<std::size_t>{2, 1, 0}})
        {
            const std::vector<ProcessGrouping::Group> groups = userGroupsInOrder(snapshots, order, ascending);
            ASSERT_EQ(groups.size(), 3U) << "order " << order[0] << ", ascending " << ascending;
            const ProcessGrouping::Group* pc = groupKeyed(groups, "pc\\alice");
            const ProcessGrouping::Group* corp = groupKeyed(groups, "corp\\alice");
            const ProcessGrouping::Group* bare = groupKeyed(groups, "alice");
            ASSERT_NE(pc, nullptr);
            ASSERT_NE(corp, nullptr);
            ASSERT_NE(bare, nullptr);
            EXPECT_EQ(pc->members, (std::vector<std::size_t>{0}));
            EXPECT_DOUBLE_EQ(pc->totals.cpuPercent, 5.0);
            EXPECT_EQ(corp->members, (std::vector<std::size_t>{1}));
            EXPECT_EQ(bare->members, (std::vector<std::size_t>{2}));
            EXPECT_EQ(pc->label, "PC\\alice") << "two groups are alice to Windows";
            EXPECT_EQ(corp->label, "CORP\\ALICE");
            EXPECT_EQ(bare->label, "alice");
        }
    }
}

// #3377 review: the Processes filter hides rows, not accounts. With PC\alice's process filtered out, a bare
// alice could still be PC\alice's or CORP\alice's, so it keeps a group of its own, CORP's totals don't take
// it, and no header is drawn for the hidden PC\alice; the labels still say both domains.
TEST(UserGroupsTest, AFilteredOutAccountStillMakesABareNameAmbiguous)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        processOf(10, "PC", "alice", 5.0),
        processOf(11, "CORP", "alice", 3.0),
        process(12, "alice", 1.0, 100), // Its domain wasn't read
    };
    for (const bool ascending : {false, true})
    {
        for (const std::vector<std::size_t>& order : {std::vector<std::size_t>{1, 2}, std::vector<std::size_t>{2, 1}})
        {
            const std::vector<ProcessGrouping::Group> groups = userGroupsInOrder(snapshots, order, ascending);
            ASSERT_EQ(groups.size(), 2U) << "order " << order[0] << ", ascending " << ascending;
            EXPECT_EQ(groupKeyed(groups, "pc\\alice"), nullptr) << "PC\\alice's only process is filtered out";
            const ProcessGrouping::Group* corp = groupKeyed(groups, "corp\\alice");
            const ProcessGrouping::Group* bare = groupKeyed(groups, "alice");
            ASSERT_NE(corp, nullptr);
            ASSERT_NE(bare, nullptr);
            EXPECT_EQ(corp->members, (std::vector<std::size_t>{1}));
            EXPECT_DOUBLE_EQ(corp->totals.cpuPercent, 3.0);
            EXPECT_EQ(corp->label, "CORP\\alice");
            EXPECT_EQ(bare->members, (std::vector<std::size_t>{2}));
        }
    }
}

// #3377 review: with the one alice account's own process filtered out, a bare alice still joins that account
// (its group, under its key), and nothing else changes.
TEST(UserGroupsTest, ABareNameJoinsItsAccountWhenTheAccountsProcessesAreFilteredOut)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        processOf(10, "PC", "alice", 5.0),
        process(11, "alice", 1.0, 100), // Its domain wasn't read
        processOf(12, "PC", "bob", 2.0),
    };
    for (const bool ascending : {false, true})
    {
        for (const std::vector<std::size_t>& order : {std::vector<std::size_t>{1, 2}, std::vector<std::size_t>{2, 1}})
        {
            const std::vector<ProcessGrouping::Group> groups = userGroupsInOrder(snapshots, order, ascending);
            ASSERT_EQ(groups.size(), 2U) << "order " << order[0] << ", ascending " << ascending;
            const ProcessGrouping::Group* alice = groupKeyed(groups, "pc\\alice");
            ASSERT_NE(alice, nullptr) << "order " << order[0] << ", ascending " << ascending;
            EXPECT_EQ(alice->members, (std::vector<std::size_t>{1}));
            EXPECT_EQ(alice->label, "alice");
            const ProcessGrouping::Group* bob = groupKeyed(groups, "pc\\bob");
            ASSERT_NE(bob, nullptr);
            EXPECT_EQ(bob->members, (std::vector<std::size_t>{2}));
        }
    }
}

// #3328: the names the User column writes with their domain are the ones two accounts with processes share,
// counted ignoring ASCII case as Windows compares account names; a process with no domain counts for none.
TEST(SharedUserNamesTest, ANameIsSharedOnlyWhenTwoAccountsHaveProcesses)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        processOf(10, "PC", "alice", 1.0),
        processOf(11, "CORP", "Alice", 1.0), // Another account, its name in another case
        processOf(12, "pc", "ALICE", 1.0),   // PC\alice again
        processOf(13, "PC", "bob", 1.0),
        processOf(14, "pc", "Bob", 1.0), // PC\bob again: one account
        processOf(15, "NT AUTHORITY", "SYSTEM", 1.0),
        process(16, "carol", 1.0, 100), // No domain: Linux, or one not read
        processOf(17, "PC", "carol", 1.0),
        processOf(18, "PC", "", 1.0), // No user read
        processOf(19, "CORP", "", 1.0),
    };
    const std::vector<std::string> shared = sharedUserNames(snapshots);
    ASSERT_EQ(shared.size(), 1U);
    EXPECT_EQ(shared[0], "alice"); // As its first process spells it
    EXPECT_TRUE(isSharedUserName(shared, "alice"));
    EXPECT_TRUE(isSharedUserName(shared, "ALICE"));
    EXPECT_FALSE(isSharedUserName(shared, "bob"));
    EXPECT_FALSE(isSharedUserName(shared, "carol"));
    EXPECT_FALSE(isSharedUserName(shared, "SYSTEM"));
    EXPECT_FALSE(isSharedUserName(shared, ""));
}

TEST(SharedUserNamesTest, LinuxProcessesNameNoDomainSoNoNameIsShared)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        process(1, "root", 1.0, 100),
        process(2, "alice", 1.0, 100),
        process(3, "Alice", 1.0, 100), // Two Linux accounts, but neither has a domain to write
    };
    EXPECT_TRUE(sharedUserNames(snapshots).empty());
    EXPECT_TRUE(sharedUserNames({}).empty());
    EXPECT_FALSE(isSharedUserName({}, "alice"));
}

TEST(SharedUserNamesTest, EverySharedNameIsFoundWhateverItsPlaceInTheSortedList)
{
    std::vector<Domain::ProcessSnapshot> snapshots;
    for (const char* name : {"dave", "Bob", "erin", "alice", "carol"})
    {
        snapshots.push_back(processOf(1, "PC", name, 1.0));
        snapshots.push_back(processOf(2, "CORP", name, 1.0));
    }
    const std::vector<std::string> shared = sharedUserNames(snapshots);
    EXPECT_EQ(shared, (std::vector<std::string>{"alice", "Bob", "carol", "dave", "erin"})); // Sorted ignoring case
    for (const char* name : {"ALICE", "bob", "Carol", "dave", "Erin"})
    {
        EXPECT_TRUE(isSharedUserName(shared, name)) << name;
    }
    EXPECT_FALSE(isSharedUserName(shared, "frank"));
    EXPECT_FALSE(isSharedUserName(shared, "aaron"));
}

} // namespace
} // namespace App::UserGrouping
