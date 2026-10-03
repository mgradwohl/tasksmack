// Linux process status and labels: the cgroup freeze check behind "Suspended" (#1105), and the
// command shown for a zombie (#1155).

#include "Platform/Linux/CgroupFreezeStatus.h"
#include "Platform/Linux/LinuxProcessProbe.h"
#include "Platform/Linux/PriorityErrorMessage.h"
#include "Platform/Linux/UserNameLookup.h"
#include "Platform/ProcessTypes.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <pwd.h>
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

TEST(LinuxProcessStatusTest, ZombieWithUnreadableCmdlineIsStillDefunct)
{
    // /proc/<pid>/cmdline can be denied for another user's process while stat still shows state Z;
    // the zombie label must not depend on reading it (#1228 review).
    const ScopedTempDir proc("ts_test_proc_zombie_nocmdline");
    writeControlFile(proc.path / "4242" / "stat",
                     "4242 (defunct-app) Z 1 4242 4242 0 -1 4194564 0 0 0 0 0 0 0 0 20 0 1 0 12345 0 0 "
                     "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    // No cmdline file at all: opening it fails, as a permission denial would.

    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    const auto it = std::ranges::find_if(processes, [](const ProcessCounters& p) { return p.pid == 4242; });
    ASSERT_NE(it, processes.end());
    EXPECT_EQ(it->state, 'Z');
    EXPECT_EQ(it->command, "defunct-app <defunct>");
}

TEST(PriorityErrorMessageTest, PermissionErrorsGiveTheRightAdvice)
{
    // EPERM: another user's process. EACCES: raising priority without CAP_SYS_NICE. The advice
    // used to be swapped (#1155).
    const std::string otherUser = priorityErrorMessage(EPERM, 5, 1234);
    EXPECT_NE(otherUser.find("belongs to another user"), std::string::npos) << otherUser;
    EXPECT_NE(otherUser.find("Run TaskSmack as root"), std::string::npos) << otherUser;
    EXPECT_EQ(otherUser.find("renice"), std::string::npos) << otherUser; // renice -p changes one thread

    const std::string needsPrivilege = priorityErrorMessage(EACCES, -5, 1234);
    EXPECT_NE(needsPrivilege.find("CAP_SYS_NICE"), std::string::npos) << needsPrivilege;
    EXPECT_EQ(needsPrivilege.find("another user"), std::string::npos) << needsPrivilege;
    EXPECT_EQ(needsPrivilege.find("renice"), std::string::npos) << needsPrivilege;

    EXPECT_NE(priorityErrorMessage(ESRCH, 0, 1).find("not found"), std::string::npos);
    EXPECT_FALSE(priorityErrorMessage(EINVAL, 0, 1).empty());
}

TEST(UserNameLookupTest, LargeEntryIsRetriedWithABiggerBuffer)
{
    // A big LDAP/SSSD entry doesn't fit the first buffer: ERANGE, then success once it has grown.
    // The name, not the numeric UID, must come back (#1155).
    constexpr std::size_t NEEDED = 4096;
    std::size_t calls = 0;
    const auto fakeGetpwuidR = [&calls](uid_t uid, passwd* entry, char* buffer, std::size_t size, passwd** result) -> int
    {
        ++calls;
        *result = nullptr;
        if (size < NEEDED)
        {
            return ERANGE;
        }
        std::strcpy(buffer, "ldap-user");
        entry->pw_name = buffer;
        entry->pw_uid = uid;
        *result = entry;
        return 0;
    };

    EXPECT_EQ(lookUpUserName(1000, fakeGetpwuidR), "ldap-user");
    EXPECT_EQ(calls, 3U); // 1 KiB, 2 KiB, then 4 KiB
}

TEST(UserNameLookupTest, UnknownUidIsNullopt)
{
    const auto noSuchUser = [](uid_t, passwd*, char*, std::size_t, passwd** result) -> int
    {
        *result = nullptr;
        return 0;
    };
    EXPECT_FALSE(lookUpUserName(4242, noSuchUser).has_value());
}

} // namespace
} // namespace Platform
