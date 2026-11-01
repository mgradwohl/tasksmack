/// @file test_WindowsThreadActions.cpp
/// @brief One thread's Suspend, Resume and End on Windows (#3069): the guard table through faked Win32 calls,
/// the wording, and real calls on a thread this test process starts for the purpose.
///
/// The fakes never hand out a handle that could change anything: OpenProcess returns this test process opened
/// for query only, OpenThread this test's own thread opened for query only, and SuspendThread, ResumeThread and
/// TerminateThread are fakes. The real-call test acts only on a thread of this process that it started itself;
/// no other process's thread is ever opened.

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
#include <string>
#include <vector>

namespace Platform
{
namespace
{

struct FakeThreadWin
{
    static constexpr DWORD TARGET_TICKS = 4321;
    static constexpr DWORD OWN_PID = 1000; // TaskSmack's own PID, as getCurrentProcessId() answers it
    static constexpr DWORD TARGET_PID = 4242;

    static inline BOOL critical = FALSE;
    static inline DWORD protectionLevel = PROTECTION_LEVEL_NONE;
    static inline DWORD processOpenError = 0;
    static inline DWORD threadOpenError = 0;
    static inline DWORD lastThreadAccess = 0;
    static inline DWORD threadOwner = TARGET_PID; // GetProcessIdOfThread's answer
    static inline DWORD previousCount = 0;        // SuspendThread's and ResumeThread's answer
    static inline DWORD callError = 0;            // the call fails with this, when not 0
    static inline int threadOpens = 0;
    static inline int suspends = 0;
    static inline int resumes = 0;
    static inline int terminates = 0;

    static void reset()
    {
        critical = FALSE;
        protectionLevel = PROTECTION_LEVEL_NONE;
        processOpenError = 0;
        threadOpenError = 0;
        lastThreadAccess = 0;
        threadOwner = TARGET_PID;
        previousCount = 0;
        callError = 0;
        threadOpens = 0;
        suspends = 0;
        resumes = 0;
        terminates = 0;
    }

    static HANDLE WINAPI openProcess(DWORD /*access*/, BOOL /*inherit*/, DWORD /*pid*/)
    {
        if (processOpenError != 0)
        {
            SetLastError(processOpenError);
            return nullptr;
        }
        return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    }
    static BOOL WINAPI
    getProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
    {
        creation->dwLowDateTime = TARGET_TICKS;
        creation->dwHighDateTime = 0;
        return TRUE;
    }
    static BOOL WINAPI isProcessCritical(HANDLE /*process*/, PBOOL answer)
    {
        *answer = critical;
        return TRUE;
    }
    static BOOL WINAPI getProcessInformation(HANDLE /*process*/, PROCESS_INFORMATION_CLASS /*infoClass*/, LPVOID info, DWORD /*size*/)
    {
        static_cast<PROCESS_PROTECTION_LEVEL_INFORMATION*>(info)->ProtectionLevel = protectionLevel;
        return TRUE;
    }
    static DWORD WINAPI getCurrentProcessId()
    {
        return OWN_PID;
    }
    /// This test's own thread, opened for query only, whatever was asked for: no fake call below changes it.
    static HANDLE WINAPI openThread(DWORD access, BOOL /*inherit*/, DWORD /*tid*/)
    {
        ++threadOpens;
        lastThreadAccess = access;
        if (threadOpenError != 0)
        {
            SetLastError(threadOpenError);
            return nullptr;
        }
        return OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
    }
    static DWORD WINAPI getProcessIdOfThread(HANDLE /*thread*/)
    {
        return threadOwner;
    }
    static DWORD WINAPI suspendThread(HANDLE /*thread*/)
    {
        ++suspends;
        return answer();
    }
    static DWORD WINAPI resumeThread(HANDLE /*thread*/)
    {
        ++resumes;
        return answer();
    }
    static BOOL WINAPI terminateThread(HANDLE /*thread*/, DWORD /*exitCode*/)
    {
        ++terminates;
        if (callError != 0)
        {
            SetLastError(callError);
            return FALSE;
        }
        return TRUE;
    }
    static DWORD answer()
    {
        if (callError != 0)
        {
            SetLastError(callError);
            return static_cast<DWORD>(-1);
        }
        return previousCount;
    }

    [[nodiscard]] static WindowsProcessFunctions functions()
    {
        WindowsProcessFunctions fns{};
        fns.openProcess = &openProcess;
        fns.getProcessTimes = &getProcessTimes;
        fns.isProcessCritical = &isProcessCritical;
        fns.getProcessInformation = &getProcessInformation;
        fns.getCurrentProcessId = &getCurrentProcessId;
        fns.openThread = &openThread;
        fns.getProcessIdOfThread = &getProcessIdOfThread;
        fns.suspendThread = &suspendThread;
        fns.resumeThread = &resumeThread;
        fns.terminateThread = &terminateThread;
        return fns;
    }
};

constexpr ProcessTarget TARGET{.pid = static_cast<std::int32_t>(FakeThreadWin::TARGET_PID), .startTimeTicks = FakeThreadWin::TARGET_TICKS};
constexpr std::int32_t TID = 777;

class WindowsThreadActionsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeThreadWin::reset();
    }
};

[[nodiscard]] int calls()
{
    return FakeThreadWin::suspends + FakeThreadWin::resumes + FakeThreadWin::terminates;
}

TEST_F(WindowsThreadActionsTest, TheCapabilityNeedsEveryThreadCall)
{
    EXPECT_TRUE(WindowsProcessActions(FakeThreadWin::functions()).actionCapabilities().canActOnThreads);
    WindowsProcessFunctions missing = FakeThreadWin::functions();
    missing.terminateThread = nullptr;
    WindowsProcessActions actions(missing);
    EXPECT_FALSE(actions.actionCapabilities().canActOnThreads);
    const ProcessActionResult result = actions.actOnThread(TARGET, TID, ThreadAction::Suspend);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(FakeThreadWin::threadOpens, 0);
    EXPECT_TRUE(realWindowsProcessFunctions().openThread != nullptr);
}

// The guard table: what is refused before anything is opened, for each action, and never as a permission
// failure (no administrator makes it safe).
TEST_F(WindowsThreadActionsTest, TheKernelsAndTaskSmacksOwnThreadsAreNeverActedOn)
{
    WindowsProcessActions actions(FakeThreadWin::functions());
    actions.refuseThreadActionsOn(5000); // the helper's TaskSmack
    struct Case
    {
        ProcessTarget target;
        std::int32_t tid;
        std::string expected;
    };
    const std::vector<Case> cases{
        {.target = TARGET, .tid = 0, .expected = "Thread 0 isn't a valid thread ID; nothing sent"},
        {.target = TARGET, .tid = -4, .expected = "Thread -4 isn't a valid thread ID; nothing sent"},
        {.target = {.pid = 4, .startTimeTicks = 1},
         .tid = TID,
         .expected = "PID 4 is the System process, whose threads are the kernel's; TaskSmack won't "},
        {.target = {.pid = 1000, .startTimeTicks = 1}, .tid = TID, .expected = "Process 1000 is TaskSmack itself; TaskSmack won't "},
        {.target = {.pid = 1001, .startTimeTicks = 1}, .tid = TID, .expected = "Process 1001 is TaskSmack itself; TaskSmack won't "},
        {.target = {.pid = 5000, .startTimeTicks = 1}, .tid = TID, .expected = "Process 5000 is TaskSmack itself; TaskSmack won't "},
    };
    for (const ThreadAction action : {ThreadAction::Suspend, ThreadAction::Resume, ThreadAction::End})
    {
        for (const Case& c : cases)
        {
            const ProcessActionResult result = actions.actOnThread(c.target, c.tid, action);
            EXPECT_FALSE(result.success) << c.expected;
            EXPECT_TRUE(result.errorMessage.starts_with(c.expected)) << result.errorMessage;
            EXPECT_FALSE(result.permissionDenied) << c.expected;
        }
    }
    EXPECT_EQ(FakeThreadWin::threadOpens, 0);
    EXPECT_EQ(calls(), 0);
}

TEST_F(WindowsThreadActionsTest, ACriticalProcesssThreadsAreNeverSuspendedOrEndedButMayBeResumed)
{
    FakeThreadWin::critical = TRUE;
    WindowsProcessActions actions(FakeThreadWin::functions());
    const std::string critical = "Process 4242 is a critical system process, which Windows can't run without; TaskSmack won't ";
    EXPECT_EQ(actions.actOnThread(TARGET, TID, ThreadAction::Suspend).errorMessage, critical + "suspend its threads");
    EXPECT_EQ(actions.actOnThread(TARGET, TID, ThreadAction::End).errorMessage, critical + "end its threads");
    EXPECT_EQ(calls(), 0);
    // Letting a thread run again can't stop Windows, as the process-level Resume.
    EXPECT_TRUE(actions.actOnThread(TARGET, TID, ThreadAction::Resume).success);
    EXPECT_EQ(FakeThreadWin::resumes, 1);
}

TEST_F(WindowsThreadActionsTest, AProtectedProcesssThreadsAreNeverSuspendedOrEnded)
{
    FakeThreadWin::protectionLevel = PROTECTION_LEVEL_ANTIMALWARE_LIGHT;
    WindowsProcessActions actions(FakeThreadWin::functions());
    const ProcessActionResult suspend = actions.actOnThread(TARGET, TID, ThreadAction::Suspend);
    EXPECT_EQ(suspend.errorMessage, protectedProcessRefusal(4242, GuardedAction::SuspendThread));
    EXPECT_FALSE(suspend.permissionDenied);
    EXPECT_EQ(actions.actOnThread(TARGET, TID, ThreadAction::End).errorMessage, protectedProcessRefusal(4242, GuardedAction::EndThread));
    // Resume reaches the thread's open, which a protected process refuses even an administrator: no retry.
    FakeThreadWin::threadOpenError = ERROR_ACCESS_DENIED;
    const ProcessActionResult resume = actions.actOnThread(TARGET, TID, ThreadAction::Resume);
    EXPECT_FALSE(resume.success);
    EXPECT_FALSE(resume.permissionDenied);
    EXPECT_TRUE(resume.errorMessage.contains("protected process")) << resume.errorMessage;
    EXPECT_EQ(calls(), 0);
}

TEST_F(WindowsThreadActionsTest, ARefusedThreadOpenIsAPermissionFailureTheHelperCanRetry)
{
    FakeThreadWin::threadOpenError = ERROR_ACCESS_DENIED;
    WindowsProcessActions actions(FakeThreadWin::functions());
    const ProcessActionResult result = actions.actOnThread(TARGET, TID, ThreadAction::End);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.permissionDenied);
    EXPECT_EQ(result.errorMessage, "Access denied: can't end thread 777 of process 4242. It may run elevated or as another user.");
    EXPECT_EQ(FakeThreadWin::lastThreadAccess, static_cast<DWORD>(THREAD_TERMINATE | THREAD_QUERY_LIMITED_INFORMATION))
        << "the least access End needs";

    static_cast<void>(actions.actOnThread(TARGET, TID, ThreadAction::Suspend));
    EXPECT_EQ(FakeThreadWin::lastThreadAccess, static_cast<DWORD>(THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION));

    FakeThreadWin::threadOpenError = ERROR_INVALID_PARAMETER;
    const ProcessActionResult gone = actions.actOnThread(TARGET, TID, ThreadAction::Resume);
    EXPECT_EQ(gone.errorMessage, "Thread 777 not found - it may have already exited");
    EXPECT_FALSE(gone.permissionDenied);
    EXPECT_EQ(calls(), 0);
}

TEST_F(WindowsThreadActionsTest, AThreadOfAnotherProcessIsRefused)
{
    FakeThreadWin::threadOwner = 9999; // the TID was reused by another process's thread
    WindowsProcessActions actions(FakeThreadWin::functions());
    const ProcessActionResult result = actions.actOnThread(TARGET, TID, ThreadAction::End);
    EXPECT_EQ(result.errorMessage, "Thread 777 belongs to process 9999, not process 4242; nothing sent");
    FakeThreadWin::threadOwner = 0;
    EXPECT_EQ(actions.actOnThread(TARGET, TID, ThreadAction::Suspend).errorMessage,
              "Cannot confirm that thread 777 belongs to process 4242; nothing sent");
    EXPECT_EQ(calls(), 0);
}

TEST_F(WindowsThreadActionsTest, AReusedProcessIdIsRefusedBeforeItsThreadIsOpened)
{
    WindowsProcessActions actions(FakeThreadWin::functions());
    const ProcessActionResult result = actions.actOnThread({.pid = TARGET.pid, .startTimeTicks = 99}, TID, ThreadAction::End);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Process 4242 has exited and its PID now belongs to a different process; action not sent");
    FakeThreadWin::processOpenError = ERROR_INVALID_PARAMETER;
    EXPECT_EQ(actions.actOnThread(TARGET, TID, ThreadAction::End).errorMessage, "Process not found - may have already exited");
    EXPECT_EQ(FakeThreadWin::threadOpens, 0);
}

TEST_F(WindowsThreadActionsTest, EachActionCallsItsOwnFunctionAndSaysTheSuspendCount)
{
    WindowsProcessActions actions(FakeThreadWin::functions());
    ProcessActionResult result = actions.actOnThread(TARGET, TID, ThreadAction::Suspend);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.successNote.empty()) << "a first suspension";
    FakeThreadWin::previousCount = 2;
    EXPECT_EQ(actions.actOnThread(TARGET, TID, ThreadAction::Suspend).successNote, "its suspend count is now 3");
    EXPECT_EQ(actions.actOnThread(TARGET, TID, ThreadAction::Resume).successNote, "it needs 1 more resume to run again");
    FakeThreadWin::previousCount = 0;
    EXPECT_EQ(actions.actOnThread(TARGET, TID, ThreadAction::Resume).successNote, "it wasn't suspended");
    EXPECT_TRUE(actions.actOnThread(TARGET, TID, ThreadAction::End).success);
    EXPECT_EQ(FakeThreadWin::suspends, 2);
    EXPECT_EQ(FakeThreadWin::resumes, 2);
    EXPECT_EQ(FakeThreadWin::terminates, 1);
}

TEST_F(WindowsThreadActionsTest, AFailedCallSaysWhyAndADeniedOneCanBeRetried)
{
    FakeThreadWin::callError = ERROR_GEN_FAILURE;
    WindowsProcessActions actions(FakeThreadWin::functions());
    const ProcessActionResult suspend = actions.actOnThread(TARGET, TID, ThreadAction::Suspend);
    EXPECT_EQ(suspend.errorMessage, "Failed to suspend thread 777 of process 4242: error 31");
    EXPECT_FALSE(suspend.permissionDenied);
    EXPECT_EQ(actions.actOnThread(TARGET, TID, ThreadAction::End).errorMessage, "Failed to end thread 777 of process 4242: error 31");
    FakeThreadWin::callError = ERROR_ACCESS_DENIED;
    const ProcessActionResult resume = actions.actOnThread(TARGET, TID, ThreadAction::Resume);
    EXPECT_EQ(resume.errorMessage, "Access denied: can't resume thread 777 of process 4242");
    EXPECT_TRUE(resume.permissionDenied);
    EXPECT_TRUE(actions.actOnThread(TARGET, TID, ThreadAction::End).permissionDenied);
}

TEST(WindowsThreadActionsMathTest, TheWordingTables)
{
    EXPECT_STREQ(threadActionVerb(ThreadAction::Suspend), "suspend");
    EXPECT_STREQ(threadActionVerb(ThreadAction::Resume), "resume");
    EXPECT_STREQ(threadActionVerb(ThreadAction::End), "end");
    EXPECT_EQ(threadActionGuard(ThreadAction::Suspend), GuardedAction::SuspendThread);
    EXPECT_EQ(threadActionGuard(ThreadAction::End), GuardedAction::EndThread);
    EXPECT_FALSE(threadActionGuard(ThreadAction::Resume).has_value());
    EXPECT_EQ(threadActionRefusal(4242, 1, ThreadAction::End, 1000, 0), "") << "an ordinary thread";
    EXPECT_EQ(threadActionRefusal(4242, 1, ThreadAction::End, 1000, 4242),
              "Process 4242 is TaskSmack itself; TaskSmack won't end its own threads");
    EXPECT_EQ(threadActionNote(ThreadAction::Suspend, 0), "");
    EXPECT_EQ(threadActionNote(ThreadAction::Resume, 1), "");
    EXPECT_EQ(threadActionNote(ThreadAction::Resume, 4), "it needs 3 more resumes to run again");
    EXPECT_EQ(threadActionNote(ThreadAction::End, 0), "");
    EXPECT_EQ(threadOpenFailure(ERROR_NOT_ENOUGH_MEMORY, 1, 2, ThreadAction::Suspend, false),
              "Failed to open thread 2 of process 1: error 8");
    EXPECT_EQ(accessDeniedMessage(7, GuardedAction::SuspendThread),
              "Access denied: can't suspend the threads of process 7. It may run elevated or as another user.");
    EXPECT_EQ(accessDeniedMessage(7, GuardedAction::EndThread),
              "Access denied: can't end the threads of process 7. It may run elevated or as another user.");
    EXPECT_STREQ(guardedActionWording(GuardedAction::SuspendThread), "suspend its threads");
}

/// A thread of this test process, waiting until told to stop or ended: the only thread the real calls act on.
class OwnTestThread
{
  public:
    OwnTestThread()
        : m_Stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          m_Started(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          m_Thread(CreateThread(nullptr, 0, &OwnTestThread::run, this, 0, &m_Tid))
    {}
    ~OwnTestThread()
    {
        SetEvent(m_Stop);
        WaitForSingleObject(m_Thread, INFINITE);
        CloseHandle(m_Thread);
        CloseHandle(m_Started);
        CloseHandle(m_Stop);
    }
    OwnTestThread(const OwnTestThread&) = delete;
    OwnTestThread& operator=(const OwnTestThread&) = delete;
    OwnTestThread(OwnTestThread&&) = delete;
    OwnTestThread& operator=(OwnTestThread&&) = delete;

    [[nodiscard]] HANDLE handle() const noexcept
    {
        return m_Thread;
    }
    /// Waits until the thread runs its own code, so suspending it can't leave the loader lock held.
    [[nodiscard]] bool waitStarted() const noexcept
    {
        return WaitForSingleObject(m_Started, 5000) == WAIT_OBJECT_0;
    }
    [[nodiscard]] std::int32_t tid() const noexcept
    {
        return static_cast<std::int32_t>(m_Tid);
    }

  private:
    static DWORD WINAPI run(LPVOID self)
    {
        const auto* thread = static_cast<const OwnTestThread*>(self);
        SetEvent(thread->m_Started);
        WaitForSingleObject(thread->m_Stop, INFINITE); // holds no lock: safe to end
        return 0;
    }

    HANDLE m_Stop = nullptr;
    HANDLE m_Started = nullptr; // set once the thread runs: past its start-up, which holds the loader lock
    DWORD m_Tid = 0;
    HANDLE m_Thread = nullptr;
};

[[nodiscard]] ProcessTarget thisProcess()
{
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    return {.pid = static_cast<std::int32_t>(GetCurrentProcessId()),
            .startTimeTicks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime};
}

/// The real calls, but with an own PID that isn't this process's, so its own guard lets the test act on its
/// own thread (the guard itself is tested above).
[[nodiscard]] WindowsProcessFunctions realCallsOnThisProcess()
{
    WindowsProcessFunctions fns = realWindowsProcessFunctions();
    fns.getCurrentProcessId = []() -> DWORD
    {
        return GetCurrentProcessId() + 4;
    };
    return fns;
}

TEST(WindowsThreadActionsRealTest, SuspendsResumesAndEndsATestThreadOfItsOwn)
{
    const OwnTestThread thread;
    ASSERT_NE(thread.handle(), nullptr);
    ASSERT_TRUE(thread.waitStarted());
    WindowsProcessActions actions(realCallsOnThisProcess());
    const ProcessTarget self = thisProcess();

    ProcessActionResult result = actions.actOnThread(self, thread.tid(), ThreadAction::Suspend);
    ASSERT_TRUE(result.success) << result.errorMessage;
    result = actions.actOnThread(self, thread.tid(), ThreadAction::Suspend);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.successNote, "its suspend count is now 2");
    EXPECT_EQ(ResumeThread(thread.handle()), 2U) << "Windows counted both";
    result = actions.actOnThread(self, thread.tid(), ThreadAction::Resume);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(result.successNote.empty());
    EXPECT_EQ(actions.actOnThread(self, thread.tid(), ThreadAction::Resume).successNote, "it wasn't suspended");

    result = actions.actOnThread(self, thread.tid(), ThreadAction::End);
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(WaitForSingleObject(thread.handle(), 5000), WAIT_OBJECT_0) << "ended";
    DWORD exitCode = 0;
    ASSERT_NE(GetExitCodeThread(thread.handle(), &exitCode), FALSE);
    EXPECT_EQ(exitCode, 1U);
}

} // namespace
} // namespace Platform
