// The Processes table's Group by (#3068): group keys, app names from systemd units, totals and order.

#include "App/Panels/ProcessGrouping.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
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

TEST(ProcessGroupingTest, KeysByMode)
{
    const Domain::ProcessSnapshot p = proc("firefox", "app-gnome-firefox-4521.scope", "", 1.0, 10);
    EXPECT_EQ(ProcessGrouping::groupKey(p, GroupBy::Unit), "app-gnome-firefox-4521.scope");
    EXPECT_EQ(ProcessGrouping::groupKey(p, GroupBy::App), "firefox");
    EXPECT_EQ(ProcessGrouping::groupKey(p, GroupBy::Container), "");
    EXPECT_EQ(ProcessGrouping::groupKey(proc("nginx", "", "docker:1a2b3c4d5e6f", 0, 0), GroupBy::Container), "docker:1a2b3c4d5e6f");
    EXPECT_EQ(ProcessGrouping::groupKey(p, GroupBy::None), "");
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
    for (const GroupBy mode : {GroupBy::None, GroupBy::Unit, GroupBy::Container, GroupBy::App})
    {
        EXPECT_EQ(ProcessGrouping::groupByFromConfig(ProcessGrouping::groupByConfigValue(mode)), mode);
    }
    EXPECT_EQ(ProcessGrouping::groupByFromConfig("bogus"), GroupBy::None);
}

} // namespace
} // namespace App
