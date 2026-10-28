/// @file test_WindowsProcessActions.cpp
/// @brief Integration tests for Platform::WindowsProcessActions
///
/// These tests verify the capabilities reporting and error handling
/// of process actions. We avoid actually terminating processes to keep
/// tests safe and non-destructive. Efficiency mode (#3071) is tested through faked calls,
/// and for real only on a child process the test starts itself.

#include "Domain/PriorityConfig.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/ProcessTypes.h"
#include "Platform/Windows/WindowsAffinityMath.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"
#include "Platform/Windows/WindowsProcessDump.h"
#include "Platform/Windows/WindowsProcessDumpMath.h"
#include "Platform/Windows/WindowsProcessMachine.h"

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

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

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
// priorityClassToNice: the nice value the probe reports for each class (#1204)
// =============================================================================

TEST(PriorityClassToNiceTest, EverySettableClassRoundTrips)
{
    // Reading a class and writing the reported value back must set the same class.
    for (const uint32_t priorityClass : {
             static_cast<uint32_t>(IDLE_PRIORITY_CLASS),
             static_cast<uint32_t>(BELOW_NORMAL_PRIORITY_CLASS),
             static_cast<uint32_t>(NORMAL_PRIORITY_CLASS),
             static_cast<uint32_t>(ABOVE_NORMAL_PRIORITY_CLASS),
             static_cast<uint32_t>(HIGH_PRIORITY_CLASS),
         })
    {
        EXPECT_EQ(niceToPriorityClass(priorityClassToNice(priorityClass)), priorityClass) << "class 0x" << std::hex << priorityClass;
    }
}

TEST(PriorityClassToNiceTest, EveryClassIsLabelledAsItself)
{
    // The regression: Above normal reported -5 and High -10, the thresholds where the next class
    // down starts, so they were labelled "Normal" and "Above Normal".
    EXPECT_EQ(Domain::Priority::getPriorityLabel(priorityClassToNice(IDLE_PRIORITY_CLASS)), "Idle");
    EXPECT_EQ(Domain::Priority::getPriorityLabel(priorityClassToNice(BELOW_NORMAL_PRIORITY_CLASS)), "Below Normal");
    EXPECT_EQ(Domain::Priority::getPriorityLabel(priorityClassToNice(NORMAL_PRIORITY_CLASS)), "Normal");
    EXPECT_EQ(Domain::Priority::getPriorityLabel(priorityClassToNice(ABOVE_NORMAL_PRIORITY_CLASS)), "Above Normal");
    EXPECT_EQ(Domain::Priority::getPriorityLabel(priorityClassToNice(HIGH_PRIORITY_CLASS)), "High");
}

TEST(PriorityClassToNiceTest, RealtimeIsDistinctFromHighButNeverSetAsRealtime)
{
    // Realtime keeps MIN_NICE so the UI can name it; writing that back sets High, never Realtime.
    EXPECT_EQ(priorityClassToNice(REALTIME_PRIORITY_CLASS), Domain::Priority::MIN_NICE);
    EXPECT_NE(priorityClassToNice(REALTIME_PRIORITY_CLASS), priorityClassToNice(HIGH_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(priorityClassToNice(REALTIME_PRIORITY_CLASS)), static_cast<uint32_t>(HIGH_PRIORITY_CLASS));
}

TEST(PriorityClassToNiceTest, AnUnknownClassIsNormal)
{
    EXPECT_EQ(priorityClassToNice(0), Domain::Priority::NORMAL_NICE); // GetPriorityClass() failed
    EXPECT_EQ(priorityClassToNice(0x12345U), Domain::Priority::NORMAL_NICE);
}

static_assert(priorityClassToNice(HIGH_PRIORITY_CLASS) < Domain::Priority::HIGH_THRESHOLD);

// The probe's nice values and Process Details' are one table, Domain::Priority::classNice() (#2168):
// retuning it for one side retunes it for the other.
static_assert(priorityClassToNice(IDLE_PRIORITY_CLASS) == Domain::Priority::classNice(Domain::Priority::PriorityClass::Idle));
static_assert(priorityClassToNice(BELOW_NORMAL_PRIORITY_CLASS) ==
              Domain::Priority::classNice(Domain::Priority::PriorityClass::BelowNormal));
static_assert(priorityClassToNice(NORMAL_PRIORITY_CLASS) == Domain::Priority::classNice(Domain::Priority::PriorityClass::Normal));
static_assert(priorityClassToNice(ABOVE_NORMAL_PRIORITY_CLASS) ==
              Domain::Priority::classNice(Domain::Priority::PriorityClass::AboveNormal));
static_assert(priorityClassToNice(HIGH_PRIORITY_CLASS) == Domain::Priority::classNice(Domain::Priority::PriorityClass::High));
static_assert(priorityClassToNice(REALTIME_PRIORITY_CLASS) == Domain::Priority::classNice(Domain::Priority::PriorityClass::Realtime));
// And each value reads back as its own class, Realtime included.
static_assert(Domain::Priority::classFromNice(priorityClassToNice(IDLE_PRIORITY_CLASS)) == Domain::Priority::PriorityClass::Idle);
static_assert(Domain::Priority::classFromNice(priorityClassToNice(BELOW_NORMAL_PRIORITY_CLASS)) ==
              Domain::Priority::PriorityClass::BelowNormal);
static_assert(Domain::Priority::classFromNice(priorityClassToNice(NORMAL_PRIORITY_CLASS)) == Domain::Priority::PriorityClass::Normal);
static_assert(Domain::Priority::classFromNice(priorityClassToNice(ABOVE_NORMAL_PRIORITY_CLASS)) ==
              Domain::Priority::PriorityClass::AboveNormal);
static_assert(Domain::Priority::classFromNice(priorityClassToNice(HIGH_PRIORITY_CLASS)) == Domain::Priority::PriorityClass::High);
static_assert(Domain::Priority::classFromNice(priorityClassToNice(REALTIME_PRIORITY_CLASS)) == Domain::Priority::PriorityClass::Realtime);

// =============================================================================
// toPriorityClass: the class the probe reports beside nice (#1280)
// =============================================================================

TEST(ToPriorityClassTest, EveryWin32ClassIsNamed)
{
    EXPECT_EQ(toPriorityClass(IDLE_PRIORITY_CLASS), PriorityClass::Idle);
    EXPECT_EQ(toPriorityClass(BELOW_NORMAL_PRIORITY_CLASS), PriorityClass::BelowNormal);
    EXPECT_EQ(toPriorityClass(NORMAL_PRIORITY_CLASS), PriorityClass::Normal);
    EXPECT_EQ(toPriorityClass(ABOVE_NORMAL_PRIORITY_CLASS), PriorityClass::AboveNormal);
    EXPECT_EQ(toPriorityClass(HIGH_PRIORITY_CLASS), PriorityClass::High);
    EXPECT_EQ(toPriorityClass(REALTIME_PRIORITY_CLASS), PriorityClass::Realtime);
}

TEST(ToPriorityClassTest, AnUnknownClassIsNone)
{
    EXPECT_EQ(toPriorityClass(0), PriorityClass::None); // GetPriorityClass() failed
    EXPECT_EQ(toPriorityClass(0x12345U), PriorityClass::None);
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
    EXPECT_NO_THROW({ const WindowsProcessActions actions; });
}

TEST(WindowsProcessActionsTest, CapabilitiesReportedCorrectly)
{
    const WindowsProcessActions actions;
    const auto caps = actions.actionCapabilities();

    EXPECT_TRUE(caps.canTerminate);
    EXPECT_TRUE(caps.canKill);
    EXPECT_TRUE(caps.canStop);       // NtSuspendProcess (#2582)
    EXPECT_TRUE(caps.canContinue);   // NtResumeProcess (#2582)
    EXPECT_TRUE(caps.canCreateDump); // MiniDumpWriteDump (#807)
}

TEST(WindowsProcessActionsTest, SuspendingThisProcessIsRefusedBeforeAnythingIsSent)
{
    WindowsProcessActions actions;

    // Refused by PID, whatever the start time: a suspended TaskSmack (here, this test) would hang.
    const auto result = actions.stop({.pid = static_cast<int32_t>(GetCurrentProcessId()), .startTimeTicks = 1});
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied);
    EXPECT_NE(result.errorMessage.find("TaskSmack itself"), std::string::npos) << result.errorMessage;
}

TEST(WindowsProcessActionsTest, SuspendAndResumeOfANonExistentProcessFail)
{
    WindowsProcessActions actions;

    EXPECT_FALSE(actions.stop({.pid = 999999, .startTimeTicks = 1}).success);
    EXPECT_FALSE(actions.resume({.pid = 999999, .startTimeTicks = 1}).success);
    EXPECT_EQ(actions.stop({.pid = 0, .startTimeTicks = 1}).errorMessage, "Invalid PID");
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
    /// Its main thread's suspend count: 1 as created, more for each suspend not yet resumed. Read by
    /// suspending the thread once more and resuming it, which returns the count it had.
    [[nodiscard]] DWORD suspendCount() const
    {
        const DWORD previous = SuspendThread(m_Info.hThread);
        ResumeThread(m_Info.hThread);
        return previous;
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

// #1566, #2206: the child has exited, but the test's handle keeps its process object (and so its
// PID and start time) alive: the identity check passes, so the exit must be noticed by the open
// itself and reported as an exit, with nothing sent, rather than as a refused TerminateProcess.
TEST(WindowsProcessActionsTest, KillingAnExitedProcessIsReportedAsAFailure)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.started());
    const ProcessTarget target = child.target();

    WindowsProcessActions actions;
    ASSERT_TRUE(actions.kill(target).success);
    ASSERT_TRUE(child.exitsSoon());

    const auto again = actions.kill(target);
    EXPECT_FALSE(again.success);
    EXPECT_EQ(again.errorMessage, std::format("Process {} has exited; action not sent", target.pid));
}

// Suspend and Resume (#2582) on the test's own suspended child, never on any other process: Windows
// counts suspensions per thread, so each suspend adds one to the main thread's count and each resume
// takes one away.
TEST(WindowsProcessActionsTest, SuspendThenResumeChangesTheChildsSuspendCount)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.started());
    ASSERT_EQ(child.suspendCount(), 1U) << "created suspended";

    WindowsProcessActions actions;
    const auto suspended = actions.stop(child.target());
    EXPECT_TRUE(suspended.success) << suspended.errorMessage;
    EXPECT_EQ(child.suspendCount(), 2U);

    const auto resumed = actions.resume(child.target());
    EXPECT_TRUE(resumed.success) << resumed.errorMessage;
    EXPECT_EQ(child.suspendCount(), 1U);
    EXPECT_TRUE(child.alive());
}

TEST(WindowsProcessActionsTest, SuspendOrResumeWithADifferentStartTimeLeavesTheChildAlone)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.started());
    ProcessTarget reused = child.target();
    reused.startTimeTicks += 1;

    WindowsProcessActions actions;
    const auto suspended = actions.stop(reused);
    EXPECT_FALSE(suspended.success);
    EXPECT_NE(suspended.errorMessage.find("different process"), std::string::npos) << suspended.errorMessage;
    const auto resumed = actions.resume(reused);
    EXPECT_FALSE(resumed.success);
    EXPECT_EQ(child.suspendCount(), 1U) << "nothing reached the child";
}

TEST(WindowsProcessActionsTest, SetPriorityChangesTheClassOfAMatchingProcess)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.started());

    WindowsProcessActions actions;
    const auto result = actions.setPriority(child.target(), priorityClassToNice(BELOW_NORMAL_PRIORITY_CLASS));
    EXPECT_TRUE(result.success) << result.errorMessage;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(child.target().pid));
    ASSERT_NE(process, nullptr);
    EXPECT_EQ(GetPriorityClass(process), static_cast<DWORD>(BELOW_NORMAL_PRIORITY_CLASS));
    CloseHandle(process);
}

TEST(WindowsProcessActionsTest, SetPriorityOfANonExistentProcessSaysItIsGone)
{
    WindowsProcessActions actions;
    const auto result = actions.setPriority({.pid = std::numeric_limits<int32_t>::max(), .startTimeTicks = 1}, 0);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Process not found - may have already exited");
}

TEST(WindowsProcessActionsTest, SetPriorityOfTheSystemProcessIsRefused)
{
    // PID 4 is the System process. Without elevation it can't be opened to set its priority; with
    // elevation it can, but its start time isn't 1, so the identity check refuses. Either way nothing
    // is sent to it.
    WindowsProcessActions actions;
    const auto result = actions.setPriority({.pid = 4, .startTimeTicks = 1}, 0);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}

TEST(WindowsProcessActionsTest, IoPriorityIsNotSupportedAndNothingIsSent)
{
    // #803: Windows has no ionice equivalent, so the capability is off and both calls refuse.
    WindowsProcessActions actions;
    EXPECT_FALSE(actions.actionCapabilities().canSetIoPriority);

    const auto set = actions.setIoPriority({.pid = 1234, .startTimeTicks = 1}, IoPriorityClass::Idle, 0);
    EXPECT_FALSE(set.success);
    EXPECT_NE(set.errorMessage.find("not supported on Windows"), std::string::npos) << set.errorMessage;
    EXPECT_NE(set.errorMessage.find("1234"), std::string::npos) << set.errorMessage;

    const auto get = actions.getIoPriority({.pid = 1234, .startTimeTicks = 1});
    ASSERT_FALSE(get.has_value());
    EXPECT_NE(get.error().find("not supported on Windows"), std::string::npos) << get.error();
}

// =============================================================================
// setAffinity (#1881): through faked Win32 calls, then once against a real suspended child
// =============================================================================

namespace
{

/// What the faked calls do and saw. Function pointers can't capture, so the fakes share this state;
/// each test starts from reset().
struct FakeWin
{
    static inline std::uint64_t creationTicks = 0; // What GetProcessTimes reports
    static inline DWORD openError = 0;             // OpenProcess fails with this, when not 0
    static inline DWORD setError = 0;              // The setter fails with this, when not 0
    static inline DWORD openedAccess = 0;
    static inline int maskCalls = 0;
    static inline DWORD_PTR lastMask = 0;
    static inline int cpuSetCalls = 0;
    static inline std::vector<ULONG> lastCpuSets;
    static inline std::vector<std::uint32_t> groupSizes; // reset(): one group of 8
    static inline std::vector<ProcessorCoreRecord> cores;
    static inline std::vector<CpuSetRecord> cpuSets;

    static void reset()
    {
        creationTicks = 1234;
        openError = 0;
        setError = 0;
        openedAccess = 0;
        maskCalls = 0;
        lastMask = 0;
        cpuSetCalls = 0;
        lastCpuSets.clear();
        groupSizes = {8};
        cores.clear();
        cpuSets.clear();
    }

    static HANDLE WINAPI openProcess(DWORD access, BOOL /*inherit*/, DWORD /*pid*/)
    {
        openedAccess = access;
        if (openError != 0)
        {
            SetLastError(openError);
            return nullptr;
        }
        // A real handle (to this test process), so the action's owner can close it.
        return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    }
    static BOOL WINAPI
    getProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
    {
        creation->dwLowDateTime = static_cast<DWORD>(creationTicks & 0xFFFFFFFFU);
        creation->dwHighDateTime = static_cast<DWORD>(creationTicks >> 32U);
        return TRUE;
    }
    static BOOL WINAPI setProcessAffinityMask(HANDLE /*process*/, DWORD_PTR mask)
    {
        ++maskCalls;
        lastMask = mask;
        if (setError != 0)
        {
            SetLastError(setError);
            return FALSE;
        }
        return TRUE;
    }
    static BOOL WINAPI setProcessDefaultCpuSets(HANDLE /*process*/, const ULONG* ids, ULONG count)
    {
        ++cpuSetCalls;
        lastCpuSets.assign(ids, ids + count); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the API's array
        if (setError != 0)
        {
            SetLastError(setError);
            return FALSE;
        }
        return TRUE;
    }
    static BOOL WINAPI isProcessCritical(HANDLE /*process*/, PBOOL critical)
    {
        *critical = FALSE; // the guard has its own tests (test_WindowsProcessActionsSeams.cpp, #2556)
        return TRUE;
    }

    [[nodiscard]] static WindowsProcessFunctions functions()
    {
        return {
            .openProcess = &openProcess,
            .getProcessTimes = &getProcessTimes,
            .setProcessAffinityMask = &setProcessAffinityMask,
            .setProcessDefaultCpuSets = &setProcessDefaultCpuSets,
            .readGroupSizes = [] { return groupSizes; },
            .readCores = [] { return cores; },
            .readCpuSets = [] { return cpuSets; },
            .pssCaptureSnapshot = nullptr,
            .isProcessCritical = &isProcessCritical,
            .getProcessInformation = nullptr,
        };
    }
};

constexpr ProcessTarget FAKE_TARGET{.pid = 4242, .startTimeTicks = 1234};

class WindowsSetAffinityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeWin::reset();
    }
};

} // namespace

TEST_F(WindowsSetAffinityTest, OneGroupSetsTheMaskThroughAVerifiedHandle)
{
    WindowsProcessActions actions(FakeWin::functions());
    EXPECT_TRUE(actions.actionCapabilities().canSetAffinity);

    const auto result = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1010U));
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(FakeWin::maskCalls, 1);
    EXPECT_EQ(FakeWin::lastMask, DWORD_PTR{0b1010U});
    EXPECT_EQ(FakeWin::cpuSetCalls, 0);
    EXPECT_NE(FakeWin::openedAccess & PROCESS_SET_INFORMATION, 0U);
    EXPECT_TRUE(result.successNote.empty()) << "a hard affinity mask is what Set affinity promises: " << result.successNote;
}

TEST_F(WindowsSetAffinityTest, AReusedPidIsRefusedAndNothingIsSet)
{
    FakeWin::creationTicks = 9999; // The PID now belongs to a process started later
    WindowsProcessActions actions(FakeWin::functions());
    const auto result = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1U));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("different process"), std::string::npos) << result.errorMessage;
    EXPECT_EQ(FakeWin::maskCalls, 0);

    const auto unknown = actions.setAffinity({.pid = 4242, .startTimeTicks = 0}, CpuAffinity::fromMask(0b1U));
    EXPECT_FALSE(unknown.success);
    EXPECT_NE(unknown.errorMessage.find("Cannot confirm"), std::string::npos) << unknown.errorMessage;
    EXPECT_EQ(FakeWin::maskCalls, 0);
}

TEST_F(WindowsSetAffinityTest, AccessDeniedSaysTheProcessMayBeProtectedOrElevated)
{
    FakeWin::openError = ERROR_ACCESS_DENIED;
    WindowsProcessActions actions(FakeWin::functions());
    const auto opened = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1U));
    EXPECT_FALSE(opened.success);
    EXPECT_NE(opened.errorMessage.find("Access denied"), std::string::npos) << opened.errorMessage;
    EXPECT_NE(opened.errorMessage.find("protected"), std::string::npos) << opened.errorMessage;
    EXPECT_EQ(FakeWin::maskCalls, 0);

    // Opened, but the set itself refused: the same message.
    FakeWin::openError = 0;
    FakeWin::setError = ERROR_ACCESS_DENIED;
    const auto set = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1U));
    EXPECT_FALSE(set.success);
    EXPECT_NE(set.errorMessage.find("Access denied"), std::string::npos) << set.errorMessage;
}

TEST_F(WindowsSetAffinityTest, AGoneProcessAndARefusedMaskAreReported)
{
    FakeWin::openError = ERROR_INVALID_PARAMETER;
    WindowsProcessActions actions(FakeWin::functions());
    EXPECT_EQ(actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1U)).errorMessage, "Process not found - may have already exited");

    FakeWin::openError = 0;
    FakeWin::setError = ERROR_INVALID_PARAMETER;
    const auto refused = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1U));
    EXPECT_FALSE(refused.success);
    EXPECT_NE(refused.errorMessage.find("processor group"), std::string::npos) << refused.errorMessage;

    FakeWin::setError = ERROR_GEN_FAILURE;
    const auto other = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1U));
    EXPECT_NE(other.errorMessage.find(std::to_string(ERROR_GEN_FAILURE)), std::string::npos) << other.errorMessage;
}

TEST_F(WindowsSetAffinityTest, AnEmptyChoiceOrACpuThatDoesNotExistIsRefusedBeforeOpening)
{
    WindowsProcessActions actions(FakeWin::functions());
    EXPECT_EQ(actions.setAffinity(FAKE_TARGET, CpuAffinity{}).errorMessage, "Choose at least one CPU");
    Platform::CpuAffinity beyond;
    beyond.set(8); // Group 0 has CPUs 0-7
    EXPECT_FALSE(actions.setAffinity(FAKE_TARGET, beyond).success);
    EXPECT_EQ(actions.setAffinity({.pid = 0, .startTimeTicks = 1}, CpuAffinity::fromMask(1U)).errorMessage, "Invalid PID");
    EXPECT_EQ(FakeWin::openedAccess, 0U) << "nothing opened";
}

TEST_F(WindowsSetAffinityTest, SeveralGroupsSetCpuSetsAndEveryCpuClearsThem)
{
    FakeWin::groupSizes = {2, 2}; // CPUs 0-1 in group 0, 2-3 in group 1
    FakeWin::cores = {{.group = 0, .mask = 0b11U, .efficiencyClass = 0}, {.group = 1, .mask = 0b11U, .efficiencyClass = 0}};
    FakeWin::cpuSets = {{.id = 256, .group = 0, .logicalProcessorIndex = 0},
                        {.id = 257, .group = 0, .logicalProcessorIndex = 1},
                        {.id = 258, .group = 1, .logicalProcessorIndex = 0},
                        {.id = 259, .group = 1, .logicalProcessorIndex = 1}};
    WindowsProcessActions actions(FakeWin::functions());
    EXPECT_TRUE(actions.affinityLayout().appliedAsCpuSets);

    const auto result = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b0110U)); // CPU 1 (group 0) and 2 (group 1)
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(FakeWin::lastCpuSets, (std::vector<ULONG>{257, 258}));
    EXPECT_EQ(FakeWin::maskCalls, 0);
    EXPECT_NE(FakeWin::openedAccess & PROCESS_SET_LIMITED_INFORMATION, 0U);

    EXPECT_TRUE(actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1111U)).success);
    EXPECT_TRUE(FakeWin::lastCpuSets.empty()) << "every CPU clears the default CPU sets";

    FakeWin::cpuSets.pop_back(); // CPU 3 went offline after the dialog opened
    const auto missing = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1000U));
    EXPECT_FALSE(missing.success);
    EXPECT_NE(missing.errorMessage.find("0 of the 1"), std::string::npos) << missing.errorMessage;
}

TEST_F(WindowsSetAffinityTest, SeveralGroupsSaySoftCpuSetsWereSetNotAHardMask)
{
    // The regression (#2173): CPU sets are a preference that threads with an affinity or CPU sets of
    // their own, and a hard affinity mask, override; success read as though a hard mask was set.
    FakeWin::groupSizes = {2, 2};
    FakeWin::cores = {{.group = 0, .mask = 0b11U, .efficiencyClass = 0}, {.group = 1, .mask = 0b11U, .efficiencyClass = 0}};
    FakeWin::cpuSets = {{.id = 256, .group = 0, .logicalProcessorIndex = 0},
                        {.id = 257, .group = 0, .logicalProcessorIndex = 1},
                        {.id = 258, .group = 1, .logicalProcessorIndex = 0},
                        {.id = 259, .group = 1, .logicalProcessorIndex = 1}};
    WindowsProcessActions actions(FakeWin::functions());

    const auto some = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b0011U));
    ASSERT_TRUE(some.success) << some.errorMessage;
    EXPECT_NE(some.successNote.find("preferred CPUs (CPU sets)"), std::string::npos) << some.successNote;
    EXPECT_NE(some.successNote.find("threads with their own affinity are not moved"), std::string::npos) << some.successNote;

    const auto every = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b1111U));
    ASSERT_TRUE(every.success) << every.errorMessage;
    EXPECT_NE(every.successNote.find("clearing its preferred CPUs"), std::string::npos) << every.successNote;
    EXPECT_NE(every.successNote.find("affinity mask"), std::string::npos) << "an earlier hard mask is not undone: " << every.successNote;

    FakeWin::setError = ERROR_ACCESS_DENIED;
    const auto refused = actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(0b0011U));
    EXPECT_FALSE(refused.success);
    EXPECT_TRUE(refused.successNote.empty()) << "a failure carries no success note";
}

TEST_F(WindowsSetAffinityTest, NoGroupSizesMeansNoAffinityAction)
{
    FakeWin::groupSizes.clear();
    WindowsProcessActions actions(FakeWin::functions());
    EXPECT_FALSE(actions.actionCapabilities().canSetAffinity);
    EXPECT_FALSE(actions.setAffinity(FAKE_TARGET, CpuAffinity::fromMask(1U)).success);
    EXPECT_EQ(FakeWin::openedAccess, 0U);
}

TEST(WindowsProcessActionsTest, SetAffinityPinsAMatchingChildAndRefusesAReusedPid)
{
    WindowsProcessActions actions;
    if (actions.affinityLayout().appliedAsCpuSets)
    {
        GTEST_SKIP() << "several processor groups: CPU sets don't show in GetProcessAffinityMask";
    }
    const SuspendedChild child;
    ASSERT_TRUE(child.started());

    ProcessTarget reused = child.target();
    reused.startTimeTicks += 1;
    EXPECT_FALSE(actions.setAffinity(reused, CpuAffinity::fromMask(1U)).success);

    const auto result = actions.setAffinity(child.target(), CpuAffinity::fromMask(1U));
    ASSERT_TRUE(result.success) << result.errorMessage;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(child.target().pid));
    ASSERT_NE(process, nullptr);
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    EXPECT_NE(GetProcessAffinityMask(process, &processMask, &systemMask), 0);
    EXPECT_EQ(processMask, DWORD_PTR{1});
    CloseHandle(process);
}

TEST(WindowsProcessActionsTest, AffinityLayoutListsThisMachinesCpus)
{
    const WindowsProcessActions actions;
    const CpuAffinityLayout layout = actions.affinityLayout();
    ASSERT_FALSE(layout.groupSizes.empty());
    EXPECT_FALSE(layout.online.empty());
    EXPECT_TRUE(layout.online.test(0));
}

// =============================================================================
// Efficiency mode (#3071): through faked Win32 calls, then once against a real suspended child
// =============================================================================

namespace
{

/// What the faked Efficiency mode calls do and saw; each test starts from reset(). The handle the open
/// hands out is a real one to this test process, which only the fakes ever see: nothing here changes
/// this process's priority or power throttling.
struct FakeEco
{
    struct Throttle
    {
        ULONG control = 0;
        ULONG state = 0;
        friend bool operator==(const Throttle&, const Throttle&) = default;
    };

    static inline std::uint64_t creationTicks = 0;
    static inline DWORD openError = 0;     // OpenProcess fails with this, when not 0
    static inline DWORD throttleError = 0; // SetProcessInformation fails with this, when not 0
    static inline DWORD classError = 0;    // SetPriorityClass fails with this, when not 0
    static inline DWORD openedAccess = 0;
    static inline DWORD priorityClass = 0; // What GetPriorityClass reports; SetPriorityClass changes it
    static inline bool critical = false;
    static inline bool isProtected = false;
    static inline std::vector<Throttle> throttles;
    static inline std::vector<DWORD> classesSet;

    static void reset()
    {
        creationTicks = 1234;
        openError = 0;
        throttleError = 0;
        classError = 0;
        openedAccess = 0;
        priorityClass = NORMAL_PRIORITY_CLASS;
        critical = false;
        isProtected = false;
        throttles.clear();
        classesSet.clear();
    }

    static HANDLE WINAPI openProcess(DWORD access, BOOL /*inherit*/, DWORD /*pid*/)
    {
        openedAccess = access;
        if (openError != 0)
        {
            SetLastError(openError);
            return nullptr;
        }
        return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    }
    static BOOL WINAPI
    getProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
    {
        creation->dwLowDateTime = static_cast<DWORD>(creationTicks & 0xFFFFFFFFU);
        creation->dwHighDateTime = static_cast<DWORD>(creationTicks >> 32U);
        return TRUE;
    }
    static BOOL WINAPI isProcessCritical(HANDLE /*process*/, PBOOL isCritical)
    {
        *isCritical = critical ? TRUE : FALSE;
        return TRUE;
    }
    static BOOL WINAPI getProcessInformation(HANDLE /*process*/, PROCESS_INFORMATION_CLASS infoClass, LPVOID info, DWORD size)
    {
        if (infoClass != ProcessProtectionLevelInfo || size != sizeof(PROCESS_PROTECTION_LEVEL_INFORMATION))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        static_cast<PROCESS_PROTECTION_LEVEL_INFORMATION*>(info)->ProtectionLevel =
            isProtected ? PROTECTION_LEVEL_ANTIMALWARE_LIGHT : PROTECTION_LEVEL_NONE;
        return TRUE;
    }
    static DWORD WINAPI getPriorityClass(HANDLE /*process*/)
    {
        return priorityClass;
    }
    static BOOL WINAPI setPriorityClass(HANDLE /*process*/, DWORD newClass)
    {
        classesSet.push_back(newClass);
        if (classError != 0)
        {
            SetLastError(classError);
            return FALSE;
        }
        priorityClass = newClass;
        return TRUE;
    }
    static BOOL WINAPI setProcessInformation(HANDLE /*process*/, PROCESS_INFORMATION_CLASS infoClass, LPVOID info, DWORD size)
    {
        if (infoClass != ProcessPowerThrottling || size != sizeof(PROCESS_POWER_THROTTLING_STATE))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        const auto* state = static_cast<const PROCESS_POWER_THROTTLING_STATE*>(info);
        EXPECT_EQ(state->Version, static_cast<ULONG>(PROCESS_POWER_THROTTLING_CURRENT_VERSION));
        throttles.push_back({.control = state->ControlMask, .state = state->StateMask});
        if (throttleError != 0)
        {
            SetLastError(throttleError);
            return FALSE;
        }
        return TRUE;
    }

    [[nodiscard]] static WindowsProcessFunctions functions()
    {
        WindowsProcessFunctions fns{};
        fns.openProcess = &openProcess;
        fns.getProcessTimes = &getProcessTimes;
        fns.readGroupSizes = []
        {
            return std::vector<std::uint32_t>{8};
        };
        fns.isProcessCritical = &isProcessCritical;
        fns.getProcessInformation = &getProcessInformation;
        fns.getCurrentProcessId = []
        {
            return DWORD{1};
        }; // never a fake target's
        fns.getPriorityClass = &getPriorityClass;
        fns.setPriorityClass = &setPriorityClass;
        fns.setProcessInformation = &setProcessInformation;
        return fns;
    }
};

constexpr FakeEco::Throttle ECO_ON{.control = PROCESS_POWER_THROTTLING_EXECUTION_SPEED, .state = PROCESS_POWER_THROTTLING_EXECUTION_SPEED};
constexpr FakeEco::Throttle ECO_HANDED_BACK{.control = 0, .state = 0};

/// The priority classes every WindowsProcessActions shares are process-wide, so each test names its own
/// fake process (its PID), whose start time is FakeEco's.
[[nodiscard]] constexpr ProcessTarget ecoTarget(std::int32_t pid)
{
    return {.pid = pid, .startTimeTicks = 1234};
}

class WindowsEfficiencyModeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeEco::reset();
    }
};

} // namespace

TEST_F(WindowsEfficiencyModeTest, OnSetsEcoQosAndIdleAndOffRestoresTheClassItHad)
{
    WindowsProcessActions actions(FakeEco::functions());
    EXPECT_TRUE(actions.actionCapabilities().canSetEfficiencyMode);
    FakeEco::priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;

    const auto on = actions.setEfficiencyMode(ecoTarget(7101), true);
    ASSERT_TRUE(on.success) << on.errorMessage;
    EXPECT_NE(FakeEco::openedAccess & PROCESS_SET_INFORMATION, 0U);
    EXPECT_EQ(FakeEco::throttles, (std::vector<FakeEco::Throttle>{ECO_ON}));
    EXPECT_EQ(FakeEco::classesSet, (std::vector<DWORD>{IDLE_PRIORITY_CLASS}));

    const auto off = actions.setEfficiencyMode(ecoTarget(7101), false);
    ASSERT_TRUE(off.success) << off.errorMessage;
    EXPECT_EQ(FakeEco::throttles, (std::vector<FakeEco::Throttle>{ECO_ON, ECO_HANDED_BACK}));
    EXPECT_EQ(FakeEco::classesSet, (std::vector<DWORD>{IDLE_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS})) << "the class it had";
}

TEST_F(WindowsEfficiencyModeTest, TheClassIsRememberedForEveryActionsInstanceAndOnlyForThatProcess)
{
    // The Processes table and Process Details each have their own actions: one turns it on, the other off.
    WindowsProcessActions table(FakeEco::functions());
    WindowsProcessActions details(FakeEco::functions());
    FakeEco::priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    ASSERT_TRUE(table.setEfficiencyMode(ecoTarget(7102), true).success);
    ASSERT_TRUE(details.setEfficiencyMode(ecoTarget(7102), true).success) << "on again: Idle doesn't replace the class remembered";

    // Another process with the same PID (a reused one) is a different process: it has nothing remembered.
    FakeEco::creationTicks = 9999;
    ASSERT_TRUE(details.setEfficiencyMode({.pid = 7102, .startTimeTicks = 9999}, false).success);
    EXPECT_EQ(FakeEco::classesSet.back(), static_cast<DWORD>(NORMAL_PRIORITY_CLASS));

    FakeEco::creationTicks = 1234;
    FakeEco::priorityClass = IDLE_PRIORITY_CLASS;
    ASSERT_TRUE(details.setEfficiencyMode(ecoTarget(7102), false).success);
    EXPECT_EQ(FakeEco::classesSet.back(), static_cast<DWORD>(BELOW_NORMAL_PRIORITY_CLASS));
}

TEST_F(WindowsEfficiencyModeTest, OffWithNothingRememberedRestoresNormalFromIdleAndLeavesAnyOtherClass)
{
    WindowsProcessActions actions(FakeEco::functions());
    FakeEco::priorityClass = IDLE_PRIORITY_CLASS; // Task Manager, or the process itself, put it there
    ASSERT_TRUE(actions.setEfficiencyMode(ecoTarget(7103), false).success);
    EXPECT_EQ(FakeEco::throttles, (std::vector<FakeEco::Throttle>{ECO_HANDED_BACK}));
    EXPECT_EQ(FakeEco::classesSet, (std::vector<DWORD>{NORMAL_PRIORITY_CLASS}));

    // Changed since: a class someone chose is left as it is.
    FakeEco::classesSet.clear();
    FakeEco::priorityClass = HIGH_PRIORITY_CLASS;
    ASSERT_TRUE(actions.setEfficiencyMode(ecoTarget(7103), false).success);
    EXPECT_TRUE(FakeEco::classesSet.empty());
}

TEST_F(WindowsEfficiencyModeTest, ProtectedCriticalAndSystemProcessesAndAReusedPidAreRefused)
{
    WindowsProcessActions actions(FakeEco::functions());

    FakeEco::critical = true;
    const auto critical = actions.setEfficiencyMode(ecoTarget(7104), true);
    EXPECT_FALSE(critical.success);
    EXPECT_FALSE(critical.permissionDenied) << "no administrator makes it safe";
    EXPECT_NE(critical.errorMessage.find("critical system process"), std::string::npos) << critical.errorMessage;
    EXPECT_NE(critical.errorMessage.find("Efficiency mode"), std::string::npos) << critical.errorMessage;

    FakeEco::critical = false;
    FakeEco::isProtected = true;
    const auto guarded = actions.setEfficiencyMode(ecoTarget(7104), true);
    EXPECT_FALSE(guarded.success);
    EXPECT_FALSE(guarded.permissionDenied);
    EXPECT_NE(guarded.errorMessage.find("protected process"), std::string::npos) << guarded.errorMessage;

    const auto system = actions.setEfficiencyMode({.pid = SYSTEM_PROCESS_PID, .startTimeTicks = 1234}, true);
    EXPECT_FALSE(system.success);
    EXPECT_NE(system.errorMessage.find("System process"), std::string::npos) << system.errorMessage;

    FakeEco::isProtected = false;
    FakeEco::creationTicks = 9999;
    const auto reused = actions.setEfficiencyMode(ecoTarget(7104), true);
    EXPECT_FALSE(reused.success);
    EXPECT_NE(reused.errorMessage.find("different process"), std::string::npos) << reused.errorMessage;

    EXPECT_TRUE(FakeEco::throttles.empty()) << "nothing reached a refused process";
    EXPECT_TRUE(FakeEco::classesSet.empty());
}

TEST_F(WindowsEfficiencyModeTest, AccessDeniedIsAPermissionRefusalTheHelperCanRetry)
{
    WindowsProcessActions actions(FakeEco::functions());
    FakeEco::openError = ERROR_ACCESS_DENIED; // elevated, or another user's
    const auto opened = actions.setEfficiencyMode(ecoTarget(7105), true);
    EXPECT_FALSE(opened.success);
    EXPECT_TRUE(opened.permissionDenied);
    EXPECT_EQ(opened.errorMessage,
              "Access denied: can't change the Efficiency mode of process 7105. It may run elevated or as another user.");

    FakeEco::openError = 0;
    FakeEco::throttleError = ERROR_ACCESS_DENIED;
    const auto refused = actions.setEfficiencyMode(ecoTarget(7105), true);
    EXPECT_FALSE(refused.success);
    EXPECT_TRUE(refused.permissionDenied);
    EXPECT_TRUE(FakeEco::classesSet.empty()) << "the class isn't touched when the throttling is refused";

    FakeEco::openError = ERROR_INVALID_PARAMETER;
    EXPECT_EQ(actions.setEfficiencyMode(ecoTarget(7105), true).errorMessage, "Process not found - may have already exited");
}

TEST_F(WindowsEfficiencyModeTest, ARefusedThrottlingOrClassChangeIsReportedAndARefusedOnChangesNothing)
{
    WindowsProcessActions actions(FakeEco::functions());
    FakeEco::throttleError = ERROR_GEN_FAILURE;
    const auto throttle = actions.setEfficiencyMode(ecoTarget(7106), true);
    EXPECT_FALSE(throttle.success);
    EXPECT_FALSE(throttle.permissionDenied);
    EXPECT_EQ(throttle.errorMessage, std::format("Failed to turn Efficiency mode on for process 7106: error {}", ERROR_GEN_FAILURE));

    FakeEco::throttleError = 0;
    FakeEco::throttles.clear();
    FakeEco::classError = ERROR_GEN_FAILURE;
    const auto on = actions.setEfficiencyMode(ecoTarget(7106), true);
    EXPECT_FALSE(on.success);
    EXPECT_NE(on.errorMessage.find("Efficiency mode was not turned on"), std::string::npos) << on.errorMessage;
    EXPECT_EQ(FakeEco::throttles, (std::vector<FakeEco::Throttle>{ECO_ON, ECO_HANDED_BACK})) << "the throttling is undone";

    FakeEco::priorityClass = IDLE_PRIORITY_CLASS;
    const auto off = actions.setEfficiencyMode(ecoTarget(7106), false);
    EXPECT_FALSE(off.success);
    EXPECT_NE(off.errorMessage.find("its priority could not be restored"), std::string::npos) << off.errorMessage;
}

TEST_F(WindowsEfficiencyModeTest, WithoutItsCallsThereIsNoEfficiencyModeAndNothingIsOpened)
{
    WindowsProcessFunctions fns = FakeEco::functions();
    fns.setProcessInformation = nullptr;
    WindowsProcessActions actions(fns);
    EXPECT_FALSE(actions.actionCapabilities().canSetEfficiencyMode);
    const auto result = actions.setEfficiencyMode(ecoTarget(7107), true);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("not available"), std::string::npos) << result.errorMessage;
    EXPECT_EQ(FakeEco::openedAccess, 0U);

    EXPECT_TRUE(WindowsProcessActions{}.actionCapabilities().canSetEfficiencyMode) << "the real calls";
}

TEST(WindowsEfficiencyModeMathTest, TheClassRestoredIsTheOneRememberedWhileTheProcessIsStillAtIdle)
{
    EXPECT_EQ(priorityClassToRestore(HIGH_PRIORITY_CLASS, IDLE_PRIORITY_CLASS), std::optional<uint32_t>{HIGH_PRIORITY_CLASS});
    EXPECT_EQ(priorityClassToRestore(std::nullopt, IDLE_PRIORITY_CLASS), std::optional<uint32_t>{NORMAL_PRIORITY_CLASS});
    EXPECT_EQ(priorityClassToRestore(HIGH_PRIORITY_CLASS, 0), std::optional<uint32_t>{HIGH_PRIORITY_CLASS}) << "unreadable now";
    EXPECT_EQ(priorityClassToRestore(std::nullopt, 0), std::optional<uint32_t>{NORMAL_PRIORITY_CLASS});
    EXPECT_FALSE(priorityClassToRestore(IDLE_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS).has_value()) << "changed since";
}

TEST(WindowsEfficiencyModeMathTest, ThePrioritiesKeepTheFirstRealClassAndStayBounded)
{
    EfficiencyModePriorities priorities;
    const ProcessTarget target{.pid = 10, .startTimeTicks = 20};
    priorities.remember(target, 0); // unreadable: nothing to remember
    EXPECT_EQ(priorities.size(), 0U);
    priorities.remember(target, IDLE_PRIORITY_CLASS);
    priorities.remember(target, HIGH_PRIORITY_CLASS); // a real class replaces Idle
    priorities.remember(target, IDLE_PRIORITY_CLASS); // on again: kept
    EXPECT_EQ(priorities.take(target), std::optional<uint32_t>{HIGH_PRIORITY_CLASS});
    EXPECT_FALSE(priorities.take(target).has_value()) << "taken once";
    EXPECT_FALSE(priorities.take({.pid = 10, .startTimeTicks = 21}).has_value());

    for (std::int32_t pid = 1; pid <= static_cast<std::int32_t>(EfficiencyModePriorities::MAX_REMEMBERED) + 5; ++pid)
    {
        priorities.remember({.pid = pid, .startTimeTicks = 1}, NORMAL_PRIORITY_CLASS);
    }
    EXPECT_EQ(priorities.size(), EfficiencyModePriorities::MAX_REMEMBERED);
    EXPECT_FALSE(priorities.take({.pid = 1, .startTimeTicks = 1}).has_value()) << "the lowest PID's went first";
    EXPECT_TRUE(priorities.take({.pid = 1000, .startTimeTicks = 1}).has_value());
}

TEST(WindowsEfficiencyModeMathTest, TheRefusalsSayWhatWasntChanged)
{
    EXPECT_EQ(guardedActionWording(GuardedAction::EfficiencyMode), std::string_view{"change its Efficiency mode"});
    EXPECT_NE(protectedProcessRefusal(42, GuardedAction::EfficiencyMode).find("won't let even an administrator change its Efficiency mode"),
              std::string::npos);
    EXPECT_EQ(efficiencyModeFailure(5, 42, false), "Failed to turn Efficiency mode off for process 42: error 5");
    EXPECT_EQ(efficiencyModePriorityFailure(5, 42, false),
              "Efficiency mode was turned off for process 42, but its priority could not be restored: error 5");
}

// The real calls, on this test's own suspended child only: Idle and EcoQoS on, then both handed back.
TEST(WindowsProcessActionsTest, EfficiencyModeTurnsAChildsEcoQosAndIdleOnAndOff)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.started());
    const Windows::UniqueHandle process(
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(child.target().pid)));
    ASSERT_TRUE(process);
    const auto throttling = [&process]
    {
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        EXPECT_NE(GetProcessInformation(process.get(), ProcessPowerThrottling, &state, sizeof(state)), FALSE);
        return state.ControlMask & state.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    };
    const DWORD before = GetPriorityClass(process.get());

    WindowsProcessActions actions;
    ProcessTarget reused = child.target();
    reused.startTimeTicks += 1;
    EXPECT_FALSE(actions.setEfficiencyMode(reused, true).success);
    EXPECT_EQ(GetPriorityClass(process.get()), before) << "nothing reached the child";

    const auto on = actions.setEfficiencyMode(child.target(), true);
    ASSERT_TRUE(on.success) << on.errorMessage;
    EXPECT_EQ(GetPriorityClass(process.get()), static_cast<DWORD>(IDLE_PRIORITY_CLASS));
    EXPECT_NE(throttling(), 0U);

    const auto off = actions.setEfficiencyMode(child.target(), false);
    ASSERT_TRUE(off.success) << off.errorMessage;
    EXPECT_EQ(GetPriorityClass(process.get()), before);
    EXPECT_EQ(throttling(), 0U);
}

// =============================================================================
// Create dump file (#807), against a running child process this test starts (ping, waiting on
// itself), dumped into a folder of the test's own that is deleted afterwards. Nothing else is dumped.
// =============================================================================

namespace
{

/// A running child process (ping counting to itself for a minute), terminated on scope exit.
class RunningChild
{
  public:
    RunningChild()
    {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        std::wstring commandLine = L"ping.exe -n 60 127.0.0.1";
        if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &m_Info) ==
            0)
        {
            m_Info = {};
            return;
        }
        // Past the loader's start-up, so the dump sees a settled process.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    RunningChild(const RunningChild&) = delete;
    RunningChild& operator=(const RunningChild&) = delete;
    RunningChild(RunningChild&&) = delete;
    RunningChild& operator=(RunningChild&&) = delete;
    ~RunningChild()
    {
        if (m_Info.hProcess != nullptr)
        {
            TerminateProcess(m_Info.hProcess, 0);
            WaitForSingleObject(m_Info.hProcess, 5000);
            CloseHandle(m_Info.hThread);
            CloseHandle(m_Info.hProcess);
        }
    }

    [[nodiscard]] bool started() const
    {
        return m_Info.hProcess != nullptr;
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

/// A folder of this test's own under %TEMP%, removed with everything in it on scope exit.
class DumpFolder
{
  public:
    DumpFolder()
        : m_Path(std::filesystem::temp_directory_path() /
                 ("tasksmack-dump-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())))
    {
        std::filesystem::create_directories(m_Path);
    }
    DumpFolder(const DumpFolder&) = delete;
    DumpFolder& operator=(const DumpFolder&) = delete;
    DumpFolder(DumpFolder&&) = delete;
    DumpFolder& operator=(DumpFolder&&) = delete;
    ~DumpFolder()
    {
        try
        {
            std::error_code ignored;
            std::filesystem::remove_all(m_Path, ignored);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a destructor; a folder left behind in %TEMP% is harmless
        {}
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return m_Path;
    }
    [[nodiscard]] bool empty() const
    {
        return std::filesystem::is_empty(m_Path);
    }

  private:
    std::filesystem::path m_Path;
};

} // namespace

TEST(WindowsProcessDumpTest, DumpsAChildIntoTheFolderWithAMinidumpHeader)
{
    const RunningChild child;
    ASSERT_TRUE(child.started());
    const DumpFolder folder;
    WindowsProcessActions actions;

    const ProcessDumpResult result = actions.createDump(child.target(), folder.path(), std::stop_token{});
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->file.parent_path(), folder.path());
    EXPECT_TRUE(result->file.filename().string().contains("_" + std::to_string(child.target().pid) + "_")) << result->file;
    EXPECT_EQ(result->file.extension(), ".dmp");
    ASSERT_TRUE(std::filesystem::exists(result->file));
    EXPECT_EQ(result->sizeBytes, std::filesystem::file_size(result->file));
    EXPECT_GT(result->sizeBytes, 1024U * 1024U); // full memory: far more than a header and a thread list
    EXPECT_TRUE(result->note.empty());           // a 64-bit ping from a 64-bit test
    // Written from a PssCaptureSnapshot clone (#2001), not the running process.
    EXPECT_TRUE(result->fromSnapshot) << result->liveReason;
    EXPECT_TRUE(result->liveReason.empty()) << result->liveReason;

    // A minidump starts "MDMP" (MINIDUMP_HEADER::Signature).
    std::array<char, 4> signature{};
    {
        std::ifstream file(result->file, std::ios::binary);
        file.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        ASSERT_TRUE(file.good());
    }
    EXPECT_EQ(std::string(signature.data(), signature.size()), "MDMP");

    // A second dump of the same process in the same second gets a name of its own.
    const ProcessDumpResult second = actions.createDump(child.target(), folder.path(), std::stop_token{});
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_NE(second->file, result->file);
}

TEST(WindowsProcessDumpTest, AnInvalidOrGoneOrReusedPidWritesNothing)
{
    const DumpFolder folder;
    WindowsProcessActions actions;

    ProcessDumpResult result = actions.createDump({.pid = -1, .startTimeTicks = 1}, folder.path(), std::stop_token{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Invalid PID");

    result = actions.createDump({.pid = std::numeric_limits<int32_t>::max(), .startTimeTicks = 1}, folder.path(), std::stop_token{});
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().empty());

    const RunningChild child;
    ASSERT_TRUE(child.started());
    ProcessTarget reused = child.target();
    reused.startTimeTicks += 1;
    result = actions.createDump(reused, folder.path(), std::stop_token{});
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().contains("different process")) << result.error();

    // An unknown start time is refused too, rather than dumping whatever holds the PID.
    result = actions.createDump({.pid = child.target().pid, .startTimeTicks = 0}, folder.path(), std::stop_token{});
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(folder.empty());
}

TEST(WindowsProcessDumpTest, AMissingFolderIsNamedAndACancelLeavesNoFile)
{
    const RunningChild child;
    ASSERT_TRUE(child.started());
    const DumpFolder folder;
    WindowsProcessActions actions;

    const std::filesystem::path missing = folder.path() / "missing";
    ProcessDumpResult result = actions.createDump(child.target(), missing, std::stop_token{});
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().contains("doesn't exist")) << result.error();

    std::stop_source stop;
    stop.request_stop();
    result = actions.createDump(child.target(), folder.path(), stop.get_token());
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().starts_with("Cancelled")) << result.error();
    EXPECT_TRUE(folder.empty()); // the partial file is deleted
}

namespace
{
/// PssCaptureSnapshot standing in for one that fails, as it does on a process Windows won't clone.
DWORD WINAPI failingCapture(HANDLE /*process*/, PSS_CAPTURE_FLAGS /*flags*/, DWORD /*contextFlags*/, HPSS* snapshot)
{
    *snapshot = nullptr;
    return ERROR_NOT_ENOUGH_MEMORY;
}
} // namespace

TEST(WindowsProcessDumpTest, AFailedSnapshotFallsBackToALiveDumpAndSaysWhy)
{
    const RunningChild child;
    ASSERT_TRUE(child.started());
    const DumpFolder folder;
    WindowsProcessFunctions functions = realWindowsProcessFunctions();
    functions.pssCaptureSnapshot = &failingCapture;
    WindowsProcessActions actions(functions);

    const ProcessDumpResult result = actions.createDump(child.target(), folder.path(), std::stop_token{});
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_FALSE(result->fromSnapshot);
    EXPECT_EQ(result->liveReason, "there wasn't enough memory to take a snapshot of it");
    EXPECT_GT(result->sizeBytes, 1024U * 1024U); // still a full dump, read from the running process
    std::array<char, 4> signature{};
    {
        std::ifstream file(result->file, std::ios::binary);
        file.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        ASSERT_TRUE(file.good());
    }
    EXPECT_EQ(std::string(signature.data(), signature.size()), "MDMP");

    // Without PssCaptureSnapshot at all, the dump is live too, and says so.
    functions.pssCaptureSnapshot = nullptr;
    WindowsProcessActions noSnapshots(functions);
    const ProcessDumpResult live = noSnapshots.createDump(child.target(), folder.path(), std::stop_token{});
    ASSERT_TRUE(live.has_value()) << live.error();
    EXPECT_FALSE(live->fromSnapshot);
    EXPECT_FALSE(live->liveReason.empty());
}

namespace
{
/// processMachine() standing in for a native ARM64 process's, as the x64 build sees one on Windows on ARM.
std::uint16_t arm64Machine(HANDLE /*process*/)
{
    return DumpMath::MACHINE_ARM64;
}
/// processMachine() standing in for a 32-bit x86 (WOW64) process's.
std::uint16_t x86Machine(HANDLE /*process*/)
{
    return DumpMath::MACHINE_I386;
}
} // namespace

TEST(WindowsProcessDumpTest, AProcessOfAnotherArchitectureIsRefusedBeforeAnyFileIsMade)
{
    const RunningChild child;
    ASSERT_TRUE(child.started());
    const DumpFolder folder;
    const std::int32_t pid = child.target().pid;
    const Windows::UniqueHandle process(OpenProcess(Windows::DUMP_PROCESS_ACCESS, FALSE, static_cast<DWORD>(pid)));
    ASSERT_TRUE(process);

    // The real reader: this test and its ping child are the same architecture as the build.
#if defined(_M_ARM64) || defined(__aarch64__)
    constexpr std::uint16_t BUILT_FOR = DumpMath::MACHINE_ARM64;
#else
    constexpr std::uint16_t BUILT_FOR = DumpMath::MACHINE_AMD64;
#endif
    EXPECT_EQ(Windows::processMachine(GetCurrentProcess()), BUILT_FOR);
    EXPECT_EQ(Windows::processMachine(process.get()), BUILT_FOR);
    const Windows::UniqueHandle waitOnly(OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId()));
    ASSERT_TRUE(waitOnly);
    EXPECT_EQ(Windows::processMachine(waitOnly.get()), DumpMath::MACHINE_UNKNOWN); // no query access: unread

    // A process of another architecture: refused, saying why, and nothing is written.
    const Windows::DumpSource arm64{.captureSnapshot = nullptr, .liveReason = "test", .processMachine = &arm64Machine};
    const Windows::DumpSource x86{.captureSnapshot = nullptr, .liveReason = "test", .processMachine = &x86Machine};
    if constexpr (BUILT_FOR == DumpMath::MACHINE_AMD64)
    {
        const ProcessDumpResult refused = Windows::writeProcessDump(process.get(), pid, folder.path(), std::stop_token{}, arm64);
        ASSERT_FALSE(refused.has_value());
        EXPECT_TRUE(refused.error().contains("is an ARM64 process, and this TaskSmack is an x64 build")) << refused.error();
        EXPECT_TRUE(folder.empty());
    }

    // A 32-bit (WOW64) process: dumped, with the note on reading it in WinDbg.
    const ProcessDumpResult noted = Windows::writeProcessDump(process.get(), pid, folder.path(), std::stop_token{}, x86);
    ASSERT_TRUE(noted.has_value()) << noted.error();
    EXPECT_TRUE(noted->note.contains(".effmach x86")) << noted->note;
}

TEST(WindowsProcessDumpTest, DumpSpaceReadsTheFolderAndTheChildsPrivateBytes)
{
    const RunningChild child;
    ASSERT_TRUE(child.started());
    const DumpFolder folder;
    WindowsProcessActions actions;
    EXPECT_TRUE(actions.actionCapabilities().canChooseDumpFolder);

    const DumpSpace space = actions.dumpSpace(child.target(), folder.path());
    ASSERT_TRUE(space.freeBytes.has_value());
    EXPECT_GT(space.freeBytes.value_or(0), 0U);
    ASSERT_TRUE(space.processBytes.has_value());
    EXPECT_GT(space.processBytes.value_or(0), 0U);

    // A reused PID's private bytes are never read; a missing folder has no free space to read.
    ProcessTarget reused = child.target();
    reused.startTimeTicks += 1;
    const DumpSpace other = actions.dumpSpace(reused, folder.path() / "missing");
    EXPECT_FALSE(other.processBytes.has_value());
    EXPECT_FALSE(other.freeBytes.has_value());
}

TEST(WindowsProcessDumpTest, ProtectionIsReadAndAnUnopenablePidIsNotProtected)
{
    EXPECT_FALSE(Windows::isProtectedProcess(GetCurrentProcessId()));
    // No process has an odd PID (they are multiples of 4), so OpenProcess() fails.
    EXPECT_FALSE(Windows::isProtectedProcess(0xFFFF'FFFBU));
}

TEST(WindowsProcessDumpTest, TheDumpHelpersRefuseNoFolderAndAHandleThatCannotBeRead)
{
    const DumpFolder folder;
    // This process, through a handle that can only be waited on: its name, memory and private bytes
    // are all unreadable through it.
    const Windows::UniqueHandle waitOnly(OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId()));
    ASSERT_TRUE(waitOnly);

    EXPECT_FALSE(Windows::freeBytesInFolder({}).has_value());
    EXPECT_FALSE(Windows::processPrivateBytes(waitOnly.get()).has_value());

    const auto pid = static_cast<std::int32_t>(GetCurrentProcessId());
    ProcessDumpResult result = Windows::writeProcessDump(waitOnly.get(), pid, {}, std::stop_token{}, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "No folder was given to write the dump file in");

    // The file is named without the image name it couldn't read, and the cancel already requested
    // deletes it: nothing is left in the folder.
    std::stop_source stop;
    stop.request_stop();
    result = Windows::writeProcessDump(waitOnly.get(), pid, folder.path(), stop.get_token(), {});
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().starts_with("Cancelled")) << result.error();
    EXPECT_TRUE(folder.empty());
}

} // namespace Platform
