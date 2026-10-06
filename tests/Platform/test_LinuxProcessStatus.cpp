// Linux process status and labels: the cgroup freeze check behind "Suspended" (#1105), and the
// command shown for a zombie (#1155).

#include "Platform/Linux/CgroupFreezeStatus.h"
#include "Platform/Linux/LinuxProcessProbe.h"
#include "Platform/Linux/PriorityErrorMessage.h"
#include "Platform/Linux/ThreadPriority.h"
#include "Platform/Linux/UserNameLookup.h"
#include "Platform/ProcessTypes.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

TEST(CgroupFreezeStatusTest, FreezerMustBeAWholeControllerName)
{
    // A named hierarchy "name=myfreezer" isn't the freezer controller; its path must not be looked
    // up in the freezer hierarchy, where an unrelated frozen cgroup could sit (#1228 review).
    const ScopedTempDir root("cgroup_named_hierarchy");
    writeControlFile(root.path / "freezer" / "app" / "freezer.state", "FROZEN\n");

    EXPECT_FALSE(isCgroupFrozen("5:name=myfreezer:/app\n", root.path));
    EXPECT_TRUE(isCgroupFrozen("5:cpuset,freezer:/app\n", root.path)); // co-mounted controllers
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

TEST(LinuxProcessStatusTest, FrozenCgroupIsReadFromTheInjectedCgroupRoot)
{
    // #1183: the cgroup root is injected like the proc root, so "Suspended" is testable with fixtures.
    const ScopedTempDir proc("ts_test_proc_frozen_cgroup");
    const ScopedTempDir cgroup("ts_test_cgroup_root_frozen");
    for (const char* pid : {"4242", "4343"})
    {
        writeControlFile(proc.path / pid / "stat",
                         std::string(pid) + " (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
                                            "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    }
    writeControlFile(proc.path / "4242" / "cgroup", "0::/frozen.scope\n");
    writeControlFile(proc.path / "4343" / "cgroup", "0::/thawed.scope\n");
    writeControlFile(cgroup.path / "frozen.scope" / "cgroup.events", "populated 1\nfrozen 1\n");
    writeControlFile(cgroup.path / "thawed.scope" / "cgroup.events", "populated 1\nfrozen 0\n");

    LinuxProcessProbe probe(proc.path, proc.path / "no-powercap", cgroup.path);
    const auto processes = probe.enumerate();
    const auto statusOf = [&processes](std::int32_t pid)
    {
        const auto it = std::ranges::find_if(processes, [pid](const ProcessCounters& p) { return p.pid == pid; });
        return it == processes.end() ? std::string("<missing>") : it->status;
    };
    EXPECT_EQ(statusOf(4242), "Suspended");
    EXPECT_EQ(statusOf(4343), "");
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

// reniceThreads (#1104/#1228 review): the pass bookkeeping, with /proc and setpriority faked.

using ThreadList = std::expected<std::vector<id_t>, std::error_code>;

TEST(ReniceThreadsTest, PartialFailureIsCountedNotReportedAsSuccess)
{
    constexpr std::int32_t PID = 100;
    const std::map<id_t, int> results{{100, 0}, {101, EPERM}, {102, 0}};
    const auto change = reniceThreads(
        PID,
        [](std::int32_t) { return ThreadList{std::vector<id_t>{100, 101, 102}}; },
        [&results](id_t tid) { return results.at(tid); },
        [](std::int32_t, id_t) { return true; });

    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(change->changed, 2U);
    EXPECT_EQ(change->failed, 1U);
    EXPECT_EQ(change->firstError, EPERM);
}

TEST(ReniceThreadsTest, ExitedWorkerIsSkippedButAnExitedLeaderFails)
{
    const auto workerGone = reniceThreads(
        100,
        [](std::int32_t) { return ThreadList{std::vector<id_t>{100, 101}}; },
        [](id_t tid) { return tid == 101 ? ESRCH : 0; },
        [](std::int32_t, id_t) { return true; });
    ASSERT_TRUE(workerGone.has_value());
    EXPECT_EQ(workerGone->changed, 1U);
    EXPECT_EQ(workerGone->failed, 0U);

    const auto leaderGone = reniceThreads(
        100,
        [](std::int32_t) { return ThreadList{std::vector<id_t>{100}}; },
        [](id_t) { return ESRCH; },
        [](std::int32_t, id_t) { return true; });
    ASSERT_TRUE(leaderGone.has_value());
    EXPECT_EQ(leaderGone->failed, 1U);
    EXPECT_EQ(leaderGone->firstError, ESRCH);
}

TEST(ReniceThreadsTest, WorkerThatLeftTheProcessIsUnconfirmed)
{
    // The call succeeded but the TID is no longer the target's: it may have been reused elsewhere.
    const auto change = reniceThreads(
        100,
        [](std::int32_t) { return ThreadList{std::vector<id_t>{100, 101}}; },
        [](id_t) { return 0; },
        [](std::int32_t, id_t tid) { return tid != 101; });
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(change->changed, 1U);
    EXPECT_EQ(change->unconfirmed, 1U);
}

TEST(ReniceThreadsTest, ThreadsStartedDuringThePassAreReniced)
{
    // The second listing shows a thread created during the first pass; the third shows nothing new.
    int listing = 0;
    std::vector<id_t> reniced;
    const auto change = reniceThreads(
        100,
        [&listing](std::int32_t)
        { return ++listing == 1 ? ThreadList{std::vector<id_t>{100, 101}} : ThreadList{std::vector<id_t>{100, 101, 102}}; },
        [&reniced](id_t tid)
        {
            reniced.push_back(tid);
            return 0;
        },
        [](std::int32_t, id_t) { return true; });
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(change->changed, 3U);
    EXPECT_EQ(reniced, (std::vector<id_t>{100, 101, 102}));
    EXPECT_FALSE(change->threadsKeptStarting);
    EXPECT_EQ(listing, 3);
}

TEST(ReniceThreadsTest, ExitedWorkersIdReusedByANewThreadIsReniced)
{
    // Worker 101 exits (ESRCH) and a new thread of the same process reuses 101 before the next
    // listing: it must be reniced, not taken as already done (#1228 review).
    int calls101 = 0;
    const auto change = reniceThreads(
        100,
        [](std::int32_t) { return ThreadList{std::vector<id_t>{100, 101}}; },
        [&calls101](id_t tid) { return (tid == 101 && ++calls101 == 1) ? ESRCH : 0; },
        [](std::int32_t, id_t) { return true; });
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(calls101, 2);
    EXPECT_EQ(change->changed, 2U);
    EXPECT_FALSE(change->threadsKeptStarting);
}

TEST(ReniceThreadsTest, ThreadsThatNeverStopStartingAreReported)
{
    id_t next = 100;
    const auto change = reniceThreads(
        100,
        [&next](std::int32_t) { return ThreadList{std::vector<id_t>{next++}}; },
        [](id_t) { return 0; },
        [](std::int32_t, id_t) { return true; });
    ASSERT_TRUE(change.has_value());
    EXPECT_TRUE(change->threadsKeptStarting);
}

TEST(ReniceThreadsTest, ListingFailures)
{
    // The first listing failing is an error; a later one is always kept, ENOENT included: the task
    // directory also goes away when only the leader has exited (#1228 review).
    const auto denied = std::make_error_code(std::errc::permission_denied);
    const auto first = reniceThreads(
        100,
        [&](std::int32_t) { return ThreadList{std::unexpected(denied)}; },
        [](id_t) { return 0; },
        [](std::int32_t, id_t) { return true; });
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(first.error(), denied);

    for (const auto& [later, kept] : {std::pair{denied, true}, std::pair{std::make_error_code(std::errc::no_such_file_or_directory), true}})
    {
        int listing = 0;
        const auto change = reniceThreads(
            100,
            [&, later = later](std::int32_t)
            { return ++listing == 1 ? ThreadList{std::vector<id_t>{100}} : ThreadList{std::unexpected(later)}; },
            [](id_t) { return 0; },
            [](std::int32_t, id_t) { return true; });
        ASSERT_TRUE(change.has_value());
        EXPECT_EQ(static_cast<bool>(change->relistError), kept) << later.message();
    }
}

} // namespace
} // namespace Platform
