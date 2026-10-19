/// @file test_WindowsProcessActionsSeams.cpp
/// @brief WindowsProcessActions' failure paths through faked Win32 calls (#2039), and the real
/// CPU-set reader, which only a machine with several processor groups calls.
///
/// The fakes never hand out a handle that could change anything: OpenProcess returns this test
/// process opened for query only, so a setter that reaches Windows is refused.

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/Windows/WindowsAffinityMath.h"
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

#include <cstddef>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

/// What the faked calls do and saw. Function pointers can't capture, so the fakes share this state;
/// each test starts from reset().
struct FailingWin
{
    static inline DWORD openError = 0;     // OpenProcess fails with this, when not 0
    static inline DWORD timesError = 0;    // GetProcessTimes fails with this, when not 0
    static inline DWORD refusedAccess = 0; // OpenProcess asked for any of these rights fails with ERROR_ACCESS_DENIED
    static inline BOOL critical = FALSE;   // IsProcessCritical's answer
    static inline DWORD criticalError = 0; // IsProcessCritical fails with this, when not 0
    static inline DWORD protectionLevel = PROTECTION_LEVEL_NONE;
    static inline int opens = 0;
    static inline int criticalChecks = 0;
    static inline int setterCalls = 0;
    static inline LONG suspendStatus = 0; // NtSuspendProcess's NTSTATUS
    static inline LONG resumeStatus = 0;  // NtResumeProcess's NTSTATUS
    static inline int suspendCalls = 0;
    static inline int resumeCalls = 0;

    static void reset()
    {
        openError = 0;
        timesError = 0;
        refusedAccess = 0;
        critical = FALSE;
        criticalError = 0;
        protectionLevel = PROTECTION_LEVEL_NONE;
        opens = 0;
        criticalChecks = 0;
        setterCalls = 0;
        suspendStatus = 0;
        resumeStatus = 0;
        suspendCalls = 0;
        resumeCalls = 0;
    }

    static HANDLE WINAPI openProcess(DWORD access, BOOL /*inherit*/, DWORD /*pid*/)
    {
        ++opens;
        if ((access & refusedAccess) != 0)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }
        if (openError != 0)
        {
            SetLastError(openError);
            return nullptr;
        }
        // Query-only, whatever was asked for: a setter that gets this far is refused by Windows.
        return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    }
    static BOOL WINAPI
    getProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
    {
        if (timesError != 0)
        {
            SetLastError(timesError);
            return FALSE;
        }
        creation->dwLowDateTime = TARGET_TICKS;
        creation->dwHighDateTime = 0;
        return TRUE;
    }
    static BOOL WINAPI setProcessAffinityMask(HANDLE /*process*/, DWORD_PTR /*mask*/)
    {
        ++setterCalls;
        return TRUE;
    }
    static BOOL WINAPI setProcessDefaultCpuSets(HANDLE /*process*/, const ULONG* /*ids*/, ULONG /*count*/)
    {
        ++setterCalls;
        return TRUE;
    }
    static BOOL WINAPI isProcessCritical(HANDLE /*process*/, PBOOL answer)
    {
        ++criticalChecks;
        if (criticalError != 0)
        {
            SetLastError(criticalError);
            return FALSE;
        }
        *answer = critical;
        return TRUE;
    }
    static BOOL WINAPI getProcessInformation(HANDLE /*process*/, PROCESS_INFORMATION_CLASS infoClass, LPVOID info, DWORD size)
    {
        if (infoClass != ProcessProtectionLevelInfo || size != sizeof(PROCESS_PROTECTION_LEVEL_INFORMATION))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        static_cast<PROCESS_PROTECTION_LEVEL_INFORMATION*>(info)->ProtectionLevel = protectionLevel;
        return TRUE;
    }

    // Never the real calls: the handle they are given is this test process's, and it is never suspended.
    static LONG NTAPI ntSuspendProcess(HANDLE /*process*/)
    {
        ++suspendCalls;
        return suspendStatus;
    }
    static LONG NTAPI ntResumeProcess(HANDLE /*process*/)
    {
        ++resumeCalls;
        return resumeStatus;
    }

    [[nodiscard]] static WindowsProcessFunctions functions()
    {
        return {
            .openProcess = &openProcess,
            .getProcessTimes = &getProcessTimes,
            .setProcessAffinityMask = &setProcessAffinityMask,
            .setProcessDefaultCpuSets = &setProcessDefaultCpuSets,
            .readGroupSizes = [] { return std::vector<std::uint32_t>{8}; },
            .readCores = [] { return std::vector<ProcessorCoreRecord>{}; },
            .readCpuSets = [] { return std::vector<CpuSetRecord>{}; },
            .pssCaptureSnapshot = nullptr,
            .isProcessCritical = &isProcessCritical,
            .getProcessInformation = &getProcessInformation,
            .ntSuspendProcess = &ntSuspendProcess,
            .ntResumeProcess = &ntResumeProcess,
        };
    }

    static constexpr DWORD TARGET_TICKS = 4321;
};

constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = FailingWin::TARGET_TICKS};

class WindowsProcessActionsSeamTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FailingWin::reset();
    }
};

} // namespace

TEST_F(WindowsProcessActionsSeamTest, AnUnreadableCreationTimeStopsEveryActionBeforeItIsSent)
{
    FailingWin::timesError = ERROR_ACCESS_DENIED;
    WindowsProcessActions actions(FailingWin::functions());
    const std::string expected = "Cannot confirm the identity of process 4242: error 5; action not sent";

    const std::vector<std::pair<const char*, ProcessActionResult>> results{
        {"terminate", actions.terminate(TARGET)},
        {"kill", actions.kill(TARGET)},
        {"setPriority", actions.setPriority(TARGET, 10)},
        {"setAffinity", actions.setAffinity(TARGET, CpuAffinity::fromMask(0b1U))},
        {"stop", actions.stop(TARGET)},
        {"resume", actions.resume(TARGET)},
    };
    for (const auto& [name, result] : results)
    {
        EXPECT_FALSE(result.success) << name;
        EXPECT_EQ(result.errorMessage, expected) << name;
    }
    EXPECT_EQ(FailingWin::setterCalls, 0);
    EXPECT_EQ(FailingWin::suspendCalls + FailingWin::resumeCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, AnUnexpectedOpenErrorIsReportedWithItsCode)
{
    FailingWin::openError = ERROR_NOT_ENOUGH_MEMORY;
    WindowsProcessActions actions(FailingWin::functions());
    const std::string expected = "Failed to open process 4242: error 8";

    EXPECT_EQ(actions.setPriority(TARGET, 0).errorMessage, expected);
    EXPECT_EQ(actions.setAffinity(TARGET, CpuAffinity::fromMask(0b1U)).errorMessage, expected);
    EXPECT_EQ(actions.kill(TARGET).errorMessage, expected);
    EXPECT_EQ(actions.terminate(TARGET).errorMessage, expected);
    EXPECT_EQ(FailingWin::setterCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, APriorityWindowsRefusesIsReportedWithItsError)
{
    // The identity check passes, but the handle is query-only, so SetPriorityClass is refused.
    WindowsProcessActions actions(FailingWin::functions());

    const ProcessActionResult result = actions.setPriority(TARGET, -10);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Failed to set priority for process 4242: error 5");
}

// The critical-process guard (#2556). Every "critical" process here is this test process, opened
// for query only, and the answer is the fake's: no real critical process is ever opened or touched.

TEST_F(WindowsProcessActionsSeamTest, ACriticalProcessIsNeverEndedReprioritisedOrRepinned)
{
    FailingWin::critical = TRUE;
    WindowsProcessActions actions(FailingWin::functions());
    const std::string critical = "Process 4242 is a critical system process, which Windows can't run without; TaskSmack won't ";

    const std::vector<std::pair<std::string, ProcessActionResult>> results{
        {critical + "end it", actions.kill(TARGET)},
        {critical + "end it", actions.terminate(TARGET)},
        {critical + "change its priority", actions.setPriority(TARGET, 10)},
        {critical + "change its priority", actions.setPriority(TARGET, -20)},
        {critical + "change its CPU affinity", actions.setAffinity(TARGET, CpuAffinity::fromMask(0b1U))},
        {critical + "suspend it", actions.stop(TARGET)},
    };
    for (const auto& [expected, result] : results)
    {
        EXPECT_FALSE(result.success) << expected;
        EXPECT_EQ(result.errorMessage, expected);
        // Not a permission failure: the UI must not offer "Authenticate and retry", since an
        // administrator ending it bugchecks Windows just the same.
        EXPECT_FALSE(result.permissionDenied) << expected;
    }
    EXPECT_EQ(FailingWin::criticalChecks, 6);
    EXPECT_EQ(FailingWin::setterCalls, 0);
    EXPECT_EQ(FailingWin::suspendCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, AProcessWhoseCriticalityCantBeReadIsLeftAlone)
{
    FailingWin::criticalError = ERROR_ACCESS_DENIED;
    WindowsProcessActions actions(FailingWin::functions());
    const std::string expected = "Cannot tell whether process 4242 is a critical system process: error 5; action not sent";

    EXPECT_EQ(actions.kill(TARGET).errorMessage, expected);
    EXPECT_EQ(actions.terminate(TARGET).errorMessage, expected);
    EXPECT_EQ(actions.setPriority(TARGET, 0).errorMessage, expected);
    EXPECT_EQ(actions.setAffinity(TARGET, CpuAffinity::fromMask(0b1U)).errorMessage, expected);
    EXPECT_EQ(actions.stop(TARGET).errorMessage, expected);
    EXPECT_EQ(FailingWin::setterCalls, 0);
    EXPECT_EQ(FailingWin::suspendCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, WithoutACriticalCheckNothingIsSent)
{
    WindowsProcessFunctions functions = FailingWin::functions();
    functions.isProcessCritical = nullptr;
    WindowsProcessActions actions(functions);
    const std::string expected = "Cannot tell whether process 4242 is a critical system process: error 120; action not sent";

    EXPECT_EQ(actions.kill(TARGET).errorMessage, expected);
    EXPECT_EQ(actions.setPriority(TARGET, 0).errorMessage, expected);
    EXPECT_EQ(actions.setAffinity(TARGET, CpuAffinity::fromMask(0b1U)).errorMessage, expected);
    EXPECT_EQ(FailingWin::setterCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, ANonCriticalProcessIsActedOn)
{
    // The guard answers "not critical", so the action reaches Windows, which refuses the query-only handle.
    WindowsProcessActions actions(FailingWin::functions());

    EXPECT_EQ(actions.kill(TARGET).errorMessage, "Failed to terminate process 4242: error 5");
    EXPECT_EQ(actions.setAffinity(TARGET, CpuAffinity::fromMask(0b1U)).errorMessage, "");
    EXPECT_EQ(FailingWin::setterCalls, 1);
}

TEST_F(WindowsProcessActionsSeamTest, AProtectedProcessIsNeverEndedAndSaysWhy)
{
    FailingWin::protectionLevel = PROTECTION_LEVEL_ANTIMALWARE_LIGHT;
    WindowsProcessActions actions(FailingWin::functions());
    const std::string expected = "Process 4242 is a protected process (a system or anti-malware service), which not even an administrator "
                                 "can end; TaskSmack won't end it";

    const ProcessActionResult killed = actions.kill(TARGET);
    EXPECT_EQ(killed.errorMessage, expected);
    EXPECT_FALSE(killed.permissionDenied);
    EXPECT_EQ(actions.terminate(TARGET).errorMessage, expected);

    // Windows refuses a protected process's priority itself: only ending one is guarded here.
    EXPECT_EQ(actions.setPriority(TARGET, 0).errorMessage, "Failed to set priority for process 4242: error 5");
}

TEST_F(WindowsProcessActionsSeamTest, AProtectedProcessThatRefusesTheKillsOpenIsNamedAsProtected)
{
    // Windows refuses PROCESS_TERMINATE on a protected process before any handle exists; a limited
    // query, which it allows, says why.
    FailingWin::refusedAccess = PROCESS_TERMINATE;
    FailingWin::protectionLevel = PROTECTION_LEVEL_WINTCB_LIGHT;
    WindowsProcessActions actions(FailingWin::functions());

    const ProcessActionResult result = actions.kill(TARGET);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied);
    EXPECT_EQ(result.errorMessage,
              "Process 4242 is a protected process (a system or anti-malware service), which not even an administrator can end; "
              "TaskSmack won't end it");
}

TEST_F(WindowsProcessActionsSeamTest, AnUnprotectedProcessThatRefusesTheKillsOpenKeepsTheError)
{
    FailingWin::refusedAccess = PROCESS_TERMINATE;
    WindowsProcessActions actions(FailingWin::functions());

    EXPECT_EQ(actions.kill(TARGET).errorMessage, "Failed to open process 4242: error 5");
}

TEST_F(WindowsProcessActionsSeamTest, TheKernelsPidsAreRefusedBeforeAnythingIsOpened)
{
    WindowsProcessActions actions(FailingWin::functions());

    constexpr ProcessTarget SYSTEM{.pid = 4, .startTimeTicks = FailingWin::TARGET_TICKS};
    const ProcessActionResult killed = actions.kill(SYSTEM);
    EXPECT_EQ(killed.errorMessage, "PID 4 is the System process; TaskSmack won't end it");
    EXPECT_FALSE(killed.permissionDenied);
    EXPECT_EQ(actions.terminate(SYSTEM).errorMessage, "PID 4 is the System process; TaskSmack won't end it");
    EXPECT_EQ(actions.setPriority(SYSTEM, 0).errorMessage, "PID 4 is the System process; TaskSmack won't change its priority");
    EXPECT_EQ(actions.setAffinity(SYSTEM, CpuAffinity::fromMask(0b1U)).errorMessage,
              "PID 4 is the System process; TaskSmack won't change its CPU affinity");

    // PID 0, the System Idle Process, is an invalid PID to every action.
    constexpr ProcessTarget IDLE{.pid = 0, .startTimeTicks = FailingWin::TARGET_TICKS};
    EXPECT_EQ(actions.kill(IDLE).errorMessage, "Invalid PID");
    EXPECT_EQ(actions.terminate(IDLE).errorMessage, "Invalid PID");
    EXPECT_EQ(actions.setPriority(IDLE, 0).errorMessage, "Invalid PID");
    EXPECT_EQ(actions.setAffinity(IDLE, CpuAffinity::fromMask(0b1U)).errorMessage, "Invalid PID");

    EXPECT_EQ(FailingWin::opens, 0);
    EXPECT_EQ(FailingWin::setterCalls, 0);
}

// Suspend and Resume (#2582), through faked NtSuspendProcess and NtResumeProcess: nothing here is ever
// really suspended. The real calls are tested on a child process in test_WindowsProcessActions.cpp.

TEST_F(WindowsProcessActionsSeamTest, SuspendAndResumeGoThroughTheVerifiedHandle)
{
    WindowsProcessActions actions(FailingWin::functions());
    const ProcessActionCapabilities caps = actions.actionCapabilities();
    EXPECT_TRUE(caps.canStop);
    EXPECT_TRUE(caps.canContinue);

    const ProcessActionResult suspended = actions.stop(TARGET);
    EXPECT_TRUE(suspended.success) << suspended.errorMessage;
    EXPECT_EQ(FailingWin::suspendCalls, 1);
    EXPECT_EQ(FailingWin::criticalChecks, 1) << "the guard is asked before a suspend";

    const ProcessActionResult resumed = actions.resume(TARGET);
    EXPECT_TRUE(resumed.success) << resumed.errorMessage;
    EXPECT_EQ(FailingWin::resumeCalls, 1);
    EXPECT_EQ(FailingWin::criticalChecks, 1) << "resuming can't stop Windows, so it isn't guarded";
    EXPECT_EQ(FailingWin::opens, 2);
}

TEST_F(WindowsProcessActionsSeamTest, ACriticalProcessIsNeverSuspendedButCanBeResumed)
{
    FailingWin::critical = TRUE;
    WindowsProcessActions actions(FailingWin::functions());

    const ProcessActionResult suspended = actions.stop(TARGET);
    EXPECT_FALSE(suspended.success);
    EXPECT_EQ(suspended.errorMessage,
              "Process 4242 is a critical system process, which Windows can't run without; TaskSmack won't suspend it");
    EXPECT_FALSE(suspended.permissionDenied) << "no administrator makes it safe";
    EXPECT_EQ(FailingWin::suspendCalls, 0);

    EXPECT_TRUE(actions.resume(TARGET).success);
    EXPECT_EQ(FailingWin::resumeCalls, 1);
}

TEST_F(WindowsProcessActionsSeamTest, AProtectedProcessIsNeverSuspendedAndSaysWhy)
{
    FailingWin::protectionLevel = PROTECTION_LEVEL_ANTIMALWARE_LIGHT;
    WindowsProcessActions actions(FailingWin::functions());

    const ProcessActionResult result = actions.stop(TARGET);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied);
    EXPECT_EQ(result.errorMessage,
              "Process 4242 is a protected process (a system or anti-malware service), which not even an administrator can suspend; "
              "TaskSmack won't suspend it");
    EXPECT_EQ(FailingWin::suspendCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, AProtectedProcessThatRefusesTheSuspendOpenOffersNoRetry)
{
    // Windows refuses PROCESS_SUSPEND_RESUME on a protected process even to an administrator.
    FailingWin::refusedAccess = PROCESS_SUSPEND_RESUME;
    FailingWin::protectionLevel = PROTECTION_LEVEL_WINTCB_LIGHT;
    WindowsProcessActions actions(FailingWin::functions());

    const ProcessActionResult suspended = actions.stop(TARGET);
    EXPECT_FALSE(suspended.success);
    EXPECT_FALSE(suspended.permissionDenied);
    EXPECT_EQ(suspended.errorMessage,
              "Process 4242 is a protected process (a system or anti-malware service), which not even an administrator can suspend");

    const ProcessActionResult resumed = actions.resume(TARGET);
    EXPECT_FALSE(resumed.permissionDenied);
    EXPECT_EQ(resumed.errorMessage,
              "Process 4242 is a protected process (a system or anti-malware service), which not even an administrator can resume");
    EXPECT_EQ(FailingWin::suspendCalls + FailingWin::resumeCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, AnAccessDeniedSuspendOrResumeOffersRetryAsAdministrator)
{
    // An elevated or another account's process: running as administrator could do it.
    FailingWin::refusedAccess = PROCESS_SUSPEND_RESUME;
    WindowsProcessActions actions(FailingWin::functions());

    const ProcessActionResult suspended = actions.stop(TARGET);
    EXPECT_FALSE(suspended.success);
    EXPECT_TRUE(suspended.permissionDenied);
    EXPECT_EQ(suspended.errorMessage, "Access denied: can't suspend process 4242. It may run elevated or as another user.");
    const ProcessActionResult resumed = actions.resume(TARGET);
    EXPECT_TRUE(resumed.permissionDenied);
    EXPECT_EQ(resumed.errorMessage, "Access denied: can't resume process 4242. It may run elevated or as another user.");

    // The call itself refused for want of access says so too.
    FailingWin::refusedAccess = 0;
    FailingWin::suspendStatus = static_cast<LONG>(NT_STATUS_ACCESS_DENIED);
    const ProcessActionResult refusedCall = actions.stop(TARGET);
    EXPECT_TRUE(refusedCall.permissionDenied);
    EXPECT_EQ(refusedCall.errorMessage, "Access denied: can't suspend process 4242");
}

TEST_F(WindowsProcessActionsSeamTest, ASuspendOrResumeWindowsRefusesIsReportedWithItsStatus)
{
    FailingWin::suspendStatus = static_cast<LONG>(NT_STATUS_PROCESS_IS_TERMINATING);
    FailingWin::resumeStatus = static_cast<LONG>(0xC0000001U); // STATUS_UNSUCCESSFUL
    WindowsProcessActions actions(FailingWin::functions());

    const ProcessActionResult suspended = actions.stop(TARGET);
    EXPECT_FALSE(suspended.success);
    EXPECT_FALSE(suspended.permissionDenied);
    EXPECT_EQ(suspended.errorMessage, "Process 4242 is exiting, so it can't be suspended");

    const ProcessActionResult resumed = actions.resume(TARGET);
    EXPECT_FALSE(resumed.success);
    EXPECT_FALSE(resumed.permissionDenied);
    EXPECT_EQ(resumed.errorMessage, "Failed to resume process 4242: NTSTATUS 0xC0000001");
}

TEST_F(WindowsProcessActionsSeamTest, TheSystemProcessAndTaskSmackItselfAreNeverSuspended)
{
    WindowsProcessActions actions(FailingWin::functions());

    const ProcessActionResult system = actions.stop({.pid = 4, .startTimeTicks = FailingWin::TARGET_TICKS});
    EXPECT_EQ(system.errorMessage, "PID 4 is the System process; TaskSmack won't suspend it");
    EXPECT_FALSE(system.permissionDenied);

    // This test process stands in for TaskSmack: refused by its PID before anything is opened, so it
    // is never suspended (which would hang the test, as it would TaskSmack's window).
    const auto self = static_cast<std::int32_t>(GetCurrentProcessId());
    const ProcessActionResult own = actions.stop({.pid = self, .startTimeTicks = FailingWin::TARGET_TICKS});
    EXPECT_FALSE(own.success);
    EXPECT_FALSE(own.permissionDenied);
    EXPECT_EQ(own.errorMessage,
              std::format("Process {} is TaskSmack itself; suspending it would freeze TaskSmack with nothing left to resume it", self));
    // OpenProcess ignores a PID's low two bits, so PID+1 names this process as well.
    EXPECT_FALSE(actions.stop({.pid = self + 1, .startTimeTicks = FailingWin::TARGET_TICKS}).success);
    EXPECT_TRUE(isSamePid(8U, 11U));
    EXPECT_FALSE(isSamePid(8U, 12U));

    EXPECT_EQ(actions.stop({.pid = 0, .startTimeTicks = FailingWin::TARGET_TICKS}).errorMessage, "Invalid PID");
    EXPECT_EQ(FailingWin::opens, 0);
    EXPECT_EQ(FailingWin::suspendCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, WithoutNtSuspendProcessSuspendAndResumeAreUnavailable)
{
    WindowsProcessFunctions functions = FailingWin::functions();
    functions.ntSuspendProcess = nullptr;
    functions.ntResumeProcess = nullptr;
    WindowsProcessActions actions(functions);

    EXPECT_FALSE(actions.actionCapabilities().canStop);
    EXPECT_FALSE(actions.actionCapabilities().canContinue);
    EXPECT_EQ(actions.stop(TARGET).errorMessage, "Suspend is not available on this Windows; nothing sent to process 4242");
    EXPECT_EQ(actions.resume(TARGET).errorMessage, "Resume is not available on this Windows; nothing sent to process 4242");
    EXPECT_EQ(FailingWin::opens, 0);
}

TEST(WindowsProcessFunctionsTest, TheRealFunctionsCheckCriticalityAndProtection)
{
    const WindowsProcessFunctions functions = realWindowsProcessFunctions();
    ASSERT_NE(functions.isProcessCritical, nullptr);
    ASSERT_NE(functions.getProcessInformation, nullptr);

    // Asked of this test process only: it is neither critical nor protected.
    BOOL critical = TRUE;
    ASSERT_NE(functions.isProcessCritical(GetCurrentProcess(), &critical), FALSE);
    EXPECT_EQ(critical, FALSE);
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    ASSERT_NE(functions.getProcessInformation(GetCurrentProcess(), ProcessProtectionLevelInfo, &protection, sizeof(protection)), FALSE);
    EXPECT_EQ(protection.ProtectionLevel, static_cast<DWORD>(PROTECTION_LEVEL_NONE));

    // ntdll's, found without loading anything (#2582); only looked up here, never called.
    EXPECT_NE(functions.ntSuspendProcess, nullptr);
    EXPECT_NE(functions.ntResumeProcess, nullptr);
}

TEST(WindowsProcessFunctionsTest, TheRealCpuSetReaderListsEveryLogicalProcessorOnce)
{
    const WindowsProcessFunctions functions = realWindowsProcessFunctions();
    ASSERT_NE(functions.readCpuSets, nullptr);
    ASSERT_NE(functions.readGroupSizes, nullptr);

    const std::vector<CpuSetRecord> sets = functions.readCpuSets();
    const std::vector<std::uint32_t> groupSizes = functions.readGroupSizes();
    ASSERT_FALSE(sets.empty());
    ASSERT_FALSE(groupSizes.empty());

    std::set<std::uint32_t> ids;
    std::set<std::pair<std::uint16_t, std::uint8_t>> processors;
    for (const CpuSetRecord& set : sets)
    {
        EXPECT_TRUE(ids.insert(set.id).second) << "CPU set " << set.id << " listed twice";
        EXPECT_TRUE(processors.insert({set.group, set.logicalProcessorIndex}).second)
            << "group " << set.group << " processor " << static_cast<int>(set.logicalProcessorIndex) << " listed twice";
        ASSERT_LT(set.group, groupSizes.size());
        EXPECT_LT(set.logicalProcessorIndex, groupSizes[set.group]);
    }
    EXPECT_EQ(sets.size(), static_cast<std::size_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
}

} // namespace Platform
