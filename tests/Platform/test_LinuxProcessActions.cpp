/// @file test_LinuxProcessActions.cpp
/// @brief Integration tests for Platform::LinuxProcessActions and its process-control half, LinuxProcessControl
///
/// These tests verify the capabilities reporting and error handling
/// of process actions. We avoid actually terminating processes to keep
/// tests safe and non-destructive.

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/Linux/FileManagerReveal.h"
#include "Platform/Linux/IoPriority.h"
#include "Platform/Linux/LinuxCoreDump.h"
#include "Platform/Linux/LinuxProcessActions.h"
#include "Platform/Linux/LinuxProcessControl.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/Linux/ProcPrivileges.h"
#include "Platform/Linux/SyscallTrace.h"
#include "ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers) - POSIX signal.h provides kill(), csignal does not
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Platform
{
namespace
{

/// A PID no process can have: above PID_MAX_LIMIT (4194304, the most kernel.pid_max allows), so the
/// "non-existent process" tests reach the no-such-process path and can never signal a real process --
/// a PID like 99999 is often in use on a machine that has been up a while (#2132).
constexpr std::int32_t IMPOSSIBLE_PID = std::numeric_limits<std::int32_t>::max();

/// This test process as an action target, with the start time read the same way the probe reads it.
[[nodiscard]] ProcessTarget ownTarget()
{
    std::ifstream stat("/proc/self/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    return {.pid = static_cast<std::int32_t>(getpid()), .startTimeTicks = ProcParsing::parseStatStartTime(line).value_or(0)};
}

/// A forked child that sleeps until signalled, killed and reaped on scope exit if still running.
class SleepingChild
{
  public:
    /// `extraThreads` threads besides the main one, all sleeping. With extra threads, the
    /// constructor returns once every one of them is running (threadsReady()), so a test sees a
    /// fixed thread set rather than racing their start (#1307).
    explicit SleepingChild(int extraThreads = 0)
    {
        std::array<int, 2> ready{-1, -1};
        if (extraThreads > 0 && pipe(ready.data()) != 0)
        {
            return; // started() is false
        }
        m_Pid = fork();
        if (m_Pid == 0)
        {
            // The child's main thread never returns, so its locals outlive every worker.
            int readyFd = ready[1];
            for (int i = 0; i < extraThreads; ++i)
            {
                // NOLINTNEXTLINE(misc-include-cleaner) - pthread_t is provided by <pthread.h>
                pthread_t thread{};
                pthread_create(
                    &thread,
                    nullptr,
                    [](void* arg) -> void*
                    {
                        const char started = 1;
                        [[maybe_unused]] const auto written = write(*static_cast<const int*>(arg), &started, 1);
                        for (;;)
                        {
                            pause();
                        }
                    },
                    &readyFd);
            }
            for (;;)
            {
                pause();
            }
        }
        if (extraThreads > 0)
        {
            close(ready[1]);
            m_ThreadsReady = (m_Pid > 0) && readStartedThreads(ready[0], extraThreads);
            close(ready[0]);
        }
    }
    SleepingChild(const SleepingChild&) = delete;
    SleepingChild& operator=(const SleepingChild&) = delete;
    SleepingChild(SleepingChild&&) = delete;
    SleepingChild& operator=(SleepingChild&&) = delete;
    ~SleepingChild()
    {
        if (m_Pid > 0 && !m_Reaped)
        {
            ::kill(m_Pid, SIGKILL);
            waitpid(m_Pid, nullptr, 0);
        }
    }

    [[nodiscard]] bool started() const
    {
        return m_Pid > 0;
    }
    /// Whether every extra thread asked for is running (always false without extra threads).
    [[nodiscard]] bool threadsReady() const
    {
        return m_ThreadsReady;
    }
    [[nodiscard]] bool alive()
    {
        // NOLINTNEXTLINE(misc-include-cleaner) - WNOHANG is provided by <sys/wait.h>
        return !reap(WNOHANG);
    }
    [[nodiscard]] bool exitsSoon()
    {
        for (int attempt = 0; attempt < 500; ++attempt)
        {
            // NOLINTNEXTLINE(misc-include-cleaner) - WNOHANG is provided by <sys/wait.h>
            if (reap(WNOHANG))
            {
                return true;
            }
            usleep(10'000);
        }
        return false;
    }
    [[nodiscard]] ProcessTarget target() const
    {
        std::ifstream stat("/proc/" + std::to_string(m_Pid) + "/stat");
        const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
        return {.pid = static_cast<std::int32_t>(m_Pid), .startTimeTicks = ProcParsing::parseStatStartTime(line).value_or(0)};
    }

  private:
    /// Reads one byte per started worker from `fd`, waiting at most a few seconds for each.
    static bool readStartedThreads(int fd, int expected)
    {
        constexpr int TIMEOUT_MS = 10'000;
        int received = 0;
        while (received < expected)
        {
            pollfd waitFor{.fd = fd, .events = POLLIN, .revents = 0};
            if (poll(&waitFor, 1, TIMEOUT_MS) <= 0)
            {
                return false;
            }
            std::array<char, 16> bytes{};
            const auto got = read(fd, bytes.data(), bytes.size());
            if (got <= 0)
            {
                return false; // EOF: the child died before all its workers started
            }
            received += static_cast<int>(got);
        }
        return true;
    }

    bool reap(int options)
    {
        if (!m_Reaped && waitpid(m_Pid, nullptr, options) == m_Pid)
        {
            m_Reaped = true;
        }
        return m_Reaped;
    }

    pid_t m_Pid = -1;
    bool m_Reaped = false;
    bool m_ThreadsReady = false;
};

// =============================================================================
// Construction and Capabilities
// =============================================================================

TEST(LinuxProcessActionsTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ const LinuxProcessActions actions; });
}

TEST(LinuxProcessActionsTest, CapabilitiesReportedCorrectly)
{
    const LinuxProcessActions actions;
    auto caps = actions.actionCapabilities();

    // Linux should support all standard process actions
    EXPECT_TRUE(caps.canTerminate);
    EXPECT_TRUE(caps.canKill);
    EXPECT_TRUE(caps.canStop);
    EXPECT_TRUE(caps.canContinue);      // resume is called canContinue in the interface
    EXPECT_TRUE(caps.canSetPriority);   // setpriority() is available on Linux
    EXPECT_TRUE(caps.canSetIoPriority); // ioprio_set() too (#803)
}

// =============================================================================
// Error Handling Tests
// =============================================================================

// #1881, #1891: the Set affinity dialog's layout, read from this machine's sysfs when it opens: the online
// CPUs (at least the one this test runs on), and a class for each only where the CPU is hybrid.
TEST(LinuxProcessActionsTest, TheAffinityLayoutListsThisMachinesOnlineCpus)
{
    const LinuxProcessActions actions;
    const CpuAffinityLayout layout = actions.affinityLayout();
    if (!std::filesystem::exists("/sys/devices/system/cpu/online"))
    {
        EXPECT_TRUE(layout.online.empty());
        GTEST_SKIP() << "No /sys/devices/system/cpu/online here";
    }
    EXPECT_FALSE(layout.online.empty());
    const int cpu = ::sched_getcpu();
    ASSERT_GE(cpu, 0);
    EXPECT_TRUE(layout.online.test(static_cast<std::size_t>(cpu))) << "the CPU this test runs on is online";
    EXPECT_TRUE(layout.groupSizes.empty()) << "no processor groups on Linux";
    EXPECT_FALSE(layout.appliedAsCpuSets);
}

TEST(LinuxProcessActionsTest, TerminateNonExistentProcess)
{
    LinuxProcessActions actions;

    const int32_t nonExistentPid = IMPOSSIBLE_PID;
    const auto result = actions.terminate({.pid = nonExistentPid, .startTimeTicks = 1});

    // The no-such-process path, not the identity check: no process can hold this PID.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Process not found - may have already exited");
}

TEST(LinuxProcessActionsTest, KillNonExistentProcess)
{
    LinuxProcessActions actions;

    const int32_t nonExistentPid = IMPOSSIBLE_PID;
    const auto result = actions.kill({.pid = nonExistentPid, .startTimeTicks = 1});

    // The no-such-process path, not the identity check: no process can hold this PID.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Process not found - may have already exited");
}

// #3063: the other signals reach the process (SIGUSR1's default action ends a child with no handler), go
// only to the process the start time names, and are refused for init as terminate is.
TEST(LinuxProcessActionsTest, SendSignalReachesTheProcessTheStartTimeNames)
{
    LinuxProcessActions actions;
    EXPECT_TRUE(actions.actionCapabilities().canSendSignals);

    SleepingChild child;
    ASSERT_TRUE(child.started());
    ProcessTarget reused = child.target();
    ++reused.startTimeTicks; // the PID, but another process's start time
    EXPECT_FALSE(actions.sendSignal(reused, ProcessSignal::User1).success);
    EXPECT_TRUE(child.alive()) << "a reused PID never gets the signal";

    const auto sent = actions.sendSignal(child.target(), ProcessSignal::User1);
    EXPECT_TRUE(sent.success) << sent.errorMessage;
    EXPECT_TRUE(child.exitsSoon()) << "SIGUSR1 with no handler ends the process";

    const auto init = actions.sendSignal({.pid = 1, .startTimeTicks = 0}, ProcessSignal::Hangup);
    EXPECT_FALSE(init.success) << "init is refused, as for terminate";
}

TEST(LinuxProcessActionsTest, StopNonExistentProcess)
{
    LinuxProcessActions actions;

    const int32_t nonExistentPid = IMPOSSIBLE_PID;
    const auto result = actions.stop({.pid = nonExistentPid, .startTimeTicks = 1});

    // The no-such-process path, not the identity check: no process can hold this PID.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Process not found - may have already exited");
}

TEST(LinuxProcessActionsTest, ResumeNonExistentProcess)
{
    LinuxProcessActions actions;

    const int32_t nonExistentPid = IMPOSSIBLE_PID;
    const auto result = actions.resume({.pid = nonExistentPid, .startTimeTicks = 1});

    // The no-such-process path, not the identity check: no process can hold this PID.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Process not found - may have already exited");
}

TEST(LinuxProcessActionsTest, TerminateInvalidPid)
{
    LinuxProcessActions actions;

    // pid=0 is invalid (sendSignal() guards pid <= 0)
    const auto resultZero = actions.terminate({.pid = 0, .startTimeTicks = 1});
    EXPECT_FALSE(resultZero.success);
    EXPECT_GT(resultZero.errorMessage.size(), 0ULL);

    // Negative pid is also invalid
    const auto resultNeg = actions.terminate({.pid = -1, .startTimeTicks = 1});
    EXPECT_FALSE(resultNeg.success);
    EXPECT_GT(resultNeg.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, KillInvalidPid)
{
    LinuxProcessActions actions;

    const auto resultZero = actions.kill({.pid = 0, .startTimeTicks = 1});
    EXPECT_FALSE(resultZero.success);
    EXPECT_GT(resultZero.errorMessage.size(), 0ULL);

    const auto resultNeg = actions.kill({.pid = -1, .startTimeTicks = 1});
    EXPECT_FALSE(resultNeg.success);
    EXPECT_GT(resultNeg.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, StopInvalidPid)
{
    LinuxProcessActions actions;

    const auto resultZero = actions.stop({.pid = 0, .startTimeTicks = 1});
    EXPECT_FALSE(resultZero.success);
    EXPECT_GT(resultZero.errorMessage.size(), 0ULL);

    const auto resultNeg = actions.stop({.pid = -1, .startTimeTicks = 1});
    EXPECT_FALSE(resultNeg.success);
    EXPECT_GT(resultNeg.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, ResumeInvalidPid)
{
    LinuxProcessActions actions;

    const auto resultZero = actions.resume({.pid = 0, .startTimeTicks = 1});
    EXPECT_FALSE(resultZero.success);
    EXPECT_GT(resultZero.errorMessage.size(), 0ULL);

    const auto resultNeg = actions.resume({.pid = -1, .startTimeTicks = 1});
    EXPECT_FALSE(resultNeg.success);
    EXPECT_GT(resultNeg.errorMessage.size(), 0ULL);
}

// =============================================================================
// Priority Adjustment Tests
// =============================================================================

TEST(LinuxProcessActionsTest, SetPriorityNonExistentProcess)
{
    LinuxProcessActions actions;

    const int32_t nonExistentPid = IMPOSSIBLE_PID;
    const auto result = actions.setPriority({.pid = nonExistentPid, .startTimeTicks = 1}, 0);

    // The no-such-process path, not the identity check: no process can hold this PID.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Process not found - may have already exited");
}

TEST(LinuxProcessActionsTest, SetPriorityInvalidPid)
{
    LinuxProcessActions actions;

    // Test with invalid PIDs
    const auto result1 = actions.setPriority({.pid = 0, .startTimeTicks = 1}, 0);
    EXPECT_FALSE(result1.success);
    EXPECT_GT(result1.errorMessage.size(), 0ULL);

    const auto result2 = actions.setPriority({.pid = -1, .startTimeTicks = 1}, 0);
    EXPECT_FALSE(result2.success);
    EXPECT_GT(result2.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, SetPriorityOfAChildSucceeds)
{
    // Against a disposable child, not this test process: an unprivileged process can raise its own
    // niceness but never lower it again, and every child forked afterwards would inherit it, so a
    // later priority test running in the same process would start at 19 (#1228 review).
    const SleepingChild child;
    ASSERT_TRUE(child.started());

    // The child inherits the runner's niceness, which may already be above 10 (say under nice -n 15),
    // and an unprivileged process can only raise it: pick a value above the child's current one.
    errno = 0;
    const int before = getpriority(PRIO_PROCESS, static_cast<id_t>(child.target().pid));
    ASSERT_EQ(errno, 0);
    if (before >= 19)
    {
        GTEST_SKIP() << "child already at the lowest priority";
    }
    const int raised = std::min(before + 5, 19);

    LinuxProcessActions actions;
    const auto result = actions.setPriority(child.target(), raised);
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(getpriority(PRIO_PROCESS, static_cast<id_t>(child.target().pid)), raised);
}

TEST(LinuxProcessActionsTest, SetPriorityClampsBoundaryValues)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());

    // Out-of-range values are clamped to [-20, 19]. Lowering niceness needs privilege, so the
    // first may be refused; it must still fail cleanly, with a message.
    LinuxProcessActions actions;
    const auto belowRange = actions.setPriority(child.target(), -100);
    if (!belowRange.success)
    {
        EXPECT_FALSE(belowRange.errorMessage.empty());
    }
    const auto aboveRange = actions.setPriority(child.target(), 100);
    EXPECT_TRUE(aboveRange.success) << aboveRange.errorMessage;
    EXPECT_EQ(getpriority(PRIO_PROCESS, static_cast<id_t>(child.target().pid)), 19);
}

// =============================================================================
// Success Path Tests
// =============================================================================

TEST(LinuxProcessActionsTest, ResumeOwnProcess_Succeeds)
{
    // Send SIGCONT to our own process - safe because it's a no-op on a running process
    // but exercises the whole sendSignal() success path: pidfd, identity check, send.
    // (See also the child-process tests below, which check the refusal path leaves a process alone.)
    LinuxProcessActions actions;

    const auto result = actions.resume(ownTarget());

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());
}

// =============================================================================
// Identity (#973), against a real child process that only waits to be killed.
// =============================================================================

TEST(LinuxProcessActionsTest, KillWithADifferentStartTimeLeavesTheProcessRunning)
{
    SleepingChild child;
    ASSERT_TRUE(child.started());
    ProcessTarget reused = child.target();
    ASSERT_NE(reused.startTimeTicks, 0ULL);
    reused.startTimeTicks += 1; // Same PID, a different process as far as the action can tell.

    LinuxProcessActions actions;
    const auto result = actions.kill(reused);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("different process"), std::string::npos) << result.errorMessage;
    EXPECT_TRUE(child.alive());
}

TEST(LinuxProcessActionsTest, SetPriorityReachesTheProcessOnlyWhenTheStartTimeMatches)
{
    // Raising niceness needs no privilege, so this runs anywhere. The refused call must leave the
    // child's niceness alone; the matching call must change it, which also exercises the check
    // made after setpriority(2) that the target was not reaped in between.
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    const ProcessTarget target = child.target();
    ASSERT_NE(target.startTimeTicks, 0ULL);
    errno = 0;
    const int before = getpriority(PRIO_PROCESS, static_cast<id_t>(target.pid));
    ASSERT_EQ(errno, 0);
    const int raised = std::min(before + 5, 19);
    if (raised == before)
    {
        GTEST_SKIP() << "child already at the lowest priority (the test runner is at nice 19)";
    }

    LinuxProcessActions actions;
    ProcessTarget reused = target;
    reused.startTimeTicks += 1;
    const auto refused = actions.setPriority(reused, raised);
    EXPECT_FALSE(refused.success);
    EXPECT_EQ(getpriority(PRIO_PROCESS, static_cast<id_t>(target.pid)), before);

    const auto applied = actions.setPriority(target, raised);
    EXPECT_TRUE(applied.success) << applied.errorMessage;
    EXPECT_EQ(getpriority(PRIO_PROCESS, static_cast<id_t>(target.pid)), raised);
}

/// The thread IDs in /proc/<pid>/task.
std::vector<id_t> listThreads(pid_t pid)
{
    std::vector<id_t> tids;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(std::format("/proc/{}/task", pid), ec))
    {
        tids.push_back(static_cast<id_t>(std::stoul(entry.path().filename().string())));
    }
    return tids;
}

TEST(LinuxProcessActionsTest, SetPriorityChangesEveryThread)
{
    // Nice is per thread on Linux: setpriority(PRIO_PROCESS, pid) alone changes only the main
    // thread, leaving a multithreaded process's workers at the old priority (#1104).
    //
    // The child's workers are all running before its threads are listed, so the set is fixed while
    // it is measured (#1307). It can hold more than the main thread and the two workers: a
    // sanitizer runtime may start a thread of its own in the child (TSan's background thread
    // starts with the first pthread_create), and the renice must reach that one too.
    const SleepingChild child(2);
    ASSERT_TRUE(child.started());
    ASSERT_TRUE(child.threadsReady());
    const ProcessTarget target = child.target();
    const std::vector<id_t> tids = listThreads(target.pid);
    ASSERT_GE(tids.size(), 3U);

    errno = 0;
    const int before = getpriority(PRIO_PROCESS, static_cast<id_t>(target.pid));
    ASSERT_EQ(errno, 0);
    const int raised = std::min(before + 5, 19);
    if (raised == before)
    {
        GTEST_SKIP() << "child already at the lowest priority (the test runner is at nice 19)";
    }

    LinuxProcessActions actions;
    const auto result = actions.setPriority(target, raised);
    ASSERT_TRUE(result.success) << result.errorMessage;
    for (const id_t tid : tids)
    {
        EXPECT_EQ(getpriority(PRIO_PROCESS, tid), raised) << "thread " << tid;
    }
}

// =============================================================================
// CPU affinity (#1887), against a real child process: narrowing its own user's process needs no
// privilege. The CPU chosen is one this test may run on, so a cpuset or taskset around the test
// doesn't refuse it.
// =============================================================================

TEST(LinuxProcessActionsTest, SetAffinityChangesEveryThread)
{
    cpu_set_t own;
    CPU_ZERO(&own);
    ASSERT_EQ(sched_getaffinity(0, sizeof(own), &own), 0);
    int cpu = -1;
    for (int i = 0; i < CPU_SETSIZE && cpu < 0; ++i)
    {
        if (CPU_ISSET(i, &own))
        {
            cpu = i;
        }
    }
    ASSERT_GE(cpu, 0);

    const SleepingChild child(2);
    ASSERT_TRUE(child.started());
    ASSERT_TRUE(child.threadsReady());
    const ProcessTarget target = child.target();
    const std::vector<id_t> tids = listThreads(target.pid);
    ASSERT_GE(tids.size(), 3U);

    CpuAffinity only;
    only.set(static_cast<std::size_t>(cpu));
    LinuxProcessActions actions;
    EXPECT_TRUE(actions.actionCapabilities().canSetAffinity);
    const auto result = actions.setAffinity(target, only);
    ASSERT_TRUE(result.success) << result.errorMessage;
    for (const id_t tid : tids)
    {
        cpu_set_t now;
        CPU_ZERO(&now);
        ASSERT_EQ(sched_getaffinity(static_cast<pid_t>(tid), sizeof(now), &now), 0) << "thread " << tid;
        EXPECT_EQ(CPU_COUNT(&now), 1) << "thread " << tid;
        EXPECT_TRUE(CPU_ISSET(cpu, &now)) << "thread " << tid;
    }
}

TEST(LinuxProcessActionsTest, SetAffinityRefusesAnEmptySetAndAnInvalidPid)
{
    LinuxProcessActions actions;
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    EXPECT_EQ(actions.setAffinity(child.target(), CpuAffinity{}).errorMessage, "Choose at least one CPU");
    EXPECT_EQ(actions.setAffinity({.pid = 0, .startTimeTicks = 1}, CpuAffinity::fromMask(1)).errorMessage, "Invalid PID");
    // A different start time: the PID now names another process, and nothing is changed.
    const ProcessTarget other{.pid = child.target().pid, .startTimeTicks = child.target().startTimeTicks + 1};
    EXPECT_FALSE(actions.setAffinity(other, CpuAffinity::fromMask(1)).success);
}

// =============================================================================
// I/O priority (#803), against real child processes. Lowering a process of this user to the Idle
// class or to a lower best-effort level needs no privilege.
// =============================================================================

constexpr IoPriority IDLE_PRIORITY{.ioClass = IoPriorityClass::Idle, .level = 0};

/// The thread's I/O priority straight from ioprio_get(2), or nullopt if it can't be read.
std::optional<IoPriority> rawIoPriority(id_t tid)
{
    const auto value = static_cast<int>(::syscall(SYS_ioprio_get, IoPrio::WHO_PROCESS, static_cast<pid_t>(tid)));
    return IoPrio::decode(value);
}

TEST(LinuxProcessActionsTest, SetIoPriorityToIdleChangesEveryThreadOfAChild)
{
    // As nice, I/O priority is per thread: ioprio_set(IOPRIO_WHO_PROCESS, pid) alone reaches only the
    // main thread, so every thread must be set.
    const SleepingChild child(2);
    ASSERT_TRUE(child.started());
    ASSERT_TRUE(child.threadsReady());
    const ProcessTarget target = child.target();
    const std::vector<id_t> tids = listThreads(target.pid);
    ASSERT_GE(tids.size(), 3U);

    LinuxProcessActions actions;
    const auto result = actions.setIoPriority(target, IoPriorityClass::Idle, 0);
    ASSERT_TRUE(result.success) << result.errorMessage;
    for (const id_t tid : tids)
    {
        EXPECT_EQ(rawIoPriority(tid), std::optional<IoPriority>{IDLE_PRIORITY}) << "thread " << tid;
    }

    const IoPriorityReadResult current = actions.getIoPriority(target);
    ASSERT_TRUE(current.has_value()) << current.error();
    EXPECT_EQ(current->ioClass, IoPriorityClass::Idle);
}

TEST(LinuxProcessActionsTest, GetIoPriorityReadsAnotherUsersLiveProcess)
{
    // PID 1 belongs to root. Reading its I/O priority needs no privilege, so a non-root test run must
    // not take the EPERM that signal 0 gets from it for "exited" (#803 review).
    if (geteuid() == 0)
    {
        GTEST_SKIP() << "running as root: PID 1 is not another user's process";
    }
    std::ifstream stat("/proc/1/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const std::uint64_t startTicks = ProcParsing::parseStatStartTime(line).value_or(0);
    if (startTicks == 0)
    {
        GTEST_SKIP() << "PID 1's start time is not readable here (hidepid)";
    }

    LinuxProcessActions actions;
    const IoPriorityReadResult current = actions.getIoPriority({.pid = 1, .startTimeTicks = startTicks});
    ASSERT_TRUE(current.has_value()) << current.error();
}

/// A process found under /proc for which @p wanted(stat fields, its status's real UID) holds -- other
/// than this one -- as a target, or nullopt (hidepid, a container showing none).
template<typename Wanted> [[nodiscard]] std::optional<ProcessTarget> findProcess(Wanted wanted)
{
    for (const auto& entry : std::filesystem::directory_iterator("/proc"))
    {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos || std::stoi(name) == getpid())
        {
            continue;
        }
        std::ifstream statFile(entry.path() / "stat");
        const std::string stat((std::istreambuf_iterator<char>(statFile)), std::istreambuf_iterator<char>());
        std::ifstream statusFile(entry.path() / "status");
        std::string line;
        long uid = -1;
        while (std::getline(statusFile, line))
        {
            if (line.starts_with("Uid:"))
            {
                uid = std::stol(line.substr(4));
                break;
            }
        }
        const std::optional<ProcParsing::StatFields> fields = ProcParsing::parseStatFields(stat);
        if (fields.has_value() && fields->startTime != 0 && wanted(*fields, uid))
        {
            return ProcessTarget{.pid = std::stoi(name), .startTimeTicks = fields->startTime};
        }
    }
    return std::nullopt;
}

TEST(LinuxProcessActionsTest, SignallingAnotherUsersProcessSaysPermissionDenied)
{
    // A root process (not init or a kernel thread, which no signal reaches, #2447): a confirmed identity,
    // then EPERM from the signal itself, reported in words rather than as errno text (#1548). SIGCONT
    // ("resume") so that even a misdetected run could do no harm: resuming a running process changes nothing.
    if (geteuid() == 0)
    {
        GTEST_SKIP() << "running as root: no process is another user's";
    }
    const std::optional<ProcessTarget> rootProcess =
        findProcess([](const ProcParsing::StatFields& fields, long uid)
                    { return uid == 0 && (fields.flags & ProcParsing::PF_KTHREAD_FLAG) == 0 && fields.parentPid != 0; });
    if (!rootProcess.has_value())
    {
        GTEST_SKIP() << "no root process is visible here (hidepid, a container)";
    }

    LinuxProcessActions actions;
    const ProcessActionResult result = actions.resume(*rootProcess);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Permission denied - process belongs to another user");
    EXPECT_TRUE(result.permissionDenied); // what an administrator could do: BrokeredProcessActions retries it (#2308)
}

// #2447: as root the kernel accepts a signal to init or a kernel thread and drops it, so TaskSmack would
// say it worked. Refused instead, before the signal, as an error no password helps with.
TEST(LinuxProcessActionsTest, InitAndKernelThreadsAreNotSignalled)
{
    LinuxProcessActions actions;
    std::ifstream stat("/proc/1/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    if (const std::uint64_t startTicks = ProcParsing::parseStatStartTime(line).value_or(0); startTicks != 0)
    {
        const ProcessActionResult init = actions.resume({.pid = 1, .startTimeTicks = startTicks});
        EXPECT_FALSE(init.success);
        EXPECT_FALSE(init.permissionDenied);
        EXPECT_TRUE(init.errorMessage.contains("init")) << init.errorMessage;
    }

    const std::optional<ProcessTarget> kernelThread =
        findProcess([](const ProcParsing::StatFields& fields, long /*uid*/) { return (fields.flags & ProcParsing::PF_KTHREAD_FLAG) != 0; });
    if (!kernelThread.has_value())
    {
        GTEST_SKIP() << "no kernel thread is visible here (a container)";
    }
    const ProcessActionResult result = actions.resume(*kernelThread);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied) << "no password helps";
    EXPECT_TRUE(result.errorMessage.contains("kernel thread")) << result.errorMessage;
}

// #2641: priority, I/O priority and affinity are refused for init and kernel threads, as signals are, so
// the root helper can't put kswapd or a journal thread in the Idle I/O class. No password helps.
TEST(LinuxProcessActionsTest, InitAndKernelThreadsKeepTheirPriorityAndAffinity)
{
    LinuxProcessActions actions;
    const auto expectRefused = [&actions](const ProcessTarget& target, std::string_view because)
    {
        for (const ProcessActionResult& result : {actions.setPriority(target, 19),
                                                  actions.setIoPriority(target, IoPriorityClass::Idle, 0),
                                                  actions.setAffinity(target, CpuAffinity::fromMask(1))})
        {
            EXPECT_FALSE(result.success);
            EXPECT_FALSE(result.permissionDenied) << "no password helps";
            EXPECT_TRUE(result.errorMessage.contains(because)) << result.errorMessage;
        }
    };
    expectRefused({.pid = 1, .startTimeTicks = 1}, "init");

    const std::optional<ProcessTarget> kernelThread =
        findProcess([](const ProcParsing::StatFields& fields, long /*uid*/) { return (fields.flags & ProcParsing::PF_KTHREAD_FLAG) != 0; });
    if (!kernelThread.has_value())
    {
        GTEST_SKIP() << "no kernel thread is visible here (a container)";
    }
    expectRefused(*kernelThread, "kernel thread");
}

// #2641: a process never suspends itself: the root helper would stay stopped, out of the user's reach.
TEST(LinuxProcessActionsTest, ThisProcessIsNotSuspended)
{
    std::ifstream stat("/proc/self/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const ProcessTarget self{.pid = static_cast<std::int32_t>(::getpid()),
                             .startTimeTicks = ProcParsing::parseStatStartTime(line).value_or(0)};
    ASSERT_NE(self.startTimeTicks, 0U);
    LinuxProcessActions actions;
    const ProcessActionResult result = actions.stop(self);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied);
    EXPECT_TRUE(result.errorMessage.contains("this process")) << result.errorMessage;
}

TEST(LinuxProcessActionsTest, OtherFailuresAreNotPermissionRefusals)
{
    LinuxProcessActions actions;
    const ProcessActionResult result = actions.resume({.pid = IMPOSSIBLE_PID, .startTimeTicks = 1});
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied);
}

TEST(LinuxProcessActionsTest, SetIoPriorityBestEffortLevelIsReadBack)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());

    LinuxProcessActions actions;
    const auto result = actions.setIoPriority(child.target(), IoPriorityClass::BestEffort, 7);
    ASSERT_TRUE(result.success) << result.errorMessage;
    const IoPriorityReadResult current = actions.getIoPriority(child.target());
    ASSERT_TRUE(current.has_value()) << current.error();
    EXPECT_EQ(*current, (IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 7}));
}

TEST(LinuxProcessActionsTest, SetIoPriorityDefaultRestoresTheNiceDerivedClass)
{
    // "Default" is IOPRIO_CLASS_NONE with level 0, which ioprio_set(2) accepts (only a non-zero
    // level with NONE is EINVAL) and which hands the process back to nice-derived I/O priority, as
    // `ionice -c 0` does (#803 review).
    const SleepingChild child;
    ASSERT_TRUE(child.started());

    LinuxProcessActions actions;
    ASSERT_TRUE(actions.setIoPriority(child.target(), IoPriorityClass::Idle, 0).success);
    const auto result = actions.setIoPriority(child.target(), IoPriorityClass::None, 0);
    ASSERT_TRUE(result.success) << result.errorMessage;
    const IoPriorityReadResult current = actions.getIoPriority(child.target());
    ASSERT_TRUE(current.has_value()) << current.error();
    EXPECT_EQ(current->ioClass, IoPriorityClass::None);
}

TEST(LinuxProcessActionsTest, SetIoPriorityReachesTheProcessOnlyWhenTheStartTimeMatches)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    const ProcessTarget target = child.target();
    ASSERT_NE(target.startTimeTicks, 0ULL);
    const std::optional<IoPriority> before = rawIoPriority(static_cast<id_t>(target.pid));
    ASSERT_TRUE(before.has_value());
    if (before == std::optional<IoPriority>{IDLE_PRIORITY})
    {
        GTEST_SKIP() << "the test runner already runs at the Idle I/O class";
    }

    LinuxProcessActions actions;
    ProcessTarget reused = target;
    reused.startTimeTicks += 1;
    const auto refused = actions.setIoPriority(reused, IoPriorityClass::Idle, 0);
    EXPECT_FALSE(refused.success);
    EXPECT_NE(refused.errorMessage.find("different process"), std::string::npos) << refused.errorMessage;
    EXPECT_EQ(rawIoPriority(static_cast<id_t>(target.pid)), before);

    const auto unknown = actions.setIoPriority({.pid = target.pid, .startTimeTicks = 0}, IoPriorityClass::Idle, 0);
    EXPECT_FALSE(unknown.success);
    EXPECT_NE(unknown.errorMessage.find("Cannot confirm the identity"), std::string::npos) << unknown.errorMessage;
    EXPECT_EQ(rawIoPriority(static_cast<id_t>(target.pid)), before);

    const auto applied = actions.setIoPriority(target, IoPriorityClass::Idle, 0);
    EXPECT_TRUE(applied.success) << applied.errorMessage;
    EXPECT_EQ(rawIoPriority(static_cast<id_t>(target.pid)), std::optional<IoPriority>{IDLE_PRIORITY});
}

TEST(LinuxProcessActionsTest, GetIoPriorityRefusesAnotherOrUnknownIdentity)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    ProcessTarget reused = child.target();
    reused.startTimeTicks += 1;

    LinuxProcessActions actions;
    const IoPriorityReadResult mismatched = actions.getIoPriority(reused);
    ASSERT_FALSE(mismatched.has_value());
    EXPECT_NE(mismatched.error().find("different process"), std::string::npos) << mismatched.error();

    const IoPriorityReadResult unknown = actions.getIoPriority({.pid = child.target().pid, .startTimeTicks = 0});
    EXPECT_FALSE(unknown.has_value());
    EXPECT_FALSE(actions.getIoPriority({.pid = -1, .startTimeTicks = 1}).has_value());
}

TEST(LinuxProcessActionsTest, SetIoPriorityOfAMissingOrInvalidProcessFails)
{
    LinuxProcessActions actions;
    const auto missing = actions.setIoPriority({.pid = IMPOSSIBLE_PID, .startTimeTicks = 1}, IoPriorityClass::Idle, 0);
    EXPECT_FALSE(missing.success);
    EXPECT_FALSE(missing.errorMessage.empty());

    const auto invalid = actions.setIoPriority({.pid = 0, .startTimeTicks = 1}, IoPriorityClass::Idle, 0);
    EXPECT_FALSE(invalid.success);
    EXPECT_EQ(invalid.errorMessage, "Invalid PID");
}

TEST(LinuxProcessActionsTest, SetIoPriorityRealtimeWithoutPrivilegeSaysWhy)
{
    // Realtime needs CAP_SYS_NICE or CAP_SYS_ADMIN. Unprivileged, the refusal
    // names the capability rather than blaming another user; privileged, it simply succeeds.
    const SleepingChild child;
    ASSERT_TRUE(child.started());

    LinuxProcessActions actions;
    const auto result = actions.setIoPriority(child.target(), IoPriorityClass::Realtime, 4);
    if (result.success)
    {
        GTEST_SKIP() << "running with the privilege to set the Realtime I/O class";
    }
    EXPECT_NE(result.errorMessage.find("CAP_SYS_NICE"), std::string::npos) << result.errorMessage;
    EXPECT_EQ(result.errorMessage.find("another user"), std::string::npos) << result.errorMessage;
}

TEST(LinuxProcessActionsTest, KillWithTheMatchingStartTimeEndsTheProcess)
{
    SleepingChild child;
    ASSERT_TRUE(child.started());

    LinuxProcessActions actions;
    const auto result = actions.kill(child.target());

    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(child.exitsSoon());
}

// --- Trace system calls (#182) ---------------------------------------------------------------------
// strace is never run: the "terminal" is a script that records its argv, and the "tracer" path only
// has to exist and be executable.

/// Writes an executable /bin/sh script to @p path.
void writeScript(const std::filesystem::path& path, const std::string& body)
{
    {
        std::ofstream out(path);
        out << "#!/bin/sh\n" << body;
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
}

/// Tools whose terminal records its argv to @p dir/argv.txt and whose tracer is a no-op script.
[[nodiscard]] SyscallTrace::Tools fakeTraceTools(const std::filesystem::path& dir)
{
    const std::filesystem::path terminal = dir / "fake-terminal";
    writeScript(
        terminal,
        std::format("for a in \"$@\"; do printf '%s\\n' \"$a\"; done > '{0}/argv.tmp'\nmv '{0}/argv.tmp' '{0}/argv.txt'\n", dir.string()));
    const std::filesystem::path tracer = dir / "fake-strace";
    writeScript(tracer, "exit 0\n");
    return {.tracerPath = tracer.string(),
            .terminal = SyscallTrace::Terminal{.path = terminal.string(), .syntax = SyscallTrace::TerminalSyntax::DashE},
            .coreDumperPath = {},
            .folderOpenerPath = {}};
}

TEST(LinuxProcessActionsTest, SyscallTraceAvailabilityFollowsTheToolsFound)
{
    const TestSupport::ScopedTempDir dir("tasksmack_trace_caps");
    const SyscallTrace::Tools tools = fakeTraceTools(dir.path);
    EXPECT_EQ(LinuxProcessActions(tools).actionCapabilities().syscallTrace, SyscallTraceAvailability::Available);
    EXPECT_EQ(
        LinuxProcessActions(SyscallTrace::Tools{.tracerPath = {}, .terminal = tools.terminal, .coreDumperPath = {}, .folderOpenerPath = {}})
            .actionCapabilities()
            .syscallTrace,
        SyscallTraceAvailability::NoTracer);
    EXPECT_EQ(
        LinuxProcessActions(
            SyscallTrace::Tools{.tracerPath = tools.tracerPath, .terminal = std::nullopt, .coreDumperPath = {}, .folderOpenerPath = {}})
            .actionCapabilities()
            .syscallTrace,
        SyscallTraceAvailability::NoTerminal);
}

TEST(LinuxProcessActionsTest, SyscallTraceIsRefusedWithTheReasonWhenToolsAreMissing)
{
    LinuxProcessActions actions(SyscallTrace::Tools{});
    const auto result = actions.launchSyscallTrace(ownTarget());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, syscallTraceUnavailableReason(SyscallTraceAvailability::NoTracer));
}

TEST(LinuxProcessActionsTest, SyscallTraceRefusesAnInvalidOrUnconfirmedTarget)
{
    const TestSupport::ScopedTempDir dir("tasksmack_trace_refuse");
    LinuxProcessActions actions(fakeTraceTools(dir.path));

    EXPECT_EQ(actions.launchSyscallTrace({.pid = 0, .startTimeTicks = 1}).errorMessage, "Invalid PID");
    EXPECT_EQ(actions.launchSyscallTrace({.pid = -5, .startTimeTicks = 1}).errorMessage, "Invalid PID");

    ProcessTarget unknown = ownTarget();
    unknown.startTimeTicks = 0;
    EXPECT_FALSE(actions.launchSyscallTrace(unknown).success);

    ProcessTarget reused = ownTarget();
    reused.startTimeTicks += 1;
    const auto result = actions.launchSyscallTrace(reused);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("different process"), std::string::npos) << result.errorMessage;

    // None of those opened a terminal.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "argv.txt"));
}

TEST(LinuxProcessActionsTest, SyscallTraceOpensTheTerminalWithStraceOrExplainsPtrace)
{
    const TestSupport::ScopedTempDir dir("tasksmack_trace_launch");
    const SyscallTrace::Tools tools = fakeTraceTools(dir.path);
    LinuxProcessActions actions(tools);
    const ProcessTarget self = ownTarget();
    const auto result = actions.launchSyscallTrace(self);

    std::ifstream scopeFile("/proc/sys/kernel/yama/ptrace_scope");
    int scope = 0;
    scopeFile >> scope;
    // What decides Yama's verdict is CAP_SYS_PTRACE, not EUID 0: a root container can run with it
    // dropped. The fake tracer carries no file capability, so only root's effective set can grant it;
    // an unreadable CapEff is treated as not having it, as the implementation does.
    std::ifstream statusFile("/proc/self/status");
    const std::string status((std::istreambuf_iterator<char>(statusFile)), std::istreambuf_iterator<char>());
    const bool hasSysPtrace = SyscallTrace::rootTracerHasSysPtrace(geteuid() == 0, ProcPrivileges::parseCapEff(status));
    if (!hasSysPtrace && (scope == 1 || scope == 2))
    {
        // Yama would refuse strace's attach, so no terminal is opened and the message says why.
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.errorMessage.find("/proc/sys/kernel/yama/ptrace_scope"), std::string::npos) << result.errorMessage;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_FALSE(std::filesystem::exists(dir.path / "argv.txt"));
        return;
    }
    if (scope == 3)
    {
        EXPECT_FALSE(result.success);
        return;
    }

    ASSERT_TRUE(result.success) << result.errorMessage;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(dir.path / "argv.txt") && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::ifstream recorded(dir.path / "argv.txt");
    std::vector<std::string> args;
    for (std::string line; std::getline(recorded, line);)
    {
        args.push_back(line);
    }
    EXPECT_EQ(args, (std::vector<std::string>{"-e", tools.tracerPath, "-f", "-tt", "-p", std::to_string(self.pid)}));
}

// ============================================================================
// Create dump file with gcore (#807)
// ============================================================================
// A fake gcore stands in for gdb's: it records its arguments and writes (or doesn't) the "<prefix>.<pid>"
// file gcore would. ptrace is faked to allow the attach, so what runs after the check is reached under
// any Yama scope; the refusals themselves are tested through the same seam.

/// ptrace would let gcore attach: as if TaskSmack held CAP_SYS_PTRACE.
SyscallTrace::PtraceContext ptraceAllowed(std::int32_t /*pid*/, const std::string& /*toolPath*/)
{
    return {.ptraceScope = 1, .sameCredentials = true, .privileged = true};
}

/// ptrace would refuse: Yama's "descendants only", without the capability.
SyscallTrace::PtraceContext ptraceDescendantsOnly(std::int32_t /*pid*/, const std::string& /*toolPath*/)
{
    return {.ptraceScope = 1, .sameCredentials = true, .privileged = false};
}

/// The real syscalls, with ptrace's verdict replaced by @p context.
[[nodiscard]] LinuxProcessSyscalls syscallsWithPtrace(SyscallTrace::PtraceContext (*context)(std::int32_t, const std::string&))
{
    LinuxProcessSyscalls syscalls = realProcessSyscalls();
    syscalls.ptraceContext = context;
    return syscalls;
}

/// Tools with only a gcore: the script at @p dir/fake-gcore running @p body, after recording its
/// arguments (one per line) in @p dir/gcore-args.txt.
[[nodiscard]] SyscallTrace::Tools fakeDumpTools(const std::filesystem::path& dir, const std::string& body)
{
    const std::filesystem::path gcore = dir / "fake-gcore";
    writeScript(gcore, std::format("for a in \"$@\"; do printf '%s\\n' \"$a\"; done > '{}/gcore-args.txt'\n{}", dir.string(), body));
    return {.tracerPath = {}, .terminal = std::nullopt, .coreDumperPath = gcore.string(), .folderOpenerPath = {}};
}

/// The files in @p dir, by name.
[[nodiscard]] std::vector<std::string> filesIn(const std::filesystem::path& dir)
{
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

/// A thread that requests @p stop once gcore's "<name>.partial.<pid>" for @p pid shows in @p dir (or after
/// ten seconds): the dump is under way. The final name is reserved before gcore starts (#2147), so any
/// file at all would show too soon, before the fake gcore has done what comes before its write (#2479).
[[nodiscard]] std::jthread cancelOncePartialShows(std::stop_source& stop, const std::filesystem::path& dir, std::int32_t pid)
{
    return std::jthread(
        // NOLINTNEXTLINE(bugprone-exception-escape) - a test's helper thread: what escapes it ends the run, as a failed test should
        [&stop, dir, pid]
        {
            const std::string suffix = std::format(".partial.{}", pid);
            const auto partialShows = [&dir, &suffix]
            {
                return std::ranges::any_of(filesIn(dir), [&suffix](const std::string& name) { return name.ends_with(suffix); });
            };
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!partialShows() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            stop.request_stop();
        });
}

/// ptrace would allow strace only through TaskSmack's own root privilege (Yama mode 1).
SyscallTrace::PtraceContext ptraceAllowedAsRoot(std::int32_t /*pid*/, const std::string& /*toolPath*/)
{
    return {.ptraceScope = 1, .sameCredentials = true, .privileged = true, .privilegeIsInherited = true};
}

// #2140: gnome-terminal (here reached through an x-terminal-emulator link, as on Ubuntu) has its
// server start strace, as the desktop user, so a trace that needs TaskSmack's root is refused up front
// rather than reported as opened; a terminal that runs strace itself is used as before.
TEST(LinuxProcessActionsTest, SyscallTraceNeedingRootRefusesATerminalThatHandsItToItsServer)
{
    const TestSupport::ScopedTempDir dir("tasksmack_trace_server_terminal");
    SyscallTrace::Tools tools = fakeTraceTools(dir.path);
    const std::filesystem::path server = dir.path / "gnome-terminal";
    std::filesystem::copy_file(dir.path / "fake-terminal", server); // fakeTraceTools()'s terminal
    const std::filesystem::path link = dir.path / "x-terminal-emulator";
    std::filesystem::create_symlink(server, link);
    tools.terminal = SyscallTrace::Terminal{.path = link.string(), .syntax = SyscallTrace::TerminalSyntax::DashE};

    LinuxProcessActions viaServer(tools, syscallsWithPtrace(&ptraceAllowedAsRoot));
    const auto refused = viaServer.launchSyscallTrace(ownTarget());
    EXPECT_FALSE(refused.success);
    EXPECT_TRUE(refused.errorMessage.starts_with("gnome-terminal opens its windows from your desktop session")) << refused.errorMessage;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "argv.txt"));

    // The same privilege through a terminal that runs the command itself: opened.
    LinuxProcessActions viaChild(fakeTraceTools(dir.path), syscallsWithPtrace(&ptraceAllowedAsRoot));
    EXPECT_TRUE(viaChild.launchSyscallTrace(ownTarget()).success);
}

TEST(LinuxProcessActionsDumpTest, DumpsAreOfferedOnlyWhereGcoreIsInstalled)
{
    const TestSupport::ScopedTempDir dir("tasksmack_dump_caps");
    EXPECT_TRUE(LinuxProcessActions(fakeDumpTools(dir.path, "exit 0\n")).actionCapabilities().canCreateDump);
    EXPECT_FALSE(LinuxProcessActions(SyscallTrace::Tools{}).actionCapabilities().canCreateDump);

    // And refused as unsupported where it isn't.
    LinuxProcessActions without(SyscallTrace::Tools{});
    const std::stop_source stop;
    const ProcessDumpResult refused = without.createDump(ownTarget(), dir.path, stop.get_token());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), DUMP_UNSUPPORTED_MESSAGE);
}

// #2640: tasksmack-helper links only LinuxProcessControl, which starts no other program: no system call
// trace and no dump files, however the machine is set up, and both refused as unsupported. Everything
// else it reports as LinuxProcessActions does, with strace, a terminal and gcore all found.
TEST(LinuxProcessControlTest, OffersNoTraceOrDumpButTheSameControlAsLinuxProcessActions)
{
    const TestSupport::ScopedTempDir dir("tasksmack_control_caps");
    SyscallTrace::Tools tools = fakeTraceTools(dir.path);
    tools.coreDumperPath = fakeDumpTools(dir.path, "exit 0\n").coreDumperPath;
    const ProcessActionCapabilities full = LinuxProcessActions(tools).actionCapabilities();
    ASSERT_EQ(full.syscallTrace, SyscallTraceAvailability::Available);
    ASSERT_TRUE(full.canCreateDump);

    LinuxProcessControl control;
    const ProcessActionCapabilities caps = control.actionCapabilities();
    EXPECT_EQ(caps.syscallTrace, SyscallTraceAvailability::Unsupported);
    EXPECT_FALSE(caps.canCreateDump);
    EXPECT_EQ(caps.canTerminate, full.canTerminate);
    EXPECT_EQ(caps.canKill, full.canKill);
    EXPECT_EQ(caps.canStop, full.canStop);
    EXPECT_EQ(caps.canContinue, full.canContinue);
    EXPECT_EQ(caps.canSetPriority, full.canSetPriority);
    EXPECT_EQ(caps.canSetIoPriority, full.canSetIoPriority);
    EXPECT_EQ(caps.canSetRealtimeIoPriority, full.canSetRealtimeIoPriority);
    EXPECT_EQ(caps.canSetAffinity, full.canSetAffinity);

    EXPECT_FALSE(control.launchSyscallTrace(ownTarget()).success);
    const std::stop_source stop;
    const ProcessDumpResult refused = control.createDump(ownTarget(), dir.path, stop.get_token());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), DUMP_UNSUPPORTED_MESSAGE);
}

TEST(LinuxProcessActionsDumpTest, GcoreWritesTheCoreUnderItsFinalName)
{
    const TestSupport::ScopedTempDir work("tasksmack_dump_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_out");
    // As gcore does: writes "<prefix>.<pid>" ($2 and $3 after -o).
    LinuxProcessActions actions(fakeDumpTools(work.path, "printf 'CORE' > \"$2.$3\"\n"), syscallsWithPtrace(&ptraceAllowed));
    const ProcessTarget self = ownTarget();
    const std::stop_source stop;
    const ProcessDumpResult dump = actions.createDump(self, dumps.path, stop.get_token());
    ASSERT_TRUE(dump.has_value()) << dump.error();

    const std::string name = dump->file.filename().string();
    EXPECT_EQ(dump->file.parent_path(), dumps.path);
    EXPECT_TRUE(name.contains(std::format("_{}_", self.pid))) << name;
    EXPECT_TRUE(name.ends_with(".core")) << name;
    EXPECT_EQ(dump->sizeBytes, 4U);
    EXPECT_FALSE(dump->fromSnapshot);
    EXPECT_FALSE(dump->liveReason.empty());
    EXPECT_EQ(filesIn(dumps.path), (std::vector<std::string>{name})) << "the partial name was renamed away";

    // gcore was asked for the core of this process, under a partial name beside the final one.
    std::ifstream recorded(work.path / "gcore-args.txt");
    std::vector<std::string> args;
    for (std::string line; std::getline(recorded, line);)
    {
        args.push_back(line);
    }
    EXPECT_EQ(args, (std::vector<std::string>{"-o", dump->file.string() + ".partial", std::to_string(self.pid)}));
}

// #2147: the final name is reserved (created, empty) before gcore runs, so two dumps of one process
// started in the same second get different names and never share a ".partial" file.
TEST(LinuxProcessActionsDumpTest, TheFinalNameIsReservedWhileGcoreRuns)
{
    const TestSupport::ScopedTempDir work("tasksmack_dump_reserve_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_reserve_out");
    // Fails unless the name it will be renamed to is already there ($2 is "<final>.partial").
    LinuxProcessActions actions(fakeDumpTools(work.path, "test -e \"${2%.partial}\" || exit 3\nprintf 'CORE' > \"$2.$3\"\n"),
                                syscallsWithPtrace(&ptraceAllowed));
    const std::stop_source stop;
    const ProcessDumpResult first = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    ASSERT_TRUE(first.has_value()) << first.error();
    const ProcessDumpResult second = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_NE(first->file, second->file);
    EXPECT_EQ(filesIn(dumps.path).size(), 2U);
}

// #2148: gcore (and the gdb it runs) inherits only stdin, stdout and stderr: a descriptor TaskSmack holds
// open without close-on-exec doesn't pass into it.
TEST(LinuxProcessActionsDumpTest, GcoreInheritsNoneOfTaskSmacksDescriptors)
{
    const TestSupport::ScopedTempDir work("tasksmack_dump_fds_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_fds_out");
    // Lists its own descriptors into the core it "writes".
    LinuxProcessActions actions(fakeDumpTools(work.path, "ls /proc/$$/fd > \"$2.$3\"\n"), syscallsWithPtrace(&ptraceAllowed));
    const int leaky = ::open("/proc/self/stat", O_RDONLY); // NOLINT(cppcoreguidelines-pro-type-vararg) - no O_CLOEXEC, on purpose
    ASSERT_GE(leaky, 3);
    const std::stop_source stop;
    const ProcessDumpResult dump = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    ::close(leaky);
    ASSERT_TRUE(dump.has_value()) << dump.error();
    std::ifstream listing(dump->file);
    for (std::string fd; listing >> fd;)
    {
        EXPECT_NE(fd, std::to_string(leaky));
    }
}

TEST(LinuxProcessActionsDumpTest, AGcoreThatFailsSaysWhyAndLeavesNothing)
{
    const TestSupport::ScopedTempDir work("tasksmack_dump_fail_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_fail_out");
    // A partial file, then the error gdb prints when the attach is refused.
    LinuxProcessActions actions(
        fakeDumpTools(work.path, "printf 'PART' > \"$2.$3\"\necho 'ptrace: Operation not permitted.' >&2\nexit 1\n"),
        syscallsWithPtrace(&ptraceAllowed));
    const std::stop_source stop;
    const ProcessDumpResult dump = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    ASSERT_FALSE(dump.has_value());
    EXPECT_TRUE(dump.error().ends_with(": ptrace: Operation not permitted.")) << dump.error();
    EXPECT_TRUE(filesIn(dumps.path).empty()) << "the partial file was deleted";

    // Exit status 0 with no core written is a failure too.
    LinuxProcessActions silent(fakeDumpTools(work.path, "exit 0\n"), syscallsWithPtrace(&ptraceAllowed));
    const ProcessDumpResult none = silent.createDump(ownTarget(), dumps.path, stop.get_token());
    ASSERT_FALSE(none.has_value());
    EXPECT_TRUE(none.error().contains("exit status 0")) << none.error();
}

TEST(LinuxProcessActionsDumpTest, ACancelledDumpKillsGcoreAndDeletesThePartialFile)
{
    const TestSupport::ScopedTempDir work("tasksmack_dump_cancel_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_cancel_out");
    // Writes part of the core, then hangs as a long dump would (sleep runs in gcore's process group).
    LinuxProcessActions actions(fakeDumpTools(work.path, "printf 'PART' > \"$2.$3\"\nsleep 30\n"), syscallsWithPtrace(&ptraceAllowed));
    std::stop_source stop;
    std::jthread canceller = cancelOncePartialShows(stop, dumps.path, ownTarget().pid);
    const auto started = std::chrono::steady_clock::now();
    const ProcessDumpResult dump = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    canceller.join();
    ASSERT_FALSE(dump.has_value());
    EXPECT_EQ(dump.error(), "Cancelled; the partial dump file was deleted");
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(20)) << "didn't wait for the sleep";
    EXPECT_TRUE(filesIn(dumps.path).empty());
}

// #2122: a cancelled dump wakes a target that was running before and that the dump left stopped (gdb's
// attach stop, here sent by the fake gcore itself).
TEST(LinuxProcessActionsDumpTest, ACancelledDumpWakesATargetItLeftStopped)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    const TestSupport::ScopedTempDir work("tasksmack_dump_wake_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_wake_out");
    LinuxProcessActions actions(fakeDumpTools(work.path, "kill -STOP \"$3\"\nprintf 'PART' > \"$2.$3\"\nsleep 30\n"),
                                syscallsWithPtrace(&ptraceAllowed));
    const auto stateOf = [pid = child.target().pid]
    {
        std::ifstream stat(std::format("/proc/{}/stat", pid));
        const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
        return LinuxCoreDump::stateFromStat(line).value_or('?');
    };
    std::stop_source stop;
    // Not before the partial file shows (#2479): the reserved name shows at once, before the SIGSTOP.
    std::jthread canceller = cancelOncePartialShows(stop, dumps.path, child.target().pid);
    const ProcessDumpResult dump = actions.createDump(child.target(), dumps.path, stop.get_token());
    canceller.join();
    ASSERT_FALSE(dump.has_value());
    EXPECT_EQ(dump.error(), "Cancelled; the partial dump file was deleted");
    EXPECT_FALSE(LinuxCoreDump::isStoppedState(stateOf())) << "state " << stateOf();
}

/// The tool path the last ptrace check was asked about, by ptraceAllowedRecordingTool.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - a plain function pointer hook can't capture
std::string g_PtraceCheckedTool;

/// ptraceAllowed, recording which tool's privilege was asked about in g_PtraceCheckedTool.
SyscallTrace::PtraceContext ptraceAllowedRecordingTool(std::int32_t pid, const std::string& toolPath)
{
    g_PtraceCheckedTool = toolPath;
    return ptraceAllowed(pid, toolPath);
}

// #2148 (#2479): gcore is a script, and a file capability on a script is never applied, so createDump must
// ask whether the gdb beside it may attach, not the gcore script itself.
TEST(LinuxProcessActionsDumpTest, ThePtraceCheckIsForTheGdbGcoreRuns)
{
    const TestSupport::ScopedTempDir work("tasksmack_dump_gdb_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_gdb_out");
    const SyscallTrace::Tools tools = fakeDumpTools(work.path, "printf 'CORE' > \"$2.$3\"\n");
    LinuxProcessActions actions(tools, syscallsWithPtrace(&ptraceAllowedRecordingTool));
    g_PtraceCheckedTool.clear();
    const std::stop_source stop;
    const ProcessDumpResult dump = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    ASSERT_TRUE(dump.has_value()) << dump.error();
    EXPECT_EQ(g_PtraceCheckedTool, (work.path / "gdb").string());
    EXPECT_NE(g_PtraceCheckedTool, tools.coreDumperPath);
}

/// The child the last waitpid was asked about, by waitPidFailingOnce: gcore, which leads its process group.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - a plain function pointer hook can't capture
pid_t g_WaitedChild = -1;
/// Whether waitPidFailingOnce has failed yet.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - a plain function pointer hook can't capture
bool g_WaitFailed = false;

/// waitpid() that fails once with ECHILD, as if the child were lost to another reaper, then is the real one.
pid_t waitPidFailingOnce(pid_t pid, int* status, int options)
{
    g_WaitedChild = pid;
    if (!g_WaitFailed)
    {
        g_WaitFailed = true;
        errno = ECHILD;
        return -1;
    }
    return ::waitpid(pid, status, options);
}

// #2148 (#2479): when waiting on gcore fails, its process group (gcore and the gdb it runs, maybe attached to
// the target) is killed and reaped, not left running.
TEST(LinuxProcessActionsDumpTest, AFailedWaitOnGcoreStillKillsItsProcessGroup)
{
    const TestSupport::ScopedTempDir work("tasksmack_dump_wait_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_wait_out");
    LinuxProcessSyscalls syscalls = syscallsWithPtrace(&ptraceAllowed);
    syscalls.waitPid = &waitPidFailingOnce;
    g_WaitedChild = -1;
    g_WaitFailed = false;
    // Hangs as a long dump would; exec, so the group is that one process, which only this test can reap.
    LinuxProcessActions actions(fakeDumpTools(work.path, "exec sleep 30\n"), syscalls);
    const std::stop_source stop;
    const ProcessDumpResult dump = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    EXPECT_FALSE(dump.has_value());
    ASSERT_TRUE(g_WaitFailed);
    ASSERT_GT(g_WaitedChild, 0);
    errno = 0;
    const bool groupGone = ::kill(-g_WaitedChild, 0) != 0 && errno == ESRCH;
    EXPECT_TRUE(groupGone) << "gcore's process group " << g_WaitedChild << " is still there";
    if (!groupGone)
    {
        ::kill(-g_WaitedChild, SIGKILL);
        ::waitpid(g_WaitedChild, nullptr, 0);
    }
    EXPECT_TRUE(filesIn(dumps.path).empty());
}

TEST(LinuxProcessActionsDumpTest, ADumpPtraceWouldRefuseIsRefusedBeforeGcoreRuns)
{
    const TestSupport::ScopedTempDir work("tasksmack_dump_refused_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_refused_out");
    LinuxProcessActions actions(fakeDumpTools(work.path, "exit 0\n"), syscallsWithPtrace(&ptraceDescendantsOnly));
    const std::stop_source stop;
    const ProcessDumpResult dump = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    ASSERT_FALSE(dump.has_value());
    EXPECT_TRUE(dump.error().contains("so gcore cannot attach to process")) << dump.error();
    EXPECT_FALSE(std::filesystem::exists(work.path / "gcore-args.txt")) << "gcore never ran";
}

TEST(LinuxProcessActionsDumpTest, ADumpIsRefusedForAnInvalidTargetOrAMissingFolder)
{
    const TestSupport::ScopedTempDir work("tasksmack_dump_bad_tools");
    LinuxProcessActions actions(fakeDumpTools(work.path, "exit 0\n"), syscallsWithPtrace(&ptraceAllowed));
    const std::stop_source stop;
    const ProcessDumpResult invalid = actions.createDump({.pid = 0, .startTimeTicks = 1}, work.path, stop.get_token());
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error(), "Invalid PID");

    ProcessTarget reused = ownTarget();
    reused.startTimeTicks += 1;
    const ProcessDumpResult other = actions.createDump(reused, work.path, stop.get_token());
    ASSERT_FALSE(other.has_value());
    EXPECT_TRUE(other.error().contains("different process")) << other.error();

    const ProcessDumpResult nowhere = actions.createDump(ownTarget(), work.path / "missing", stop.get_token());
    ASSERT_FALSE(nowhere.has_value());
    EXPECT_TRUE(nowhere.error().contains("doesn't exist")) << nowhere.error();
    EXPECT_FALSE(std::filesystem::exists(work.path / "gcore-args.txt")) << "gcore never ran";
}

TEST(LinuxProcessActionsDumpTest, DumpSpaceReadsTheFolderAndTheProcess)
{
    // The confirm's free-space check (#2001): this process has anonymous memory, and the temp folder free space.
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_space");
    LinuxProcessActions actions(SyscallTrace::Tools{});
    const DumpSpace space = actions.dumpSpace(ownTarget(), dumps.path);
    EXPECT_GT(space.freeBytes.value_or(0), 0U);
    EXPECT_GT(space.processBytes.value_or(0), 0U);

    // Not the target any more: its memory isn't counted. No folder: no free space.
    ProcessTarget reused = ownTarget();
    reused.startTimeTicks += 1;
    EXPECT_EQ(actions.dumpSpace(reused, dumps.path).processBytes, std::nullopt);
    EXPECT_EQ(actions.dumpSpace(ownTarget(), {}).freeBytes, std::nullopt);
    EXPECT_EQ(actions.dumpSpace(ownTarget(), dumps.path / "missing").freeBytes, std::nullopt);
}

// ============================================================================
// Kernel failures, through LinuxProcessActions' syscall table (#1548)
// ============================================================================
// The actions still check the target's start time against the real /proc, so these aim at this test
// process; the fake calls answer for the kernel, so nothing is ever actually signalled or reniced.

/// What the fake system calls do, and what they were asked.
struct FaultPlan
{
    int openErrno = 0;                     ///< pidfd_open fails with this, when set
    int sendErrno = 0;                     ///< pidfd_send_signal fails with this, when set
    int pollEintrs = 0;                    ///< poll() is interrupted this many times first
    int pollErrno = 0;                     ///< then fails with this, when set
    int pollResult = 0;                    ///< else 1 (the process exited) or 0
    int setErrno = 0;                      ///< setpriority / ioprio_set / sched_setaffinity fail with this, when set
    std::vector<std::size_t> lastAffinity; ///< The CPUs the last sched_setaffinity was given
    int ioprioGetValue = 0;                ///< ioprio_get returns this
    int ioprioGetErrno = 0;                ///< or fails with this, when set
    int sendCalls = 0;
    int lastSignal = 0;
    int setCalls = 0;
    int pollCalls = 0;
};

FaultPlan& plan()
{
    static FaultPlan state;
    return state;
}

int fail(int err)
{
    errno = err;
    return -1;
}

int fakePidfdOpen(pid_t /*pid*/)
{
    // A real descriptor, so the action's FdGuard closes something that is ours.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
    return plan().openErrno != 0 ? fail(plan().openErrno) : ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}

int fakePidfdSendSignal(int /*pidfd*/, int signal)
{
    ++plan().sendCalls;
    plan().lastSignal = signal;
    return plan().sendErrno != 0 ? fail(plan().sendErrno) : 0;
}

int fakePollPidfd(int /*pidfd*/)
{
    ++plan().pollCalls;
    if (plan().pollEintrs > 0)
    {
        --plan().pollEintrs;
        return fail(EINTR);
    }
    return plan().pollErrno != 0 ? fail(plan().pollErrno) : plan().pollResult;
}

int fakeSet(id_t /*tid*/, int /*value*/)
{
    ++plan().setCalls;
    return plan().setErrno != 0 ? fail(plan().setErrno) : 0;
}

int fakeSetAffinity(id_t /*tid*/, std::size_t setSize, const cpu_set_t* set)
{
    ++plan().setCalls;
    plan().lastAffinity.clear();
    for (std::size_t cpu = 0; cpu < setSize * 8; ++cpu)
    {
        if (CPU_ISSET_S(cpu, setSize, set))
        {
            plan().lastAffinity.push_back(cpu);
        }
    }
    return plan().setErrno != 0 ? fail(plan().setErrno) : 0;
}

int fakeIoprioGet(pid_t /*pid*/)
{
    return plan().ioprioGetErrno != 0 ? fail(plan().ioprioGetErrno) : plan().ioprioGetValue;
}

class LinuxProcessActionsFaultTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        plan() = FaultPlan{};
    }

    LinuxProcessActions m_Actions{SyscallTrace::Tools{},
                                  LinuxProcessSyscalls{.pidfdOpen = &fakePidfdOpen,
                                                       .pidfdSendSignal = &fakePidfdSendSignal,
                                                       .pollPidfd = &fakePollPidfd,
                                                       .setPriority = &fakeSet,
                                                       .ioprioSet = &fakeSet,
                                                       .ioprioGet = &fakeIoprioGet,
                                                       .setAffinity = &fakeSetAffinity}};
};

TEST_F(LinuxProcessActionsFaultTest, SignalsGoThroughThePidfd)
{
    EXPECT_TRUE(m_Actions.terminate(ownTarget()).success);
    EXPECT_EQ(plan().lastSignal, SIGTERM);
    EXPECT_TRUE(m_Actions.kill(ownTarget()).success);
    EXPECT_EQ(plan().lastSignal, SIGKILL);
    // Not this process: it is never suspended, even through fake system calls (#2641).
    const SleepingChild other;
    ASSERT_TRUE(other.started());
    EXPECT_TRUE(m_Actions.stop(other.target()).success);
    EXPECT_EQ(plan().lastSignal, SIGSTOP);
    EXPECT_TRUE(m_Actions.resume(ownTarget()).success);
    EXPECT_EQ(plan().lastSignal, SIGCONT);
    EXPECT_EQ(plan().sendCalls, 4);
}

// #1483 (#2479): with CAP_SYS_NICE but not CAP_KILL, signal 0 to another user's process fails with EPERM,
// so the priority actions must confirm the target through the pidfd's poll, never a signal.
TEST_F(LinuxProcessActionsFaultTest, PriorityChangesNeverNeedASignalToConfirmTheTarget)
{
    plan().sendErrno = EPERM;
    plan().pollResult = 0; // still running
    const ProcessActionResult priority = m_Actions.setPriority(ownTarget(), 5);
    EXPECT_TRUE(priority.success) << priority.errorMessage;
    const ProcessActionResult ioPriority = m_Actions.setIoPriority(ownTarget(), IoPriorityClass::BestEffort, 4);
    EXPECT_TRUE(ioPriority.success) << ioPriority.errorMessage;
    EXPECT_EQ(plan().sendCalls, 0);
    EXPECT_GE(plan().pollCalls, 2);
}

TEST_F(LinuxProcessActionsFaultTest, WithoutPidfdsEveryActionIsRefused)
{
    // ENOSYS: a kernel before 5.3. EPERM/EACCES: a seccomp filter, not "another user".
    for (const int err : {ENOSYS, EPERM, EACCES})
    {
        plan().openErrno = err;
        const ProcessActionResult kill = m_Actions.kill(ownTarget());
        EXPECT_FALSE(kill.success);
        EXPECT_TRUE(kill.errorMessage.contains("pidfd")) << kill.errorMessage;
        EXPECT_FALSE(m_Actions.setPriority(ownTarget(), 5).success);
        EXPECT_FALSE(m_Actions.setIoPriority(ownTarget(), IoPriorityClass::BestEffort, 4).success);
        EXPECT_FALSE(m_Actions.getIoPriority(ownTarget()).has_value());
    }
    EXPECT_EQ(plan().sendCalls, 0); // Never sent to the bare PID instead
    EXPECT_EQ(plan().setCalls, 0);
}

TEST_F(LinuxProcessActionsFaultTest, AProcessGoneBeforeThePidfdOpensIsNotFound)
{
    plan().openErrno = ESRCH;
    EXPECT_EQ(m_Actions.kill(ownTarget()).errorMessage, "Process not found - may have already exited");
}

TEST_F(LinuxProcessActionsFaultTest, ASignalTheKernelRefusesSaysWhy)
{
    plan().sendErrno = ESRCH;
    EXPECT_EQ(m_Actions.terminate(ownTarget()).errorMessage, "Process not found - may have already exited");
    plan().sendErrno = EPERM;
    EXPECT_EQ(m_Actions.kill(ownTarget()).errorMessage, "Permission denied - process belongs to another user");
    plan().sendErrno = EINVAL;
    const SleepingChild other; // not this process, which is never suspended (#2641)
    ASSERT_TRUE(other.started());
    EXPECT_EQ(m_Actions.stop(other.target()).errorMessage, "Invalid signal");
    // Anything else: the system's own words for it.
    plan().sendErrno = EIO;
    EXPECT_EQ(m_Actions.resume(ownTarget()).errorMessage, std::system_category().message(EIO));
}

TEST_F(LinuxProcessActionsFaultTest, AProcessWhoseStartTimeCantBeReadIsNotFound)
{
    // The pidfd opens (the fake's), but there is no /proc/<pid>/stat to confirm the start time against
    // (#1395): the PID is above any pid_max.
    const ProcessTarget gone{.pid = 0x3FFFFFFF, .startTimeTicks = 12345};
    EXPECT_EQ(m_Actions.kill(gone).errorMessage, "Process not found - may have already exited");
    EXPECT_EQ(plan().sendCalls, 0);
}

TEST_F(LinuxProcessActionsFaultTest, AnIoPriorityClassOutOfRangeIsRefused)
{
    // Out of range on purpose: the guard against a class no enumerator names is what is under test.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto outOfRange = static_cast<IoPriorityClass>(200);
    const ProcessActionResult result = m_Actions.setIoPriority(ownTarget(), outOfRange, 0);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Invalid I/O priority class");
    EXPECT_EQ(plan().setCalls, 0);
}

TEST_F(LinuxProcessActionsFaultTest, ATraceWithoutAPidfdIsRefusedBeforeAnythingOpens)
{
    const TestSupport::ScopedTempDir dir("tasksmack_trace_nopidfd");
    LinuxProcessActions actions(fakeTraceTools(dir.path),
                                LinuxProcessSyscalls{.pidfdOpen = &fakePidfdOpen,
                                                     .pidfdSendSignal = &fakePidfdSendSignal,
                                                     .pollPidfd = &fakePollPidfd,
                                                     .setPriority = &fakeSet,
                                                     .ioprioSet = &fakeSet,
                                                     .ioprioGet = &fakeIoprioGet});
    plan().openErrno = ESRCH;
    EXPECT_EQ(actions.launchSyscallTrace(ownTarget()).errorMessage, "Process not found - may have already exited");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "argv.txt"));
}

TEST_F(LinuxProcessActionsFaultTest, PriorityReachesEveryThreadAndIsConfirmed)
{
    EXPECT_TRUE(m_Actions.setPriority(ownTarget(), 5).success);
    EXPECT_GE(plan().setCalls, 1); // One per thread of this process
    EXPECT_EQ(plan().pollCalls, 1);
    EXPECT_TRUE(m_Actions.setIoPriority(ownTarget(), IoPriorityClass::Idle, 0).success);
}

TEST_F(LinuxProcessActionsFaultTest, ATargetThatExitedDuringTheChangeIsReported)
{
    plan().pollResult = 1;
    const ProcessActionResult result = m_Actions.setPriority(ownTarget(), 5);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.contains("exited while its priority was being changed")) << result.errorMessage;
    const ProcessActionResult io = m_Actions.setIoPriority(ownTarget(), IoPriorityClass::BestEffort, 2);
    EXPECT_TRUE(io.errorMessage.contains("exited while its I/O priority was being changed")) << io.errorMessage;
}

TEST_F(LinuxProcessActionsFaultTest, AConfirmationThatFailsLeavesTheChangeUnconfirmed)
{
    plan().pollErrno = EBADF;
    const ProcessActionResult result = m_Actions.setPriority(ownTarget(), 5);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.contains("could not be confirmed")) << result.errorMessage;
}

TEST_F(LinuxProcessActionsFaultTest, AnInterruptedConfirmationIsRetried)
{
    plan().pollEintrs = 2;
    EXPECT_TRUE(m_Actions.setPriority(ownTarget(), 5).success);
    EXPECT_EQ(plan().pollCalls, 3);
}

TEST_F(LinuxProcessActionsFaultTest, APriorityTheKernelRefusesIsAnError)
{
    plan().setErrno = EPERM;
    const ProcessActionResult nice = m_Actions.setPriority(ownTarget(), -5);
    EXPECT_FALSE(nice.success);
    EXPECT_FALSE(nice.errorMessage.empty());
    plan().setErrno = EINVAL;
    EXPECT_FALSE(m_Actions.setIoPriority(ownTarget(), IoPriorityClass::BestEffort, 2).success);
}

TEST_F(LinuxProcessActionsFaultTest, ReadingTheIoPriority)
{
    plan().ioprioGetValue = IoPrio::encode({.ioClass = IoPriorityClass::BestEffort, .level = 4});
    const IoPriorityReadResult read = m_Actions.getIoPriority(ownTarget());
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value_or(IoPriority{}).ioClass, IoPriorityClass::BestEffort);
    EXPECT_EQ(read.value_or(IoPriority{}).level, 4);

    plan().ioprioGetErrno = ESRCH;
    EXPECT_EQ(m_Actions.getIoPriority(ownTarget()).error_or(""), "Process not found - may have already exited");
    plan().ioprioGetErrno = EINVAL;
    EXPECT_TRUE(m_Actions.getIoPriority(ownTarget()).error_or("").starts_with("Can't read the I/O priority"));

    plan().ioprioGetErrno = 0;
    plan().pollResult = 1;
    EXPECT_TRUE(m_Actions.getIoPriority(ownTarget()).error_or("").contains("exited while its I/O priority was being read"));
    plan().pollResult = 0;
    plan().pollErrno = EBADF;
    EXPECT_TRUE(m_Actions.getIoPriority(ownTarget()).error_or("").starts_with("Can't confirm"));

    plan().pollErrno = 0;
    plan().ioprioGetValue = 0x7FFF'0000; // no class TaskSmack knows
    EXPECT_TRUE(m_Actions.getIoPriority(ownTarget()).error_or("").contains("does not recognise"));
}

// ============================================================================
// Thread listings, through the same table (#1395): what reniceThreads() meets on a busy process
// ============================================================================

/// What the fake thread listing answers, call by call (the last answer repeats), and which threads fail.
struct ListPlan
{
    std::vector<std::expected<std::vector<id_t>, std::error_code>> answers;
    bool newThreadEachCall = false; ///< every listing adds a thread not seen before
    id_t nextTid = 900100;
    std::vector<id_t> gone; ///< threads isThreadOf() no longer finds (exited, TID maybe reused)
    id_t failTid = 0;       ///< setpriority / ioprio_set on this thread fails with failErrno
    int failErrno = 0;
    std::size_t calls = 0;
};

ListPlan& listPlan()
{
    static ListPlan state;
    return state;
}

std::expected<std::vector<id_t>, std::error_code> fakeListThreads(pid_t /*pid*/)
{
    ListPlan& p = listPlan();
    auto answer = p.answers.at(std::min(p.calls, p.answers.size() - 1));
    ++p.calls;
    if (answer.has_value() && p.newThreadEachCall)
    {
        answer->push_back(p.nextTid++);
    }
    return answer;
}

bool fakeIsThreadOf(pid_t /*pid*/, id_t tid)
{
    return !std::ranges::contains(listPlan().gone, tid);
}

int fakeSetPerThread(id_t tid, int /*value*/)
{
    ++plan().setCalls;
    return (tid == listPlan().failTid && listPlan().failErrno != 0) ? fail(listPlan().failErrno) : 0;
}

class LinuxProcessActionsThreadListTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        plan() = FaultPlan{};
        listPlan() = ListPlan{};
    }

    /// The target's own PID (its leader thread) and one worker, as /proc would list them.
    [[nodiscard]] static std::vector<id_t> leaderAndWorker()
    {
        return {static_cast<id_t>(ownTarget().pid), WORKER};
    }

    static constexpr id_t WORKER = 900001;

    LinuxProcessActions m_Actions{SyscallTrace::Tools{},
                                  LinuxProcessSyscalls{.pidfdOpen = &fakePidfdOpen,
                                                       .pidfdSendSignal = &fakePidfdSendSignal,
                                                       .pollPidfd = &fakePollPidfd,
                                                       .setPriority = &fakeSetPerThread,
                                                       .ioprioSet = &fakeSetPerThread,
                                                       .ioprioGet = &fakeIoprioGet,
                                                       .listThreads = &fakeListThreads,
                                                       .isThreadOf = &fakeIsThreadOf}};
};

TEST_F(LinuxProcessActionsThreadListTest, AFirstListingThatFailsIsReportedForEitherChange)
{
    // Gone before the listing: "not found", as for a process that exited.
    listPlan().answers = {std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory))};
    const ProcessActionResult gone = m_Actions.setPriority(ownTarget(), 5);
    EXPECT_FALSE(gone.success);
    EXPECT_TRUE(gone.errorMessage.contains("Process not found")) << gone.errorMessage;
    // Any other reason is named.
    listPlan() = ListPlan{};
    listPlan().answers = {std::unexpected(std::make_error_code(std::errc::permission_denied))};
    const ProcessActionResult refused = m_Actions.setIoPriority(ownTarget(), IoPriorityClass::BestEffort, 4);
    EXPECT_FALSE(refused.success);
    EXPECT_TRUE(refused.errorMessage.contains("Can't list the threads of process")) << refused.errorMessage;
    EXPECT_EQ(plan().setCalls, 0); // nothing was changed
}

TEST_F(LinuxProcessActionsThreadListTest, AWorkerThatLeftDuringTheChangeLeavesItUnconfirmed)
{
    listPlan().answers = {leaderAndWorker()};
    listPlan().gone = {WORKER}; // set, then no longer listed under the target: its TID may be another process's now
    const ProcessActionResult result = m_Actions.setPriority(ownTarget(), 5);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.contains("exited during the change")) << result.errorMessage;
}

TEST_F(LinuxProcessActionsThreadListTest, ARelistingThatFailsSaysNewThreadsMayKeepTheOldValue)
{
    listPlan().answers = {leaderAndWorker(), std::unexpected(std::make_error_code(std::errc::io_error))};
    const ProcessActionResult result = m_Actions.setIoPriority(ownTarget(), IoPriorityClass::Idle, 0);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.contains("couldn't be listed again")) << result.errorMessage;
    EXPECT_TRUE(result.errorMessage.contains("old I/O priority")) << result.errorMessage;
}

TEST_F(LinuxProcessActionsThreadListTest, AProcessThatKeepsStartingThreadsIsReported)
{
    listPlan().answers = {leaderAndWorker()};
    listPlan().newThreadEachCall = true; // every pass finds one it hasn't set: the passes run out
    const ProcessActionResult result = m_Actions.setPriority(ownTarget(), 5);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.contains("kept starting new ones")) << result.errorMessage;
}

TEST_F(LinuxProcessActionsThreadListTest, AThreadTheKernelRefusesMakesAPartialChange)
{
    listPlan().answers = {leaderAndWorker()};
    listPlan().failTid = WORKER;
    listPlan().failErrno = EPERM;
    const ProcessActionResult result = m_Actions.setPriority(ownTarget(), 5);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.contains("changed for only 1 of 2 threads")) << result.errorMessage;
}

TEST_F(LinuxProcessActionsThreadListTest, EveryListedThreadChangedIsASuccess)
{
    listPlan().answers = {leaderAndWorker()};
    EXPECT_TRUE(m_Actions.setPriority(ownTarget(), 5).success);
    EXPECT_EQ(plan().setCalls, 2);
}

TEST(LinuxProcessSyscallsTest, TheRealTableIsComplete)
{
    const LinuxProcessSyscalls real = realProcessSyscalls();
    EXPECT_NE(real.pidfdOpen, nullptr);
    EXPECT_NE(real.pidfdSendSignal, nullptr);
    EXPECT_NE(real.pollPidfd, nullptr);
    EXPECT_NE(real.setPriority, nullptr);
    EXPECT_NE(real.ioprioSet, nullptr);
    EXPECT_NE(real.ioprioGet, nullptr);
    EXPECT_NE(real.listThreads, nullptr);
    EXPECT_NE(real.isThreadOf, nullptr);
    EXPECT_NE(real.waitPid, nullptr);
}

TEST(LinuxProcessSyscallsTest, TheRealThreadListingOfAMissingProcessSaysWhy)
{
    const auto listed = realProcessSyscalls().listThreads(0x3FFFFFFF); // above any pid_max
    ASSERT_FALSE(listed.has_value());
    EXPECT_TRUE(static_cast<bool>(listed.error()));
    EXPECT_FALSE(realProcessSyscalls().isThreadOf(0x3FFFFFFF, 1));
}

// #1887: affinity goes to every thread as one dynamically sized set -- a CPU past the fixed
// cpu_set_t's 1024 kept -- and is confirmed through the pidfd like the other per-thread changes.
TEST_F(LinuxProcessActionsFaultTest, AffinityReachesEveryThreadWithEveryCpu)
{
    CpuAffinity cpus;
    cpus.set(0);
    cpus.set(2);
    cpus.set(1100);
    const ProcessActionResult result = m_Actions.setAffinity(ownTarget(), cpus);
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_GE(plan().setCalls, 1); // One per thread of this process
    EXPECT_EQ(plan().pollCalls, 1);
    EXPECT_EQ(plan().lastAffinity, (std::vector<std::size_t>{0, 2, 1100}));
    EXPECT_TRUE(m_Actions.actionCapabilities().canSetAffinity);
}

TEST_F(LinuxProcessActionsFaultTest, AffinityErrorsSayWhy)
{
    plan().setErrno = EPERM;
    const ProcessActionResult denied = m_Actions.setAffinity(ownTarget(), CpuAffinity::fromMask(1));
    EXPECT_FALSE(denied.success);
    EXPECT_TRUE(denied.errorMessage.contains("CAP_SYS_NICE")) << denied.errorMessage;
    plan().setErrno = EINVAL;
    const ProcessActionResult invalid = m_Actions.setAffinity(ownTarget(), CpuAffinity::fromMask(1));
    EXPECT_TRUE(invalid.errorMessage.contains("cgroup")) << invalid.errorMessage;
    plan().setErrno = 0;
    plan().openErrno = ENOSYS;
    const int callsBefore = plan().setCalls;
    EXPECT_TRUE(m_Actions.setAffinity(ownTarget(), CpuAffinity::fromMask(1)).errorMessage.contains("pidfd"));
    EXPECT_EQ(plan().setCalls, callsBefore) << "never sent to the bare PID";
}

TEST_F(LinuxProcessActionsFaultTest, AffinityWithoutTheCallIsUnavailable)
{
    LinuxProcessActions actions{SyscallTrace::Tools{},
                                LinuxProcessSyscalls{.pidfdOpen = &fakePidfdOpen,
                                                     .pidfdSendSignal = &fakePidfdSendSignal,
                                                     .pollPidfd = &fakePollPidfd,
                                                     .setPriority = &fakeSet,
                                                     .ioprioSet = &fakeSet,
                                                     .ioprioGet = &fakeIoprioGet}};
    EXPECT_FALSE(actions.actionCapabilities().canSetAffinity);
    // #1891: and asked anyway, it refuses rather than calling a missing function.
    EXPECT_EQ(actions.setAffinity(ownTarget(), CpuAffinity::fromMask(1)).errorMessage,
              "Setting CPU affinity isn't available on this system");
}

/// A thread listing that fails with the error in g_ListThreadsError (#1891).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - a plain function pointer hook can't capture
std::errc g_ListThreadsError = std::errc::no_such_file_or_directory;
std::expected<std::vector<id_t>, std::error_code> failingListThreads(pid_t /*pid*/)
{
    return std::unexpected(std::make_error_code(g_ListThreadsError));
}

// #1891: when the target's threads can't be listed, affinity says so -- a vanished /proc entry as a process
// gone, anything else naming the reason -- and nothing is set.
TEST_F(LinuxProcessActionsFaultTest, AffinityWhenTheThreadsCantBeListedSaysWhy)
{
    LinuxProcessActions actions{SyscallTrace::Tools{},
                                LinuxProcessSyscalls{.pidfdOpen = &fakePidfdOpen,
                                                     .pidfdSendSignal = &fakePidfdSendSignal,
                                                     .pollPidfd = &fakePollPidfd,
                                                     .setPriority = &fakeSet,
                                                     .ioprioSet = &fakeSet,
                                                     .ioprioGet = &fakeIoprioGet,
                                                     .setAffinity = &fakeSetAffinity,
                                                     .listThreads = &failingListThreads}};
    g_ListThreadsError = std::errc::no_such_file_or_directory;
    EXPECT_EQ(actions.setAffinity(ownTarget(), CpuAffinity::fromMask(1)).errorMessage, "Process not found - may have already exited");
    g_ListThreadsError = std::errc::permission_denied;
    const std::string denied = actions.setAffinity(ownTarget(), CpuAffinity::fromMask(1)).errorMessage;
    EXPECT_TRUE(denied.starts_with("Can't list the threads of process")) << denied;
    EXPECT_EQ(plan().setCalls, 0);
}

// #1891: a trace aimed at a PID whose start time no longer matches is refused before anything is opened.
TEST_F(LinuxProcessActionsFaultTest, ATraceOfAReusedPidIsRefused)
{
    const TestSupport::ScopedTempDir dir("tasksmack_trace_reused");
    LinuxProcessActions actions(fakeTraceTools(dir.path),
                                LinuxProcessSyscalls{.pidfdOpen = &fakePidfdOpen,
                                                     .pidfdSendSignal = &fakePidfdSendSignal,
                                                     .pollPidfd = &fakePollPidfd,
                                                     .setPriority = &fakeSet,
                                                     .ioprioSet = &fakeSet,
                                                     .ioprioGet = &fakeIoprioGet});
    ProcessTarget reused = ownTarget();
    reused.startTimeTicks += 1;
    const ProcessActionResult result = actions.launchSyscallTrace(reused);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.contains("different process")) << result.errorMessage;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "argv.txt"));
}

/// A target that a cancelled dump's gdb holds, as /proc shows it (#2122): running ('S', untraced) before the
/// dump; once it is cancelled, still traced by gdb (TracerPid tracedPolls more times) in a tracing stop
/// ('t'), and once gdb has let go, left stopped ('T').
struct TracedTargetPlan
{
    int stateReads = 0;
    int tracerReads = 0;
    int tracedPolls = 3;          ///< TracerPid reads after the dump that still show gdb
    bool untraced = false;        ///< TracerPid has read 0 since the dump
    int sigcontsWhileTraced = 0;  ///< SIGCONTs sent before TracerPid read 0 (too soon: gdb's stop is to come)
    int sigcontsOnceUntraced = 0; ///< SIGCONTs sent after
};

TracedTargetPlan& tracedPlan()
{
    static TracedTargetPlan state;
    return state;
}

char tracedTargetState(std::int32_t /*pid*/)
{
    TracedTargetPlan& target = tracedPlan();
    if (target.stateReads++ == 0)
    {
        return 'S';
    }
    return target.untraced ? 'T' : 't';
}

std::optional<std::int32_t> tracedTargetTracer(std::int32_t /*pid*/)
{
    TracedTargetPlan& target = tracedPlan();
    if (target.tracerReads++ == 0)
    {
        return 0;
    }
    if (target.tracedPolls > 0)
    {
        --target.tracedPolls;
        return 777;
    }
    target.untraced = true;
    return 0;
}

int tracedTargetSignal(int pidfd, int signal)
{
    if (signal == SIGCONT && tracedPlan().untraced)
    {
        ++tracedPlan().sigcontsOnceUntraced;
    }
    else if (signal == SIGCONT)
    {
        ++tracedPlan().sigcontsWhileTraced;
    }
    return fakePidfdSendSignal(pidfd, signal);
}

// #2751: when waiting on gcore fails, it is killed as a cancel kills it, and the same follows: the dump waits
// for gdb to let go and wakes the target it left stopped, and the error says TaskSmack lost track of gcore
// rather than that gcore failed.
TEST_F(LinuxProcessActionsFaultTest, AFailedWaitOnGcoreWakesTheTargetAndSaysTaskSmackLostTrack)
{
    tracedPlan() = TracedTargetPlan{};
    g_WaitedChild = -1;
    g_WaitFailed = false;
    const TestSupport::ScopedTempDir work("tasksmack_dump_lost_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_lost_out");
    // The pidfd and its signals are faked, so nothing is really sent to this test process.
    LinuxProcessActions actions(fakeDumpTools(work.path, "exec sleep 30\n"),
                                LinuxProcessSyscalls{.pidfdOpen = &fakePidfdOpen,
                                                     .pidfdSendSignal = &tracedTargetSignal,
                                                     .pollPidfd = &fakePollPidfd,
                                                     .ptraceContext = &ptraceAllowed,
                                                     .waitPid = &waitPidFailingOnce,
                                                     .processState = &tracedTargetState,
                                                     .tracerPid = &tracedTargetTracer});
    const std::stop_source stop;
    const ProcessDumpResult dump = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    ASSERT_TRUE(g_WaitFailed);
    ASSERT_FALSE(dump.has_value());
    EXPECT_TRUE(dump.error().starts_with("TaskSmack lost track of ")) << dump.error();
    EXPECT_FALSE(dump.error().contains("gcore failed")) << dump.error();
    EXPECT_TRUE(tracedPlan().untraced) << "waited for TracerPid to read 0";
    EXPECT_EQ(tracedPlan().sigcontsWhileTraced, 0);
    EXPECT_EQ(tracedPlan().sigcontsOnceUntraced, 1);
    EXPECT_TRUE(filesIn(dumps.path).empty());
}

// #2122 (#2479): a cancelled dump whose killed gdb hasn't let go yet leaves the target in a tracing stop ('t',
// TracerPid set) -- which turns into 'T' once gdb exits. The dump waits until TracerPid is back to what it was,
// then wakes the target with one SIGCONT. Unlike ACancelledDumpWakesATargetItLeftStopped's SIGSTOP, which the
// code before the fix already woke, this is the ptrace stop it missed.
TEST_F(LinuxProcessActionsFaultTest, ACancelledDumpWakesAPtraceStoppedTargetOnceItsTracerIsGone)
{
    tracedPlan() = TracedTargetPlan{};
    const TestSupport::ScopedTempDir work("tasksmack_dump_traced_tools");
    const TestSupport::ScopedTempDir dumps("tasksmack_dump_traced_out");
    // The pidfd and its signals are faked, so nothing is really sent to this test process.
    LinuxProcessActions actions(fakeDumpTools(work.path, "printf 'PART' > \"$2.$3\"\nsleep 30\n"),
                                LinuxProcessSyscalls{.pidfdOpen = &fakePidfdOpen,
                                                     .pidfdSendSignal = &tracedTargetSignal,
                                                     .pollPidfd = &fakePollPidfd,
                                                     .ptraceContext = &ptraceAllowed,
                                                     .processState = &tracedTargetState,
                                                     .tracerPid = &tracedTargetTracer});
    std::stop_source stop;
    std::jthread canceller = cancelOncePartialShows(stop, dumps.path, ownTarget().pid);
    const ProcessDumpResult dump = actions.createDump(ownTarget(), dumps.path, stop.get_token());
    canceller.join();
    ASSERT_FALSE(dump.has_value());
    EXPECT_EQ(dump.error(), "Cancelled; the partial dump file was deleted");
    EXPECT_TRUE(tracedPlan().untraced) << "waited for TracerPid to read 0";
    EXPECT_EQ(tracedPlan().sigcontsWhileTraced, 0);
    EXPECT_EQ(tracedPlan().sigcontsOnceUntraced, 1);
    EXPECT_EQ(plan().sendCalls, 1);
}

// --- Open file location (#3064) --------------------------------------------------------------------
// No file manager opens: ShowItems is faked, and the "xdg-open" is a script that records its argv.

/// What the faked ShowItems was asked for, and whether it says a file manager took it.
struct ShowItemsFake
{
    std::vector<std::string> uris;
    bool answers = true;
};

ShowItemsFake& showItems()
{
    static ShowItemsFake fake;
    return fake;
}

bool fakeShowItems(const std::string& uri)
{
    showItems().uris.push_back(uri);
    return showItems().answers;
}

/// The errno the faked link read fails with.
int& linkError()
{
    static int error = 0;
    return error;
}

std::expected<std::string, int> failingExecutableLink(std::int32_t /*pid*/)
{
    return std::unexpected(linkError());
}

std::expected<std::string, int> deletedExecutableLink(std::int32_t /*pid*/)
{
    return std::string("/opt/old app/bin/tool (deleted)");
}

/// Actions whose ShowItems is faked (answering as @p answers) and whose xdg-open records its argv to
/// @p dir/argv.txt.
[[nodiscard]] LinuxProcessActions
fileLocationActions(const std::filesystem::path& dir, bool answers, LinuxProcessSyscalls syscalls = realProcessSyscalls())
{
    showItems() = ShowItemsFake{.uris = {}, .answers = answers};
    const std::filesystem::path opener = dir / "fake-xdg-open";
    writeScript(
        opener,
        std::format("for a in \"$@\"; do printf '%s\\n' \"$a\"; done > '{0}/argv.tmp'\nmv '{0}/argv.tmp' '{0}/argv.txt'\n", dir.string()));
    syscalls.showItemInFileManager = &fakeShowItems;
    return LinuxProcessActions(
        SyscallTrace::Tools{.tracerPath = {}, .terminal = std::nullopt, .coreDumperPath = {}, .folderOpenerPath = opener.string()},
        syscalls);
}

/// The argv the fake xdg-open recorded, waiting up to 5 s for it (it runs detached).
[[nodiscard]] std::vector<std::string> recordedArgv(const std::filesystem::path& dir)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(dir / "argv.txt") && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::ifstream recorded(dir / "argv.txt");
    std::vector<std::string> argv;
    for (std::string line; std::getline(recorded, line);)
    {
        argv.push_back(line);
    }
    return argv;
}

TEST(FileManagerRevealTest, FileUriPercentEncodesAllButUnreservedAndSlash)
{
    EXPECT_EQ(FileManagerReveal::fileUri("/usr/bin/bash"), "file:///usr/bin/bash");
    EXPECT_EQ(FileManagerReveal::fileUri("/opt/My App,1/50%"), "file:///opt/My%20App%2C1/50%25");
    EXPECT_EQ(FileManagerReveal::fileUri("/home/z\xC3\xBC/a#b?c"), "file:///home/z%C3%BC/a%23b%3Fc");
    EXPECT_EQ(FileManagerReveal::fileUri("/x/\xFF"), "file:///x/%FF"); // a path need not be UTF-8
}

TEST(FileManagerRevealTest, DeletedSuffixAndParentFolder)
{
    EXPECT_EQ(FileManagerReveal::withoutDeletedSuffix("/usr/bin/vim (deleted)"), "/usr/bin/vim");
    EXPECT_EQ(FileManagerReveal::withoutDeletedSuffix("/usr/bin/vim"), "/usr/bin/vim");
    EXPECT_EQ(FileManagerReveal::parentFolder("/usr/bin/vim"), "/usr/bin");
    EXPECT_EQ(FileManagerReveal::parentFolder("/init"), "/");
    EXPECT_EQ(FileManagerReveal::parentFolder("relative"), "/");
}

TEST(LinuxProcessActionsTest, OpenFileLocationAsksTheFileManagerToShowTheExecutable)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "As root for another user, ShowItems isn't tried and xdg-open runs as them";
    }
    const TestSupport::ScopedTempDir dir("tasksmack_open_location");
    LinuxProcessActions actions = fileLocationActions(dir.path, true);
    EXPECT_TRUE(actions.actionCapabilities().canOpenFileLocation);

    const auto result = actions.openFileLocation(ownTarget());

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(result.successNote.empty());
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe");
    ASSERT_EQ(showItems().uris.size(), 1U);
    EXPECT_EQ(showItems().uris.front(), FileManagerReveal::fileUri(self.string()));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "argv.txt")); // ShowItems answered: no xdg-open
}

TEST(LinuxProcessActionsTest, OpenFileLocationOpensTheFolderWhenNoFileManagerAnswers)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "As root for another user, ShowItems isn't tried and xdg-open runs as them";
    }
    const TestSupport::ScopedTempDir dir("tasksmack_open_folder");
    LinuxProcessActions actions = fileLocationActions(dir.path, false);

    const auto result = actions.openFileLocation(ownTarget());

    ASSERT_TRUE(result.success) << result.errorMessage;
    const std::vector<std::string> argv = recordedArgv(dir.path);
    ASSERT_EQ(argv.size(), 1U);
    EXPECT_EQ(argv.front(), std::filesystem::read_symlink("/proc/self/exe").parent_path().string());
}

TEST(LinuxProcessActionsTest, OpenFileLocationOfADeletedExecutableOpensItsOldFolderWithANote)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "As root for another user, ShowItems isn't tried and xdg-open runs as them";
    }
    const TestSupport::ScopedTempDir dir("tasksmack_open_deleted");
    LinuxProcessSyscalls syscalls = realProcessSyscalls();
    syscalls.executableLink = &deletedExecutableLink;
    LinuxProcessActions actions = fileLocationActions(dir.path, true, syscalls);

    const auto result = actions.openFileLocation(ownTarget());

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(result.successNote.empty());
    ASSERT_EQ(showItems().uris.size(), 1U);
    EXPECT_EQ(showItems().uris.front(), "file:///opt/old%20app/bin/tool");
}

TEST(LinuxProcessActionsTest, OpenFileLocationSaysWhyTheExecutableCantBeRead)
{
    const TestSupport::ScopedTempDir dir("tasksmack_open_unreadable");
    LinuxProcessSyscalls syscalls = realProcessSyscalls();
    syscalls.executableLink = &failingExecutableLink;
    LinuxProcessActions actions = fileLocationActions(dir.path, true, syscalls);

    linkError() = ENOENT;
    auto result = actions.openFileLocation(ownTarget());
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("kernel thread"), std::string::npos) << result.errorMessage;

    linkError() = EACCES;
    result = actions.openFileLocation(ownTarget());
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied); // nothing the helper could do better
    EXPECT_NE(result.errorMessage.find("another user"), std::string::npos) << result.errorMessage;
    EXPECT_TRUE(showItems().uris.empty());
}

TEST(LinuxProcessActionsTest, OpenFileLocationRefusesAnInvalidOrUnconfirmedTarget)
{
    const TestSupport::ScopedTempDir dir("tasksmack_open_refuse");
    LinuxProcessActions actions = fileLocationActions(dir.path, true);
    ProcessTarget reused = ownTarget();
    reused.startTimeTicks += 1; // the PID now names another process

    EXPECT_FALSE(actions.openFileLocation({.pid = 0, .startTimeTicks = 1}).success);
    EXPECT_FALSE(actions.openFileLocation(reused).success);
    EXPECT_TRUE(showItems().uris.empty());
    EXPECT_FALSE(std::filesystem::exists(dir.path / "argv.txt"));
}

TEST(LinuxProcessActionsTest, ProcessControlAloneDoesNotOpenFileLocations)
{
    LinuxProcessControl control;
    EXPECT_FALSE(control.actionCapabilities().canOpenFileLocation);
    EXPECT_FALSE(control.openFileLocation(ownTarget()).success);
}

} // namespace
} // namespace Platform
