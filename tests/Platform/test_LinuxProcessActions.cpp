/// @file test_LinuxProcessActions.cpp
/// @brief Integration tests for Platform::LinuxProcessActions
///
/// These tests verify the capabilities reporting and error handling
/// of process actions. We avoid actually terminating processes to keep
/// tests safe and non-destructive.

#include "Platform/IProcessActions.h"
#include "Platform/Linux/LinuxProcessActions.h"
#include "Platform/Linux/ProcStatStartTime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <pthread.h>

// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides kill(), csignal does not
#include <signal.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Platform
{
namespace
{

/// This test process as an action target, with the start time read the same way the probe reads it.
[[nodiscard]] ProcessTarget ownTarget()
{
    std::ifstream stat("/proc/self/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    return {.pid = static_cast<std::int32_t>(getpid()), .startTimeTicks = ProcStat::parseStartTime(line).value_or(0)};
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
        return {.pid = static_cast<std::int32_t>(m_Pid), .startTimeTicks = ProcStat::parseStartTime(line).value_or(0)};
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
    EXPECT_NO_THROW({ LinuxProcessActions actions; });
}

TEST(LinuxProcessActionsTest, CapabilitiesReportedCorrectly)
{
    LinuxProcessActions actions;
    auto caps = actions.actionCapabilities();

    // Linux should support all standard process actions
    EXPECT_TRUE(caps.canTerminate);
    EXPECT_TRUE(caps.canKill);
    EXPECT_TRUE(caps.canStop);
    EXPECT_TRUE(caps.canContinue);    // resume is called canContinue in the interface
    EXPECT_TRUE(caps.canSetPriority); // setpriority() is available on Linux
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST(LinuxProcessActionsTest, TerminateNonExistentProcess)
{
    LinuxProcessActions actions;

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    const auto result = actions.terminate({.pid = nonExistentPid, .startTimeTicks = 1});

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, KillNonExistentProcess)
{
    LinuxProcessActions actions;

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    const auto result = actions.kill({.pid = nonExistentPid, .startTimeTicks = 1});

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, StopNonExistentProcess)
{
    LinuxProcessActions actions;

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    const auto result = actions.stop({.pid = nonExistentPid, .startTimeTicks = 1});

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, ResumeNonExistentProcess)
{
    LinuxProcessActions actions;

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    const auto result = actions.resume({.pid = nonExistentPid, .startTimeTicks = 1});

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
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

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    const auto result = actions.setPriority({.pid = nonExistentPid, .startTimeTicks = 1}, 0);

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
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

TEST(LinuxProcessActionsTest, KillWithTheMatchingStartTimeEndsTheProcess)
{
    SleepingChild child;
    ASSERT_TRUE(child.started());

    LinuxProcessActions actions;
    const auto result = actions.kill(child.target());

    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(child.exitsSoon());
}

} // namespace
} // namespace Platform
