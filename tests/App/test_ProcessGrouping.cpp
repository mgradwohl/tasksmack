// The Processes table's Group by (#3068): group keys, app names from systemd units, totals and order.

#include "App/Panels/ProcessGroupTotals.h"
#include "App/Panels/ProcessGrouping.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using ProcessGrouping::GroupBy;

[[nodiscard]] Domain::ProcessSnapshot proc(const char* name, const char* unit, const char* container, double cpu, std::uint64_t rss)
{
    Domain::ProcessSnapshot s;
    s.name = name;
    if (unit[0] != '\0')
    {
        s.serviceNames = {unit};
    }
    s.container = container;
    s.cpuPercent = cpu;
    s.memoryBytes = rss;
    s.threadCount = 2;
    return s;
}

TEST(ProcessGroupingTest, AppNamesFromDesktopFlatpakAndSnapUnits)
{
    EXPECT_EQ(ProcessGrouping::appFromUnit("app-gnome-firefox-4521.scope"), "firefox");
    EXPECT_EQ(ProcessGrouping::appFromUnit("app-flatpak-org.mozilla.firefox-7731.scope"), "org.mozilla.firefox");
    EXPECT_EQ(ProcessGrouping::appFromUnit("app-kde-org.kde.konsole-1234.scope"), "org.kde.konsole");
    EXPECT_EQ(ProcessGrouping::appFromUnit("app-gnome-gnome-terminal-server-88.scope"), "gnome-terminal-server");
    EXPECT_EQ(ProcessGrouping::appFromUnit("app-gnome-org.gnome.Nautilus@a1b2c3.service"), "org.gnome.Nautilus");
    EXPECT_EQ(ProcessGrouping::appFromUnit("app-org.kde.kalendarac@autostart.service"), "org.kde.kalendarac");
    EXPECT_EQ(ProcessGrouping::appFromUnit("snap.firefox.firefox-1c9b2e7a-5c3f-4f0e.scope"), "firefox");
    EXPECT_EQ(ProcessGrouping::appFromUnit("snap.lxd.daemon.service"), "lxd");
    EXPECT_EQ(ProcessGrouping::appFromUnit("docker.service"), "");
    EXPECT_EQ(ProcessGrouping::appFromUnit("session-3.scope"), "");
}

// #3428: the Group control offers Container and App only where the probe reads them: a Windows-like probe
// (no cgroups, no systemd units) gets None, Unit and User; a Linux-like one every mode, in the same order.
TEST(ProcessGroupingTest, OfferedModesFollowTheProbesCapabilities)
{
    Platform::ProcessCapabilities windowsLike;
    windowsLike.hasServices = true; // the service list, attached
    EXPECT_EQ(ProcessGrouping::offeredGroupModes(windowsLike), (std::vector{GroupBy::None, GroupBy::Unit, GroupBy::User}));

    Platform::ProcessCapabilities linuxLike;
    linuxLike.hasServices = true;
    linuxLike.hasContainer = true;
    linuxLike.hasAppUnits = true;
    EXPECT_EQ(ProcessGrouping::offeredGroupModes(linuxLike),
              (std::vector{GroupBy::None, GroupBy::Unit, GroupBy::Container, GroupBy::App, GroupBy::User}));

    Platform::ProcessCapabilities containersOnly;
    containersOnly.hasContainer = true;
    EXPECT_EQ(ProcessGrouping::offeredGroupModes(containersOnly),
              (std::vector{GroupBy::None, GroupBy::Unit, GroupBy::Container, GroupBy::User}));
}

// #3428: a saved mode this system doesn't offer groups by None; one it offers stands.
TEST(ProcessGroupingTest, ASavedModeNotOfferedFallsBackToNone)
{
    const Platform::ProcessCapabilities windowsLike;
    EXPECT_EQ(ProcessGrouping::effectiveGroupBy(GroupBy::Container, windowsLike), GroupBy::None);
    EXPECT_EQ(ProcessGrouping::effectiveGroupBy(GroupBy::App, windowsLike), GroupBy::None);
    EXPECT_EQ(ProcessGrouping::effectiveGroupBy(GroupBy::Unit, windowsLike), GroupBy::Unit);
    EXPECT_EQ(ProcessGrouping::effectiveGroupBy(GroupBy::User, windowsLike), GroupBy::User);
    EXPECT_EQ(ProcessGrouping::effectiveGroupBy(GroupBy::None, windowsLike), GroupBy::None);

    Platform::ProcessCapabilities linuxLike;
    linuxLike.hasContainer = true;
    linuxLike.hasAppUnits = true;
    EXPECT_EQ(ProcessGrouping::effectiveGroupBy(GroupBy::Container, linuxLike), GroupBy::Container);
    EXPECT_EQ(ProcessGrouping::effectiveGroupBy(GroupBy::App, linuxLike), GroupBy::App);
}

TEST(ProcessGroupingTest, KeysByMode)
{
    const Domain::ProcessSnapshot p = proc("firefox", "app-gnome-firefox-4521.scope", "", 1.0, 10);
    EXPECT_EQ(ProcessGrouping::groupKey(p, GroupBy::Unit), "app-gnome-firefox-4521.scope");
    EXPECT_EQ(ProcessGrouping::groupKey(p, GroupBy::App), "firefox");
    EXPECT_EQ(ProcessGrouping::groupKey(p, GroupBy::Container), "");
    EXPECT_EQ(ProcessGrouping::groupKey(proc("nginx", "", "docker:1a2b3c4d5e6f", 0, 0), GroupBy::Container), "docker:1a2b3c4d5e6f");
    EXPECT_EQ(ProcessGrouping::groupKey(p, GroupBy::None), "");
}

// #3076: by user, each user's processes are a group with their totals; a process whose user couldn't be
// read is Ungrouped, last.
TEST(ProcessGroupingTest, ByUserTotalsEachUsersProcesses)
{
    std::vector<Domain::ProcessSnapshot> snapshots{
        proc("firefox", "", "", 20.0, 300), proc("sshd", "", "", 1.0, 10), proc("bash", "", "", 2.0, 20), proc("ghost", "", "", 0.5, 5)};
    snapshots[0].user = "alice";
    snapshots[1].user = "root";
    snapshots[2].user = "alice";
    EXPECT_EQ(ProcessGrouping::groupKey(snapshots[0], GroupBy::User), "alice");
    EXPECT_EQ(ProcessGrouping::groupByLabel(GroupBy::User), "User");
    const std::vector<ProcessGrouping::Group> groups =
        ProcessGrouping::buildGroups(snapshots, {0, 1, 2, 3}, GroupBy::User, snapshots.size(), ProcessColumn::CpuPercent, false);
    ASSERT_EQ(groups.size(), 3U);
    EXPECT_EQ(groups[0].key, "alice");
    EXPECT_EQ(groups[0].totals.processes, 2U);
    EXPECT_DOUBLE_EQ(groups[0].totals.cpuPercent, 22.0);
    EXPECT_EQ(groups[0].totals.resident, 320U);
    EXPECT_EQ(groups[1].key, "root");
    EXPECT_EQ(groups[2].key, ""); // Ungrouped, last
}

TEST(ProcessGroupingTest, GroupsTotalTheirRunningMembersAndSortByTotalsWithUngroupedLast)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{
        proc("dockerd", "docker.service", "", 3.0, 200),    // 0
        proc("kthreadd", "", "", 0.0, 0),                   // 1: Ungrouped
        proc("firefox", "firefox.scope", "", 8.0, 1000),    // 2
        proc("containerd", "docker.service", "", 1.0, 100), // 3
        proc("Web Content", "firefox.scope", "", 4.0, 500), // 4
        proc("gone", "docker.service", "", 50.0, 9999),     // 5: an exited row, not counted
    };
    const std::vector<std::size_t> order{0, 1, 2, 3, 4, 5};

    const std::vector<ProcessGrouping::Group> byCpu =
        ProcessGrouping::buildGroups(snapshots, order, GroupBy::Unit, 5, ProcessColumn::CpuPercent, false);
    ASSERT_EQ(byCpu.size(), 3U);
    EXPECT_EQ(byCpu[0].key, "firefox.scope"); // 12 %, highest first
    EXPECT_DOUBLE_EQ(byCpu[0].totals.cpuPercent, 12.0);
    EXPECT_EQ(byCpu[0].totals.resident, 1500U);
    EXPECT_EQ(byCpu[0].members, (std::vector<std::size_t>{2, 4}));
    EXPECT_EQ(byCpu[1].key, "docker.service");
    EXPECT_DOUBLE_EQ(byCpu[1].totals.cpuPercent, 4.0); // the exited row's 50 % isn't counted
    EXPECT_EQ(byCpu[1].totals.processes, 2U);
    EXPECT_EQ(byCpu[1].members, (std::vector<std::size_t>{0, 3, 5})); // but it is listed
    EXPECT_TRUE(byCpu[2].key.empty());                                // Ungrouped last
    EXPECT_EQ(byCpu[2].totals.threads, 2);

    // Ascending: the lower total first, Ungrouped still last. A column without totals sorts by name.
    const auto ascending = ProcessGrouping::buildGroups(snapshots, order, GroupBy::Unit, 5, ProcessColumn::CpuPercent, true);
    EXPECT_EQ(ascending[0].key, "docker.service");
    EXPECT_TRUE(ascending[2].key.empty());
    const auto byName = ProcessGrouping::buildGroups(snapshots, order, GroupBy::Unit, 5, ProcessColumn::Name, true);
    EXPECT_EQ(byName[0].key, "docker.service");
    EXPECT_FALSE(ProcessGrouping::totalFor(byName[0].totals, ProcessColumn::Name).has_value());
    EXPECT_FALSE(ProcessGrouping::totalFor(byName[0].totals, ProcessColumn::Virtual).has_value());
}

TEST(ProcessGroupingTest, EqualTotalsSortByNameAToZEvenWhenDescending)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{proc("b", "bravo.service", "", 0.0, 0), proc("a", "alpha.service", "", 0.0, 0)};
    const auto groups = ProcessGrouping::buildGroups(snapshots, {0, 1}, GroupBy::Unit, 2, ProcessColumn::CpuPercent, false);
    ASSERT_EQ(groups.size(), 2U);
    EXPECT_EQ(groups[0].key, "alpha.service");
}

TEST(ProcessGroupingTest, ConfigValuesRoundTrip)
{
    for (const GroupBy mode : {GroupBy::None, GroupBy::Unit, GroupBy::Container, GroupBy::App, GroupBy::User})
    {
        EXPECT_EQ(ProcessGrouping::groupByFromConfig(ProcessGrouping::groupByConfigValue(mode)), mode);
    }
    EXPECT_EQ(ProcessGrouping::groupByConfigValue(GroupBy::User), "user");
    EXPECT_EQ(ProcessGrouping::groupByFromConfig("bogus"), GroupBy::None);
}

// #3186: every summable column reads its own total, and the rest have none.
TEST(ProcessGroupingTest, EverySummableColumnHasItsTotal)
{
    ProcessGrouping::Totals t;
    t.cpuPercent = 1.0;
    t.memoryPercent = 2.0;
    t.resident = 3;
    t.privateBytes = 4;
    t.swap = 5;
    t.threads = 6;
    t.handles = 7;
    t.cpuTimeSeconds = 8.0;
    t.ioRead = 9.0;
    t.ioWrite = 10.0;
    t.netSent = 11.0;
    t.netReceived = 12.0;
    t.powerWatts = 13.0;
    t.gpuPercent = 14.0;
    t.gpuMemory = 15;
    t.npuPercent = 16.0;
    t.npuMemory = 17;
    const std::vector<std::pair<ProcessColumn, double>> expected{
        {ProcessColumn::CpuPercent, 1.0},
        {ProcessColumn::MemPercent, 2.0},
        {ProcessColumn::Resident, 3.0},
        {ProcessColumn::Private, 4.0},
        {ProcessColumn::Swap, 5.0},
        {ProcessColumn::Threads, 6.0},
        {ProcessColumn::Handles, 7.0},
        {ProcessColumn::CpuTime, 8.0},
        {ProcessColumn::IoRead, 9.0},
        {ProcessColumn::IoWrite, 10.0},
        {ProcessColumn::NetSent, 11.0},
        {ProcessColumn::NetReceived, 12.0},
        {ProcessColumn::Power, 13.0},
        {ProcessColumn::GpuPercent, 14.0},
        {ProcessColumn::GpuMemory, 15.0},
        {ProcessColumn::NpuPercent, 16.0},
        {ProcessColumn::NpuMemory, 17.0},
    };
    for (const auto& [column, value] : expected)
    {
        const std::optional<double> total = ProcessGrouping::totalFor(t, column);
        ASSERT_TRUE(total.has_value()) << static_cast<int>(column);
        EXPECT_DOUBLE_EQ(total.value_or(0.0), value) << static_cast<int>(column);
    }
    EXPECT_FALSE(ProcessGrouping::totalFor(t, ProcessColumn::Name).has_value());
    EXPECT_FALSE(ProcessGrouping::totalFor(t, ProcessColumn::Virtual).has_value());
}

// #3186: the Group control's labels, a unit that isn't an app's, and an index past the snapshots.
TEST(ProcessGroupingTest, LabelsNonAppUnitsAndStaleIndices)
{
    EXPECT_EQ(ProcessGrouping::groupByLabel(GroupBy::None), "None");
    EXPECT_EQ(ProcessGrouping::groupByLabel(GroupBy::Unit), "Unit");
    EXPECT_EQ(ProcessGrouping::groupByLabel(GroupBy::Container), "Container");
    EXPECT_EQ(ProcessGrouping::groupByLabel(GroupBy::App), "App");
    EXPECT_EQ(ProcessGrouping::appFromUnit("app-gnome-firefox-4521.slice"), "");
    EXPECT_EQ(ProcessGrouping::appFromUnit("sshd.service"), "");

    const std::vector<Domain::ProcessSnapshot> snapshots{proc("nginx", "", "docker:1", 1.0, 10)};
    const std::vector<ProcessGrouping::Group> groups =
        ProcessGrouping::buildGroups(snapshots, {0, 7}, GroupBy::Container, snapshots.size(), ProcessColumn::CpuPercent, false);
    ASSERT_EQ(groups.size(), 1U);
    EXPECT_EQ(groups[0].members, (std::vector<std::size_t>{0}));
}

// #3386: grouping by Unit needs the service list in the background only where units come from it (Windows).
TEST(ProcessGroupingTest, ServicesDemandFollowsColumnSectionAndUnitGrouping)
{
    for (const GroupBy mode : {GroupBy::None, GroupBy::Unit, GroupBy::Container, GroupBy::App, GroupBy::User})
    {
        EXPECT_TRUE(ProcessGrouping::servicesBackgroundDemand(true, false, mode, true));
        EXPECT_TRUE(ProcessGrouping::servicesBackgroundDemand(false, true, mode, false));
        EXPECT_EQ(ProcessGrouping::servicesBackgroundDemand(false, false, mode, true), mode == GroupBy::Unit);
        // Linux's units come from each process's cgroup: no list read for grouping.
        EXPECT_FALSE(ProcessGrouping::servicesBackgroundDemand(false, false, mode, false));
    }
#ifdef _WIN32
    EXPECT_TRUE(ProcessGrouping::UNITS_FROM_SERVICE_LIST);
#else
    EXPECT_FALSE(ProcessGrouping::UNITS_FROM_SERVICE_LIST);
#endif
}

// #3377: the signed-in sessions are read while the Users tab is shown, or the Processes tab grouped by User
// once its groups show sessions; not for any other grouping, nor with the Processes tab hidden.
TEST(ProcessGroupingTest, SessionsDemandFollowsTheUsersTabAndUserGrouping)
{
    for (const GroupBy mode : {GroupBy::None, GroupBy::Unit, GroupBy::Container, GroupBy::App, GroupBy::User})
    {
        for (const bool processesShown : {false, true})
        {
            for (const bool showSessions : {false, true})
            {
                EXPECT_TRUE(ProcessGrouping::sessionsDemand(true, processesShown, mode, showSessions));
                EXPECT_EQ(ProcessGrouping::sessionsDemand(false, processesShown, mode, showSessions),
                          showSessions && processesShown && mode == GroupBy::User);
            }
        }
    }
    // Until the fold adds the session rows, only the Users tab reads them: no change anyone can see.
    EXPECT_FALSE(ProcessGrouping::USER_GROUPS_SHOW_SESSIONS);
    static_assert(ProcessGrouping::sessionsDemand(true, false, GroupBy::None, false));
    static_assert(!ProcessGrouping::sessionsDemand(false, true, GroupBy::User, ProcessGrouping::USER_GROUPS_SHOW_SESSIONS));
}

} // namespace
} // namespace App
