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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>

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
    EXPECT_TRUE(isCloseRequestWindow(42, 42, true, false, false));
    EXPECT_FALSE(isCloseRequestWindow(41, 42, true, false, false)) << "another process's window";
    EXPECT_FALSE(isCloseRequestWindow(42, 42, false, false, false)) << "an invisible helper window";
    EXPECT_FALSE(isCloseRequestWindow(42, 42, true, true, false)) << "an owned window: a dialog";
    EXPECT_FALSE(isCloseRequestWindow(42, 42, true, false, true)) << "an unowned tool window: a palette or helper";
}

TEST(CloseRequestWindowTest, EveryEligibleWindowAskedIsSuccess)
{
    EXPECT_TRUE(closeRequestFailure(42, 1, 0, 0).empty());
    EXPECT_TRUE(closeRequestFailure(42, 3, 0, 0).empty());
}

TEST(CloseRequestWindowTest, NoEligibleWindowIsAFailureThatPointsToKill)
{
    const std::string failure = closeRequestFailure(42, 0, 0, 0);
    EXPECT_NE(failure.find("42"), std::string::npos) << failure;
    EXPECT_NE(failure.find("no window"), std::string::npos) << failure;
    EXPECT_NE(failure.find("Kill"), std::string::npos) << failure;
}

// A window that exists but refused the request (UIPI, say) is not reported as "no window", and a
// partial refusal is not reported as success (#1237 review).
TEST(CloseRequestWindowTest, RefusedRequestsAreReportedAsRefusalsWithTheError)
{
    const std::string all = closeRequestFailure(42, 2, 2, 5);
    EXPECT_EQ(all.find("no window"), std::string::npos) << all;
    EXPECT_NE(all.find("error 5"), std::string::npos) << all;
    EXPECT_NE(all.find("Kill"), std::string::npos) << all;

    const std::string some = closeRequestFailure(42, 3, 1, 5);
    EXPECT_FALSE(some.empty());
    EXPECT_NE(some.find("2 of 3"), std::string::npos) << some;
    EXPECT_NE(some.find("error 5"), std::string::npos) << some;
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

namespace
{

/// A visible, unowned top-level application window (not a tool window) owned by this test process,
/// pumped on its own thread, that records WM_CLOSE and ignores it -- as an application that asks
/// "save changes?" first would. It sits off-screen and never activates.
class CloseRecordingWindow
{
  public:
    CloseRecordingWindow()
        : m_Thread(
              [this]
              {
                  auto* const instance = GetModuleHandleW(nullptr);
                  WNDCLASSW windowClass{};
                  windowClass.lpfnWndProc = &CloseRecordingWindow::windowProc;
                  windowClass.hInstance = instance;
                  windowClass.lpszClassName = L"TaskSmackTerminateTestWindow";
                  RegisterClassW(&windowClass);
                  m_Window = CreateWindowExW(WS_EX_NOACTIVATE,
                                             windowClass.lpszClassName,
                                             L"Terminate test",
                                             WS_POPUP,
                                             -32000,
                                             -32000,
                                             1,
                                             1,
                                             nullptr,
                                             nullptr,
                                             instance,
                                             nullptr);
                  if (m_Window != nullptr)
                  {
                      SetWindowLongPtrW(m_Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&m_ClosesReceived));
                      ShowWindow(m_Window, SW_SHOWNOACTIVATE);
                  }
                  m_Ready.store(true);
                  MSG message{};
                  while (GetMessageW(&message, nullptr, 0, 0) > 0)
                  {
                      DispatchMessageW(&message);
                  }
                  if (m_Window != nullptr)
                  {
                      DestroyWindow(m_Window);
                  }
                  UnregisterClassW(windowClass.lpszClassName, instance);
              })
    {
        while (!m_Ready.load())
        {
            std::this_thread::yield();
        }
    }
    CloseRecordingWindow(const CloseRecordingWindow&) = delete;
    CloseRecordingWindow& operator=(const CloseRecordingWindow&) = delete;
    CloseRecordingWindow(CloseRecordingWindow&&) = delete;
    CloseRecordingWindow& operator=(CloseRecordingWindow&&) = delete;
    ~CloseRecordingWindow()
    {
        PostThreadMessageW(GetThreadId(m_Thread.native_handle()), WM_QUIT, 0, 0);
        m_Thread.join();
    }

    [[nodiscard]] bool created() const
    {
        return m_Window != nullptr;
    }
    /// Waits up to two seconds for a WM_CLOSE to arrive.
    [[nodiscard]] bool receivesClose() const
    {
        for (int i = 0; i < 200 && m_ClosesReceived.load() == 0; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return m_ClosesReceived.load() > 0;
    }

  private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_CLOSE)
        {
            auto* closes =
                reinterpret_cast<std::atomic<int>*>(GetWindowLongPtrW(window, GWLP_USERDATA)); // NOLINT(performance-no-int-to-ptr)
            if (closes != nullptr)
            {
                closes->fetch_add(1);
            }
            return 0; // Ignore it: the application decides whether to exit.
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    std::atomic<int> m_ClosesReceived{0};
    std::atomic<bool> m_Ready{false};
    HWND m_Window = nullptr;
    std::thread m_Thread;
};

[[nodiscard]] ProcessTarget thisProcess()
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime);
    return {
        .pid = static_cast<int32_t>(GetCurrentProcessId()),
        .startTimeTicks = (static_cast<uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
    };
}

} // namespace

// #1094 / #1237 review: the success path. Terminate finds this process's top-level window, the close
// request reaches it, and the process is asked, not ended: the test is still running to check.
TEST(WindowsProcessActionsTest, TerminatePostsWmCloseToTheProcessWindowAndDoesNotEndIt)
{
    const CloseRecordingWindow window;
    ASSERT_TRUE(window.created());

    WindowsProcessActions actions;
    const auto result = actions.terminate(thisProcess());

    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(window.receivesClose());
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
