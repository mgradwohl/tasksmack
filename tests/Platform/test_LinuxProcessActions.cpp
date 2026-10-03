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

// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides kill(), csignal does not
#include <pthread.h>
#include <signal.h>
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

// Note: This test may modify the test process's priority and cannot reliably
// restore the original value without root privileges. This is acceptable since
// test processes typically run at default nice=0 and are short-lived.
TEST(LinuxProcessActionsTest, SetPriorityOwnProcess)
{
    LinuxProcessActions actions;
    auto ownPid = static_cast<int32_t>(getpid());

    // Lowering priority (raising nice value) should work without root
    const auto result = actions.setPriority(ownTarget(), 10);

    // This may succeed or fail depending on current priority
    // If we're already at a high nice value, this should succeed
    // If we're at a lower nice value, we might need root to go back down

    // Only attempt cleanup if the initial operation succeeded
    if (result.success)
    {
        // Attempt to reset to 0 (may fail without privileges - see note above)
        const auto resetResult = actions.setPriority(ownTarget(), 0);
        if (!resetResult.success)
        {
            // Log warning but don't fail - lowering nice requires privileges
            GTEST_LOG_(WARNING) << "Test cleanup: Failed to reset priority for PID " << ownPid << ": " << resetResult.errorMessage;
        }
    }
    else
    {
        // At minimum, the error message should be informative if it fails
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }
}

TEST(LinuxProcessActionsTest, SetPriorityClampsBoundaryValues)
{
    LinuxProcessActions actions;
    auto ownPid = static_cast<int32_t>(getpid());

    // Test extreme values - they should be clamped internally
    // These may fail due to permissions, but shouldn't crash
    const auto result1 = actions.setPriority(ownTarget(), -100); // Way below -20
    const auto result2 = actions.setPriority(ownTarget(), 100);  // Way above 19

    // Either succeeds or has an error message, but no crash
    if (!result1.success)
    {
        EXPECT_GT(result1.errorMessage.size(), 0ULL);
    }
    if (!result2.success)
    {
        EXPECT_GT(result2.errorMessage.size(), 0ULL);
    }

    // Cleanup: attempt to reset priority to 0 to avoid affecting subsequent tests.
    // Note: This may fail without root privileges (see SetPriorityOwnProcess note).
    if (result2.success)
    {
        const auto resetResult = actions.setPriority(ownTarget(), 0);
        if (!resetResult.success)
        {
            GTEST_LOG_(WARNING) << "Test cleanup: Failed to reset priority for PID " << ownPid << ": " << resetResult.errorMessage;
        }
    }
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

/// A forked child that sleeps until signalled, killed and reaped on scope exit if still running.
class SleepingChild
{
  public:
    /// `extraThreads` threads besides the main one, all sleeping.
    explicit SleepingChild(int extraThreads = 0) : m_Pid(fork())
    {
        if (m_Pid == 0)
        {
            for (int i = 0; i < extraThreads; ++i)
            {
                pthread_t thread{};
                pthread_create(
                    &thread,
                    nullptr,
                    [](void*) -> void*
                    {
                        for (;;)
                        {
                            pause();
                        }
                    },
                    nullptr);
            }
            for (;;)
            {
                pause();
            }
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
};

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
    ASSERT_NE(raised, before) << "child already at the lowest priority";

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

/// The thread IDs in /proc/<pid>/task, once there are `expected` of them (empty on timeout).
std::vector<id_t> waitForThreads(pid_t pid, std::size_t expected)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        std::vector<id_t> tids;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(std::format("/proc/{}/task", pid), ec))
        {
            tids.push_back(static_cast<id_t>(std::stoul(entry.path().filename().string())));
        }
        if (tids.size() >= expected)
        {
            return tids;
        }
        usleep(10'000);
    }
    return {};
}

TEST(LinuxProcessActionsTest, SetPriorityChangesEveryThread)
{
    // Nice is per thread on Linux: setpriority(PRIO_PROCESS, pid) alone changes only the main
    // thread, leaving a multithreaded process's workers at the old priority (#1104).
    const SleepingChild child(2);
    ASSERT_TRUE(child.started());
    const ProcessTarget target = child.target();
    const std::vector<id_t> tids = waitForThreads(target.pid, 3);
    ASSERT_EQ(tids.size(), 3U);

    errno = 0;
    const int before = getpriority(PRIO_PROCESS, static_cast<id_t>(target.pid));
    ASSERT_EQ(errno, 0);
    const int raised = std::min(before + 5, 19);
    ASSERT_NE(raised, before) << "child already at the lowest priority";

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
