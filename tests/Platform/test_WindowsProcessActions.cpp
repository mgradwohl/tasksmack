/// @file test_WindowsProcessActions.cpp
/// @brief Integration tests for Platform::WindowsProcessActions
///
/// These tests verify the capabilities reporting and error handling
/// of process actions. We avoid actually terminating processes to keep
/// tests safe and non-destructive.

#include "Platform/IProcessActions.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <cstdint>
#include <limits>
#include <string>

namespace Platform
{

// =============================================================================
// niceToPriorityClass: pure mapping from Unix nice value to Windows priority class,
// no process required. Covers all five threshold branches plus their boundaries.
// =============================================================================

TEST(NiceToPriorityClassTest, BelowHighThresholdMapsToHigh)
{
    EXPECT_EQ(niceToPriorityClass(-20), static_cast<uint32_t>(HIGH_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(-11), static_cast<uint32_t>(HIGH_PRIORITY_CLASS));
}

TEST(NiceToPriorityClassTest, AtHighThresholdMapsToAboveNormal)
{
    // HIGH_THRESHOLD = -10 is the first value no longer < HIGH_THRESHOLD.
    EXPECT_EQ(niceToPriorityClass(-10), static_cast<uint32_t>(ABOVE_NORMAL_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(-6), static_cast<uint32_t>(ABOVE_NORMAL_PRIORITY_CLASS));
}

TEST(NiceToPriorityClassTest, AtAboveNormalThresholdMapsToNormal)
{
    // ABOVE_NORMAL_THRESHOLD = -5.
    EXPECT_EQ(niceToPriorityClass(-5), static_cast<uint32_t>(NORMAL_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(0), static_cast<uint32_t>(NORMAL_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(4), static_cast<uint32_t>(NORMAL_PRIORITY_CLASS));
}

TEST(NiceToPriorityClassTest, AtBelowNormalThresholdMapsToBelowNormal)
{
    // BELOW_NORMAL_THRESHOLD = 5.
    EXPECT_EQ(niceToPriorityClass(5), static_cast<uint32_t>(BELOW_NORMAL_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(14), static_cast<uint32_t>(BELOW_NORMAL_PRIORITY_CLASS));
}

TEST(NiceToPriorityClassTest, AtIdleThresholdAndAboveMapsToIdle)
{
    // IDLE_THRESHOLD = 15.
    EXPECT_EQ(niceToPriorityClass(15), static_cast<uint32_t>(IDLE_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(19), static_cast<uint32_t>(IDLE_PRIORITY_CLASS));
}

// =============================================================================
// Terminate's close request (#1094)
// =============================================================================

TEST(CloseRequestWindowTest, OnlyTheTargetsVisibleUnownedWindowsAreAsked)
{
    EXPECT_TRUE(isCloseRequestWindow(42, 42, true, false));
    EXPECT_FALSE(isCloseRequestWindow(41, 42, true, false)) << "another process's window";
    EXPECT_FALSE(isCloseRequestWindow(42, 42, false, false)) << "an invisible helper window";
    EXPECT_FALSE(isCloseRequestWindow(42, 42, true, true)) << "an owned window: a dialog or tool window";
}

TEST(CloseRequestWindowTest, NoWindowAskedIsAFailureThatPointsToKill)
{
    EXPECT_TRUE(closeRequestFailure(42, 1).empty());
    EXPECT_TRUE(closeRequestFailure(42, 3).empty());
    const std::string failure = closeRequestFailure(42, 0);
    EXPECT_NE(failure.find("42"), std::string::npos) << failure;
    EXPECT_NE(failure.find("no window"), std::string::npos) << failure;
    EXPECT_NE(failure.find("Kill"), std::string::npos) << failure;
}

TEST(WindowsProcessActionsTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ WindowsProcessActions actions; });
}

TEST(WindowsProcessActionsTest, CapabilitiesReportedCorrectly)
{
    WindowsProcessActions actions;
    const auto caps = actions.actionCapabilities();

    EXPECT_TRUE(caps.canTerminate);
    EXPECT_TRUE(caps.canKill);
    EXPECT_FALSE(caps.canStop);
    EXPECT_FALSE(caps.canContinue);
}

TEST(WindowsProcessActionsTest, StopNotSupported)
{
    WindowsProcessActions actions;

    const auto result = actions.stop({.pid = 1, .startTimeTicks = 1});
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(WindowsProcessActionsTest, ResumeNotSupported)
{
    WindowsProcessActions actions;

    const auto result = actions.resume({.pid = 1, .startTimeTicks = 1});
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(WindowsProcessActionsTest, SetPriorityRejectsInvalidPid)
{
    WindowsProcessActions actions;

    // Test PID 0 (boundary)
    auto result = actions.setPriority({.pid = 0, .startTimeTicks = 1}, 0);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Invalid PID");

    // Test negative PID (covers full range of `pid <= 0` check)
    result = actions.setPriority({.pid = -1, .startTimeTicks = 1}, 0);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Invalid PID");
}

TEST(WindowsProcessActionsTest, TerminateNonExistentProcess)
{
    WindowsProcessActions actions;

    const int32_t nonExistentPid = std::numeric_limits<int32_t>::max();
    const auto result = actions.terminate({.pid = nonExistentPid, .startTimeTicks = 1});

    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(WindowsProcessActionsTest, KillNonExistentProcess)
{
    WindowsProcessActions actions;

    const int32_t nonExistentPid = std::numeric_limits<int32_t>::max();
    const auto result = actions.kill({.pid = nonExistentPid, .startTimeTicks = 1});

    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

// =============================================================================
// Identity (#973), against a real child process: started suspended so it does nothing, and
// terminated by the test either way.
// =============================================================================

namespace
{

/// A suspended child process that is terminated on scope exit if the test has not already done so.
class SuspendedChild
{
  public:
    SuspendedChild()
    {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        std::wstring commandLine = L"cmd.exe /c exit 0";
        if (CreateProcessW(nullptr,
                           commandLine.data(),
                           nullptr,
                           nullptr,
                           FALSE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW,
                           nullptr,
                           nullptr,
                           &startup,
                           &m_Info) == 0)
        {
            m_Info = {};
        }
    }
    SuspendedChild(const SuspendedChild&) = delete;
    SuspendedChild& operator=(const SuspendedChild&) = delete;
    SuspendedChild(SuspendedChild&&) = delete;
    SuspendedChild& operator=(SuspendedChild&&) = delete;
    ~SuspendedChild()
    {
        if (m_Info.hProcess != nullptr)
        {
            TerminateProcess(m_Info.hProcess, 0);
            CloseHandle(m_Info.hThread);
            CloseHandle(m_Info.hProcess);
        }
    }

    [[nodiscard]] bool started() const
    {
        return m_Info.hProcess != nullptr;
    }
    [[nodiscard]] bool alive() const
    {
        return WaitForSingleObject(m_Info.hProcess, 0) == WAIT_TIMEOUT;
    }
    [[nodiscard]] bool exitsSoon() const
    {
        return WaitForSingleObject(m_Info.hProcess, 5000) == WAIT_OBJECT_0;
    }
    [[nodiscard]] ProcessTarget target() const
    {
        FILETIME creation{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        GetProcessTimes(m_Info.hProcess, &creation, &exitTime, &kernelTime, &userTime);
        return {
            .pid = static_cast<int32_t>(m_Info.dwProcessId),
            .startTimeTicks = (static_cast<uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
        };
    }

  private:
    PROCESS_INFORMATION m_Info{};
};

} // namespace

TEST(WindowsProcessActionsTest, KillWithADifferentStartTimeLeavesTheProcessRunning)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.started());
    ProcessTarget reused = child.target();
    reused.startTimeTicks += 1; // Same PID, a different process as far as the action can tell.

    WindowsProcessActions actions;
    const auto result = actions.kill(reused);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("different process"), std::string::npos) << result.errorMessage;
    EXPECT_TRUE(child.alive());
}

// #1094: Terminate is a request, not Kill. A process with no window to close (this child is started
// with CREATE_NO_WINDOW) cannot be asked, so Terminate reports that and leaves it running, where it
// used to TerminateProcess it exactly as Kill does.
TEST(WindowsProcessActionsTest, TerminateAsksRatherThanKillsAWindowlessProcess)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.started());

    WindowsProcessActions actions;
    const auto result = actions.terminate(child.target());

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("no window"), std::string::npos) << result.errorMessage;
    EXPECT_TRUE(child.alive());

    const auto killed = actions.kill(child.target());
    EXPECT_TRUE(killed.success) << killed.errorMessage;
    EXPECT_TRUE(child.exitsSoon());
}

TEST(WindowsProcessActionsTest, TerminateWithADifferentStartTimeLeavesTheProcessRunning)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.started());
    ProcessTarget reused = child.target();
    reused.startTimeTicks += 1;

    WindowsProcessActions actions;
    const auto result = actions.terminate(reused);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("different process"), std::string::npos) << result.errorMessage;
    EXPECT_TRUE(child.alive());
}

TEST(WindowsProcessActionsTest, KillWithTheMatchingStartTimeEndsTheProcess)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.started());

    WindowsProcessActions actions;
    const auto result = actions.kill(child.target());

    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(child.exitsSoon());
}

} // namespace Platform
