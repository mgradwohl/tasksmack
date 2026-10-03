// Linux process status and labels: the cgroup freeze check behind "Suspended" (#1105), and the
// command shown for a zombie (#1155).

#include "Platform/Linux/CgroupFreezeStatus.h"
#include "Platform/Linux/LinuxProcessProbe.h"
#include "Platform/ProcessTypes.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <sys/wait.h>
#include <unistd.h>

namespace Platform
{
namespace
{

using CgroupPath::isCgroupFrozen;
using TestSupport::ScopedTempDir;

void writeControlFile(const std::filesystem::path& path, std::string_view contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << contents;
}

TEST(CgroupFreezeStatusTest, V2FrozenCgroupIsDetected)
{
    // systemctl freeze / docker pause on a cgroup-v2-only system such as Ubuntu 24.04.
    const ScopedTempDir root("cgroup_v2_frozen");
    writeControlFile(root.path / "system.slice" / "docker-abc.scope" / "cgroup.events", "populated 1\nfrozen 1\n");

    EXPECT_TRUE(isCgroupFrozen("0::/system.slice/docker-abc.scope\n", root.path));
}

TEST(CgroupFreezeStatusTest, V2ThawedCgroupIsNotFrozen)
{
    const ScopedTempDir root("cgroup_v2_thawed");
    writeControlFile(root.path / "user.slice" / "cgroup.events", "populated 1\nfrozen 0\n");

    EXPECT_FALSE(isCgroupFrozen("0::/user.slice\n", root.path));
}

TEST(CgroupFreezeStatusTest, V1FreezerStatesAreDetected)
{
    const ScopedTempDir root("cgroup_v1_freezer");
    writeControlFile(root.path / "freezer" / "frozen" / "freezer.state", "FROZEN\n");
    writeControlFile(root.path / "freezer" / "freezing" / "freezer.state", "FREEZING\n");
    writeControlFile(root.path / "freezer" / "thawed" / "freezer.state", "THAWED\n");

    EXPECT_TRUE(isCgroupFrozen("7:freezer:/frozen\n", root.path));
    EXPECT_TRUE(isCgroupFrozen("7:freezer:/freezing\n", root.path));
    EXPECT_FALSE(isCgroupFrozen("7:freezer:/thawed\n", root.path));
}

TEST(CgroupFreezeStatusTest, HybridHierarchyChecksEveryRelevantLine)
{
    // A hybrid system lists v1 controllers and the v2 line; only the v2 cgroup is frozen here.
    const ScopedTempDir root("cgroup_hybrid");
    writeControlFile(root.path / "freezer" / "thawed" / "freezer.state", "THAWED\n");
    writeControlFile(root.path / "app.slice" / "cgroup.events", "populated 1\nfrozen 1\n");

    EXPECT_TRUE(isCgroupFrozen("12:memory:/app.slice\n7:freezer:/thawed\n0::/app.slice\n", root.path));
}

TEST(CgroupFreezeStatusTest, PathsLeavingTheHierarchyAreNotRead)
{
    // The cgroup path comes out of /proc/<pid>/cgroup and must not be able to point the check at
    // a file outside the cgroup tree.
    const ScopedTempDir dir("cgroup_escape");
    const auto root = dir.path / "cgroup";
    std::filesystem::create_directories(root);
    writeControlFile(dir.path / "outside" / "cgroup.events", "frozen 1\n");

    EXPECT_FALSE(isCgroupFrozen("0::/../outside\n", root));
}

TEST(CgroupFreezeStatusTest, MissingOrMalformedInputIsNotFrozen)
{
    const ScopedTempDir root("cgroup_malformed");

    EXPECT_FALSE(isCgroupFrozen("", root.path));
    EXPECT_FALSE(isCgroupFrozen("0::/no-such-cgroup\n", root.path));
    EXPECT_FALSE(isCgroupFrozen("garbage without colons\n0:relative-path\n", root.path));
}

TEST(LinuxProcessStatusTest, ZombieIsNotLabelledAsAKernelThread)
{
    // A zombie's cmdline is empty, as a kernel thread's is; it used to get the "[name]" label.
    const auto child = fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        _exit(0);
    }

    LinuxProcessProbe probe;
    std::string command;
    char state = '\0';
    // Not reaped yet, so it stays a zombie; wait until /proc shows it as one.
    for (int attempt = 0; attempt < 200 && state != 'Z'; ++attempt)
    {
        const auto processes = probe.enumerate();
        const auto it = std::ranges::find_if(processes, [child](const ProcessCounters& p) { return p.pid == child; });
        if (it != processes.end())
        {
            state = it->state;
            command = it->command;
        }
        if (state != 'Z')
        {
            usleep(10'000);
        }
    }
    waitpid(child, nullptr, 0);

    ASSERT_EQ(state, 'Z');
    EXPECT_FALSE(command.starts_with('[')) << command;
    EXPECT_TRUE(command.ends_with("<defunct>")) << command;
}

} // namespace
} // namespace Platform
