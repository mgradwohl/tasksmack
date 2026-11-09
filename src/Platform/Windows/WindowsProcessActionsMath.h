#pragma once

#ifdef _WIN32

#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/IoPriorityTypes.h"
#include "Platform/ProcessTypes.h"

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
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Platform
{

/// Maps a Unix-style nice value (-20 to 19) to the closest Windows priority class, using
/// the same threshold boundaries as Domain::Priority::getPriorityLabel. Extracted as a free
/// function so its threshold branches can be unit tested directly, without spawning or
/// opening a real process. MIN_NICE, which Domain::Priority::classNice() gives Realtime and
/// no other class, is REALTIME_PRIORITY_CLASS (#3429): on Windows only Set Realtime priority,
/// behind its warning, sends it; every other value is one of the five classes below it.
[[nodiscard]] inline uint32_t niceToPriorityClass(int32_t nice)
{
    if (nice <= Domain::Priority::MIN_NICE)
    {
        return REALTIME_PRIORITY_CLASS;
    }
    if (nice < Domain::Priority::HIGH_THRESHOLD)
    {
        return HIGH_PRIORITY_CLASS;
    }
    if (nice < Domain::Priority::ABOVE_NORMAL_THRESHOLD)
    {
        return ABOVE_NORMAL_PRIORITY_CLASS;
    }
    if (nice < Domain::Priority::BELOW_NORMAL_THRESHOLD)
    {
        return NORMAL_PRIORITY_CLASS;
    }
    if (nice < Domain::Priority::IDLE_THRESHOLD)
    {
        return BELOW_NORMAL_PRIORITY_CLASS;
    }
    return IDLE_PRIORITY_CLASS;
}

/// Nice values the probe reports for each Windows priority class (#1204): Domain::Priority::classNice(),
/// the table Process Details also selects classes with (#2168).
inline constexpr int32_t IDLE_CLASS_NICE = Domain::Priority::classNice(Domain::Priority::PriorityClass::Idle);
inline constexpr int32_t BELOW_NORMAL_CLASS_NICE = Domain::Priority::classNice(Domain::Priority::PriorityClass::BelowNormal);
inline constexpr int32_t NORMAL_CLASS_NICE = Domain::Priority::classNice(Domain::Priority::PriorityClass::Normal);
inline constexpr int32_t ABOVE_NORMAL_CLASS_NICE = Domain::Priority::classNice(Domain::Priority::PriorityClass::AboveNormal);
inline constexpr int32_t HIGH_CLASS_NICE = Domain::Priority::classNice(Domain::Priority::PriorityClass::High);
inline constexpr int32_t REALTIME_CLASS_NICE = Domain::Priority::classNice(Domain::Priority::PriorityClass::Realtime);

/// The nice value to report for a GetPriorityClass() result: the inverse of niceToPriorityClass()
/// for the six classes it sets (#1204, #3429). An unknown class (or GetPriorityClass() failing with 0) is
/// reported as Normal.
[[nodiscard]] constexpr int32_t priorityClassToNice(uint32_t priorityClass) noexcept
{
    switch (priorityClass)
    {
    case IDLE_PRIORITY_CLASS:
        return IDLE_CLASS_NICE;
    case BELOW_NORMAL_PRIORITY_CLASS:
        return BELOW_NORMAL_CLASS_NICE;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return ABOVE_NORMAL_CLASS_NICE;
    case HIGH_PRIORITY_CLASS:
        return HIGH_CLASS_NICE;
    case REALTIME_PRIORITY_CLASS:
        return REALTIME_CLASS_NICE;
    case NORMAL_PRIORITY_CLASS:
    default:
        return NORMAL_CLASS_NICE;
    }
}

/// The class a GetPriorityClass() result names (#1280), reported beside its nice value so the UI can
/// tell Realtime from High, which share a nice bucket. An unknown class (or GetPriorityClass()
/// failing with 0) is None: the nice value, Normal, then names it.
[[nodiscard]] constexpr PriorityClass toPriorityClass(uint32_t priorityClass) noexcept
{
    switch (priorityClass)
    {
    case IDLE_PRIORITY_CLASS:
        return PriorityClass::Idle;
    case BELOW_NORMAL_PRIORITY_CLASS:
        return PriorityClass::BelowNormal;
    case NORMAL_PRIORITY_CLASS:
        return PriorityClass::Normal;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return PriorityClass::AboveNormal;
    case HIGH_PRIORITY_CLASS:
        return PriorityClass::High;
    case REALTIME_PRIORITY_CLASS:
        return PriorityClass::Realtime;
    default:
        return PriorityClass::None;
    }
}

/// What Set Realtime priority says when Windows quietly gave High instead (#3429): SetPriorityClass()
/// succeeds either way, and only a caller that holds SeIncreaseBasePriorityPrivilege, as an
/// administrator's elevated token does, gets Realtime.
inline constexpr std::string_view REALTIME_NEEDS_ADMIN_MESSAGE = "Windows set High: Realtime needs administrator rights.";

/// The result of a SetPriorityClass(REALTIME_PRIORITY_CLASS) that Windows accepted, from the class
/// read back after it (@p readBack, GetPriorityClass(): 0 when it can't be read) for process @p pid
/// (#3429):
/// - Realtime: done.
/// - High: Windows downgraded it for want of the privilege. A permission refusal, so
///   BrokeredProcessActions sends it through the elevated helper, or the UI offers Retry as
///   administrator. The process stays at High, which is what Windows set.
/// - Unreadable: done, with a note that it wasn't confirmed.
/// - Anything else (the process changed its own class at once): a plain failure naming that class.
[[nodiscard]] inline ProcessActionResult realtimeReadBackResult(uint32_t readBack, int32_t pid)
{
    std::string_view other = "another class";
    switch (readBack)
    {
    case REALTIME_PRIORITY_CLASS:
        return ProcessActionResult::ok();
    case HIGH_PRIORITY_CLASS:
        return ProcessActionResult::denied(std::string{REALTIME_NEEDS_ADMIN_MESSAGE});
    case 0:
        return ProcessActionResult::ok("Windows accepted Realtime, but its class couldn't be read back to confirm it");
    case ABOVE_NORMAL_PRIORITY_CLASS:
        other = "Above Normal";
        break;
    case NORMAL_PRIORITY_CLASS:
        other = "Normal";
        break;
    case BELOW_NORMAL_PRIORITY_CLASS:
        other = "Below Normal";
        break;
    case IDLE_PRIORITY_CLASS:
        other = "Idle";
        break;
    default:
        break;
    }
    return ProcessActionResult::error(std::format("Windows set {} instead of Realtime for process {}", other, pid));
}

/// Whether Terminate's close request goes to this top-level window (#1094): one the target process
/// owns, that is visible, has no owner window, and is not a tool window. Those are the application
/// windows a user would close. Closing an owned window (a dialog), a tool window (a palette or helper,
/// owned or not: WS_EX_TOOLWINDOW is independent of ownership) or an invisible window does not ask the
/// application to exit, and can confuse it.
[[nodiscard]] constexpr bool
isCloseRequestWindow(uint32_t windowPid, uint32_t targetPid, bool visible, bool hasOwner, bool isToolWindow) noexcept
{
    return windowPid == targetPid && visible && !hasOwner && !isToolWindow;
}

/// Terminate's result once its close requests have been posted (#1094). Windows has no SIGTERM:
/// a graceful exit is asked for by closing the process's windows.
///
/// @param eligible    Windows that should be asked (see isCloseRequestWindow).
/// @param failed      Of those, how many the close request could not be queued for -- PostMessage
///                    can be refused, e.g. by UIPI when the target runs elevated.
/// @param firstError  GetLastError() for the first refused request.
/// @return Empty when every eligible window was asked; otherwise why the process was not fully
///         asked. No window at all (a service, a console or background process) means it cannot be
///         asked, only ended with Kill.
[[nodiscard]] inline std::string closeRequestFailure(int32_t pid, int eligible, int failed, uint32_t firstError)
{
    if (eligible <= 0)
    {
        return std::format("Process {} has no window to close, so it cannot be asked to exit; use Kill to end it", pid);
    }
    if (failed <= 0)
    {
        return {};
    }
    if (failed >= eligible)
    {
        return std::format("Could not ask process {} to close: error {} (it may be running elevated); use Kill to end it", pid, firstError);
    }
    return std::format("Asked {} of {} windows of process {} to close; the rest refused the request (error {})",
                       eligible - failed,
                       eligible,
                       pid,
                       firstError);
}

/// An action the critical-process guard covers (#2556), for its refusal's wording.
enum class GuardedAction : std::uint8_t
{
    End,      ///< Kill (TerminateProcess) or Terminate (WM_CLOSE)
    Priority, ///< SetPriorityClass
    Affinity, ///< SetProcessAffinityMask or SetProcessDefaultCpuSets
    Suspend,  ///< NtSuspendProcess (#2582)
    /// SetProcessInformation(ProcessPowerThrottling) and SetPriorityClass (#3071)
    EfficiencyMode,
    SuspendThread, ///< SuspendThread on one of its threads (#3069)
    EndThread,     ///< TerminateThread on one of its threads (#3069)
    IoPriority,    ///< NtSetInformationProcess(ProcessIoPriority) (#3362)
    /// SetTcpEntry(MIB_TCP_STATE_DELETE_TCB) on one of its connections (#3358)
    CloseConnection,
};

/// What TaskSmack won't do, worded to follow "TaskSmack won't".
[[nodiscard]] constexpr const char* guardedActionWording(GuardedAction action) noexcept
{
    switch (action)
    {
    case GuardedAction::Priority:
        return "change its priority";
    case GuardedAction::Affinity:
        return "change its CPU affinity";
    case GuardedAction::Suspend:
        return "suspend it";
    case GuardedAction::EfficiencyMode:
        return "change its Efficiency mode";
    case GuardedAction::SuspendThread:
        return "suspend its threads";
    case GuardedAction::EndThread:
        return "end its threads";
    case GuardedAction::IoPriority:
        return "change its I/O priority";
    case GuardedAction::CloseConnection:
        return "close its connections";
    case GuardedAction::End:
        break;
    }
    return "end it";
}

/// PID 4 is the System process: the kernel's threads.
inline constexpr int32_t SYSTEM_PROCESS_PID = 4;

/// Why @p action is refused for @p pid before anything is opened, or empty (#2556): PID 4 is the System
/// process, the kernel's own, which can't be ended, and changing how it is scheduled starves the whole
/// machine. Not a permission failure: running as administrator doesn't change the answer. PID 0, the
/// System Idle Process, is refused as an invalid PID before this is asked.
[[nodiscard]] inline std::string systemPidRefusal(int32_t pid, GuardedAction action)
{
    if (pid == SYSTEM_PROCESS_PID)
    {
        return std::format("PID 4 is the System process; TaskSmack won't {}", guardedActionWording(action));
    }
    return {};
}

/// @p action refused because @p pid's process is critical (IsProcessCritical, #2556): Windows stops with
/// CRITICAL_PROCESS_DIED when one exits (winlogon.exe, the DcomLaunch svchost.exe), and starving or
/// pinning one can hang logon or the whole session. An administrator, or the elevated helper, could
/// do it, so this is not a permission failure: no password makes it safe.
[[nodiscard]] inline std::string criticalProcessRefusal(int32_t pid, GuardedAction action)
{
    return std::format(
        "Process {} is a critical system process, which Windows can't run without; TaskSmack won't {}", pid, guardedActionWording(action));
}

/// IsProcessCritical() failed with @p error, so whether @p pid's process is critical is unknown (#2556):
/// the action is refused rather than risk ending one.
[[nodiscard]] inline std::string criticalCheckFailure(int32_t pid, uint32_t error)
{
    return std::format("Cannot tell whether process {} is a critical system process: error {}; action not sent", pid, error);
}

/// @p action refused because @p pid's process is protected (PPL or PP, #2556, #2582): an anti-malware
/// service or a system process that Windows keeps even an administrator from ending or suspending, or,
/// when it refuses the open, from re-prioritising or re-pinning (#2602).
[[nodiscard]] inline std::string protectedProcessRefusal(int32_t pid, GuardedAction action = GuardedAction::End)
{
    if (action == GuardedAction::Priority || action == GuardedAction::Affinity || action == GuardedAction::EfficiencyMode ||
        action == GuardedAction::IoPriority || action == GuardedAction::CloseConnection)
    {
        return std::format("Process {} is a protected process (a system or anti-malware service); Windows won't let even an "
                           "administrator {}",
                           pid,
                           guardedActionWording(action));
    }
    return std::format("Process {} is a protected process (a system or anti-malware service), which not even an administrator can {}; "
                       "TaskSmack won't {}",
                       pid,
                       (action == GuardedAction::Suspend || action == GuardedAction::SuspendThread) ? "suspend" : "end",
                       guardedActionWording(action));
}

/// Whether OpenProcess() given @p requested opens the process whose PID is @p actual (#2582). A process
/// ID is a handle in the kernel's client-ID table, whose low two bits are tag bits that a lookup ignores,
/// so OpenProcess(PID + 1) opens PID's process; a PID compared to refuse one process is compared the same.
[[nodiscard]] constexpr bool isSamePid(std::uint32_t requested, std::uint32_t actual) noexcept
{
    constexpr std::uint32_t TAG_BITS = 0x3U;
    return (requested & ~TAG_BITS) == (actual & ~TAG_BITS);
}

/// Suspend refused because @p pid is the process asked to do it (#2582): suspending TaskSmack freezes
/// its window (and the elevated helper, its pipe), with nothing left running to resume it. Not a
/// permission failure: running as administrator doesn't change the answer.
[[nodiscard]] inline std::string selfSuspendRefusal(int32_t pid)
{
    return std::format("Process {} is TaskSmack itself; suspending it would freeze TaskSmack with nothing left to resume it", pid);
}

/// The verb for one thread's @p action (#3069): "suspend", "resume", "end".
[[nodiscard]] constexpr const char* threadActionVerb(ThreadAction action) noexcept
{
    switch (action)
    {
    case ThreadAction::Suspend:
        return "suspend";
    case ThreadAction::Resume:
        return "resume";
    case ThreadAction::End:
        break;
    }
    return "end";
}

/// The guard that covers one thread's @p action in its process (#3069): Suspend and End are the process-level
/// Suspend's and End's (a critical or protected process's threads are never suspended or ended); nullopt for
/// Resume, which, as the process-level Resume, can't stop Windows.
[[nodiscard]] constexpr std::optional<GuardedAction> threadActionGuard(ThreadAction action) noexcept
{
    switch (action)
    {
    case ThreadAction::Suspend:
        return GuardedAction::SuspendThread;
    case ThreadAction::End:
        return GuardedAction::EndThread;
    case ThreadAction::Resume:
        break;
    }
    return std::nullopt;
}

/// Why one thread's @p action is refused before anything is opened (#3069), or empty: a TID that can't be
/// one; the System process's threads (the kernel's, PID 4), for every action; and the threads of
/// @p ownPid (TaskSmack, or in the elevated helper the helper) and of @p refusedPid (the helper's
/// TaskSmack; 0: none), whose suspended or ended thread could freeze or corrupt TaskSmack with nothing
/// left to undo it. PIDs are compared as isSamePid() does. Not permission failures.
[[nodiscard]] inline std::string
threadActionRefusal(std::int32_t pid, std::int32_t tid, ThreadAction action, std::uint32_t ownPid, std::uint32_t refusedPid)
{
    if (tid <= 0)
    {
        return std::format("Thread {} isn't a valid thread ID; nothing sent", tid);
    }
    if (pid == SYSTEM_PROCESS_PID)
    {
        return std::format("PID 4 is the System process, whose threads are the kernel's; TaskSmack won't {} them",
                           threadActionVerb(action));
    }
    const auto requested = static_cast<std::uint32_t>(pid);
    if (pid > 0 && (isSamePid(requested, ownPid) || (refusedPid != 0 && isSamePid(requested, refusedPid))))
    {
        return std::format("Process {} is TaskSmack itself; TaskSmack won't {} its own threads", pid, threadActionVerb(action));
    }
    return {};
}

/// Why the thread @p tid isn't acted on: Windows says it belongs to @p actualPid, not @p pid (#3069). A TID
/// reused by another process's thread since the list was read, or one named for the wrong process.
[[nodiscard]] inline std::string threadNotInProcessRefusal(std::int32_t tid, std::int32_t pid, std::uint32_t actualPid)
{
    if (actualPid == 0)
    {
        return std::format("Cannot confirm that thread {} belongs to process {}; nothing sent", tid, pid);
    }
    return std::format("Thread {} belongs to process {}, not process {}; nothing sent", tid, actualPid, pid);
}

/// Why the thread @p tid of @p pid isn't acted on (#3069): it isn't the thread the user confirmed. @p expected is the
/// creation time the read gave it (0: unknown), @p actual the one GetThreadTimes() gives the thread opened now (0: it
/// couldn't be read). Empty when they are the same, known time.
[[nodiscard]] inline std::string threadIdentityRefusal(std::int32_t tid, std::int32_t pid, std::uint64_t expected, std::uint64_t actual)
{
    if (expected == 0 || actual == 0)
    {
        return std::format("Cannot confirm the identity of thread {} of process {}; nothing sent", tid, pid);
    }
    if (expected != actual)
    {
        return std::format("Thread {} of process {} has exited and its ID now belongs to another thread; nothing sent", tid, pid);
    }
    return {};
}

/// Why opening thread @p tid of @p pid for @p action failed with @p error (#3069). @p isProtected: a protected
/// process refuses even an administrator, so that refusal says so rather than read as a permission failure.
[[nodiscard]] inline std::string
threadOpenFailure(std::uint32_t error, std::int32_t pid, std::int32_t tid, ThreadAction action, bool isProtected)
{
    const char* verb = threadActionVerb(action);
    if (error == ERROR_ACCESS_DENIED && isProtected)
    {
        return std::format("Process {} is a protected process (a system or anti-malware service), which not even an administrator can {} a "
                           "thread of",
                           pid,
                           verb);
    }
    switch (error)
    {
    case ERROR_ACCESS_DENIED:
        return std::format("Access denied: can't {} thread {} of process {}. It may run elevated or as another user.", verb, tid, pid);
    case ERROR_INVALID_PARAMETER:
        return std::format("Thread {} not found - it may have already exited", tid);
    default:
        return std::format("Failed to open thread {} of process {}: error {}", tid, pid, error);
    }
}

/// Why SuspendThread, ResumeThread or TerminateThread (@p action) failed with @p error on thread @p tid (#3069).
[[nodiscard]] inline std::string threadCallFailure(std::uint32_t error, std::int32_t pid, std::int32_t tid, ThreadAction action)
{
    if (error == ERROR_ACCESS_DENIED)
    {
        return std::format("Access denied: can't {} thread {} of process {}", threadActionVerb(action), tid, pid);
    }
    return std::format("Failed to {} thread {} of process {}: error {}", threadActionVerb(action), tid, pid, error);
}

/// The note a successful Suspend or Resume adds to its result line (#3069), from the suspend count before it,
/// @p previousCount (SuspendThread's and ResumeThread's answer): Windows counts suspensions, so a thread
/// suspended twice runs again only after two resumes. Empty for End, and for the plain case (a first suspend,
/// a last resume).
[[nodiscard]] inline std::string threadActionNote(ThreadAction action, std::uint32_t previousCount)
{
    switch (action)
    {
    case ThreadAction::Suspend:
        return previousCount == 0 ? std::string() : std::format("its suspend count is now {}", previousCount + 1);
    case ThreadAction::Resume:
        if (previousCount == 0)
        {
            return "it wasn't suspended";
        }
        if (previousCount == 1)
        {
            return {};
        }
        return std::format("it needs {} more {} to run again", previousCount - 1, previousCount == 2 ? "resume" : "resumes");
    case ThreadAction::End:
        break;
    }
    return {};
}

/// NTSTATUS values NtSuspendProcess and NtResumeProcess return that suspendResumeFailure() words,
/// spelled out since <windows.h> doesn't define them (<ntstatus.h> does).
inline constexpr std::uint32_t NT_STATUS_ACCESS_DENIED = 0xC0000022U;
inline constexpr std::uint32_t NT_STATUS_PROCESS_IS_TERMINATING = 0xC000010AU;

/// Whether NtSuspendProcess or NtResumeProcess refused for want of access (#2582): acting as an
/// administrator could cure it, so the caller offers to retry.
[[nodiscard]] constexpr bool isSuspendResumeAccessDenied(std::uint32_t status) noexcept
{
    return status == NT_STATUS_ACCESS_DENIED;
}

/// Why NtSuspendProcess (@p suspend) or NtResumeProcess failed with @p status for @p pid (#2582).
[[nodiscard]] inline std::string suspendResumeFailure(std::uint32_t status, int32_t pid, bool suspend)
{
    const char* verb = suspend ? "suspend" : "resume";
    switch (status)
    {
    case NT_STATUS_ACCESS_DENIED:
        return std::format("Access denied: can't {} process {}", verb, pid);
    case NT_STATUS_PROCESS_IS_TERMINATING:
        return std::format("Process {} is exiting, so it can't be {}", pid, suspend ? "suspended" : "resumed");
    default:
        return std::format("Failed to {} process {}: NTSTATUS 0x{:08X}", verb, pid, status);
    }
}

/// Why opening @p pid's process to suspend (@p suspend) or resume it failed with @p error (#2582).
/// @p isProtected: Windows refuses PROCESS_SUSPEND_RESUME on a protected process even to an
/// administrator, so a refusal there says so rather than leave it to look like a permission failure.
[[nodiscard]] inline std::string suspendResumeOpenFailure(std::uint32_t error, int32_t pid, bool suspend, bool isProtected)
{
    const char* verb = suspend ? "suspend" : "resume";
    if (error == ERROR_ACCESS_DENIED && isProtected)
    {
        return std::format(
            "Process {} is a protected process (a system or anti-malware service), which not even an administrator can {}", pid, verb);
    }
    switch (error)
    {
    case ERROR_ACCESS_DENIED:
        return std::format("Access denied: can't {} process {}. It may run elevated or as another user.", verb, pid);
    case ERROR_INVALID_PARAMETER:
        return "Process not found - may have already exited";
    default:
        return std::format("Failed to open process {}: error {}", pid, error);
    }
}

/// Set affinity refused for lack of access (#1881): a protected process (anti-malware, DRM, some
/// system services) refuses even an administrator, and an elevated or another user's process needs
/// TaskSmack to run elevated.
[[nodiscard]] inline std::string affinityAccessDeniedMessage(int32_t pid)
{
    return std::format("Access denied: can't change the CPU affinity of process {}. It may be protected, or run elevated or as another "
                       "user (run TaskSmack as administrator for those).",
                       pid);
}

/// @p action refused with ERROR_ACCESS_DENIED for a process that isn't protected (#2602): one running
/// elevated or as another user, which running as administrator can act on, so the caller offers to retry.
[[nodiscard]] inline std::string accessDeniedMessage(int32_t pid, GuardedAction action)
{
    switch (action)
    {
    case GuardedAction::Priority:
        return std::format("Access denied: can't change the priority of process {}. It may run elevated or as another user.", pid);
    case GuardedAction::Affinity:
        return affinityAccessDeniedMessage(pid);
    case GuardedAction::Suspend:
        return std::format("Access denied: can't suspend process {}. It may run elevated or as another user.", pid);
    case GuardedAction::EfficiencyMode:
        return std::format("Access denied: can't change the Efficiency mode of process {}. It may run elevated or as another user.", pid);
    case GuardedAction::SuspendThread:
        return std::format("Access denied: can't suspend the threads of process {}. It may run elevated or as another user.", pid);
    case GuardedAction::EndThread:
        return std::format("Access denied: can't end the threads of process {}. It may run elevated or as another user.", pid);
    case GuardedAction::IoPriority:
        return std::format("Access denied: can't change the I/O priority of process {}. It may run elevated or as another user.", pid);
    case GuardedAction::CloseConnection:
        return std::format("Access denied: can't close the connections of process {}. It may run elevated or as another user.", pid);
    case GuardedAction::End:
        break;
    }
    return std::format("Access denied: can't end process {}. It may run elevated or as another user.", pid);
}

/// Why opening @p pid's process for Kill, Terminate, a priority or an affinity change (@p action) failed
/// with @p error (#2602). @p isProtected: Windows refuses a protected process even to an administrator,
/// so that refusal says so, rather than read as one running as administrator would cure.
[[nodiscard]] inline std::string actionOpenFailure(std::uint32_t error, int32_t pid, GuardedAction action, bool isProtected)
{
    if (error == ERROR_ACCESS_DENIED)
    {
        return isProtected ? protectedProcessRefusal(pid, action) : accessDeniedMessage(pid, action);
    }
    if (error == ERROR_INVALID_PARAMETER)
    {
        return "Process not found - may have already exited";
    }
    return std::format("Failed to open process {}: error {}", pid, error);
}

// --- I/O priority (#3362) ------------------------------------------------------------------------------------

/// ProcessIoPriority: the PROCESSINFOCLASS NtQueryInformationProcess and NtSetInformationProcess read and set a
/// process's I/O priority hint with, as a ULONG IO_PRIORITY_HINT. winternl.h leaves it out.
inline constexpr ULONG PROCESS_IO_PRIORITY_CLASS = 33;

/// The NTSTATUS values the I/O priority calls return that ioPrioritySetResult() words, spelled out since
/// <windows.h> doesn't define them (<ntstatus.h> does). NT_STATUS_ACCESS_DENIED is above.
inline constexpr std::uint32_t NT_STATUS_PRIVILEGE_NOT_HELD = 0xC0000061U;
inline constexpr std::uint32_t NT_STATUS_INVALID_PARAMETER = 0xC000000DU;

/// The hint's name in a message: "Very low", "Low", "Normal", "High", "Critical".
[[nodiscard]] constexpr std::string_view ioPriorityHintName(IoPriorityHint hint) noexcept
{
    switch (hint)
    {
    case IoPriorityHint::VeryLow:
        return "Very low";
    case IoPriorityHint::Low:
        return "Low";
    case IoPriorityHint::High:
        return "High";
    case IoPriorityHint::Critical:
        return "Critical";
    case IoPriorityHint::Normal:
        break;
    }
    return "Normal";
}

/// The hint setIoPriority() sends for @p ioClass and @p level, or why nothing is sent to @p pid: Windows has only
/// the WindowsHint class, so Linux's classes are refused, as is a level Windows doesn't define; and Critical,
/// which is the memory manager's own (paging I/O), is never set: Windows refuses it to a caller outside the
/// kernel, and a process at Critical would compete with the system's own paging.
[[nodiscard]] inline std::expected<IoPriorityHint, std::string>
ioPriorityHintToSet(IoPriorityClass ioClass, std::int32_t level, int32_t pid)
{
    const std::optional<IoPriorityHint> hint = ioPriorityHintFromLevel(level);
    if (ioClass != IoPriorityClass::WindowsHint || !hint.has_value())
    {
        return std::unexpected(std::format(
            "Windows' I/O priority is Very low, Low, Normal or High; that setting isn't one, so nothing was sent to process {}", pid));
    }
    if (*hint == IoPriorityHint::Critical)
    {
        return std::unexpected(std::format("Critical I/O priority is the system's own; TaskSmack won't set it for process {}", pid));
    }
    return *hint;
}

/// The I/O priority NtQueryInformationProcess(ProcessIoPriority) read for @p pid: @p status, and @p value, the
/// IO_PRIORITY_HINT it wrote. A hint Windows doesn't define is an error, never guessed at.
[[nodiscard]] inline IoPriorityReadResult ioPriorityReadResult(std::uint32_t status, ULONG value, int32_t pid)
{
    if ((status & 0x80000000U) != 0) // NT_SUCCESS() is a non-negative NTSTATUS
    {
        if (status == NT_STATUS_ACCESS_DENIED)
        {
            return std::unexpected(ioPriorityReadDeniedMessage(pid));
        }
        return std::unexpected(std::format("Failed to read the I/O priority of process {}: NTSTATUS 0x{:08X}", pid, status));
    }
    const std::optional<IoPriorityHint> hint =
        (value <= static_cast<ULONG>(IoPriorityHint::Critical)) ? ioPriorityHintFromLevel(static_cast<std::int32_t>(value)) : std::nullopt;
    if (!hint.has_value())
    {
        return std::unexpected(std::format("Process {} has an I/O priority Windows doesn't define ({})", pid, value));
    }
    return IoPriority{.ioClass = IoPriorityClass::WindowsHint, .level = static_cast<std::int32_t>(*hint)};
}

/// What setting @p pid's I/O priority to @p hint says, from NtSetInformationProcess's @p status. Privilege not
/// held (High without SeIncreaseBasePriorityPrivilege) and access denied are permission refusals, so
/// BrokeredProcessActions retries them through the elevated helper, or the UI offers Retry as administrator.
/// @p isProtected: access denied on a protected process is a plain error, since it refuses an administrator too.
[[nodiscard]] inline ProcessActionResult ioPrioritySetResult(std::uint32_t status, int32_t pid, IoPriorityHint hint, bool isProtected)
{
    if ((status & 0x80000000U) == 0)
    {
        return ProcessActionResult::ok();
    }
    switch (status)
    {
    case NT_STATUS_PRIVILEGE_NOT_HELD:
        return ProcessActionResult::denied(
            std::format("{} I/O priority needs administrator rights (SeIncreaseBasePriorityPrivilege); process {} wasn't changed",
                        ioPriorityHintName(hint),
                        pid));
    case NT_STATUS_ACCESS_DENIED:
        return isProtected ? ProcessActionResult::error(protectedProcessRefusal(pid, GuardedAction::IoPriority))
                           : ProcessActionResult::denied(accessDeniedMessage(pid, GuardedAction::IoPriority));
    case NT_STATUS_INVALID_PARAMETER:
        return ProcessActionResult::error(std::format("Windows refused {} I/O priority for process {}", ioPriorityHintName(hint), pid));
    default:
        break;
    }
    return ProcessActionResult::error(std::format("Failed to set the I/O priority of process {}: NTSTATUS 0x{:08X}", pid, status));
}

/// Whether Terminate's close requests were all refused for want of access (#2602): UIPI keeps a process
/// from posting to an elevated process's windows, which the elevated helper can post to, so the caller
/// offers to retry. Some asked and some refused is not retried: the windows already asked would be
/// asked twice.
[[nodiscard]] constexpr bool isCloseRequestAccessDenied(int eligible, int failed, std::uint32_t firstError) noexcept
{
    return eligible > 0 && failed >= eligible && firstError == ERROR_ACCESS_DENIED;
}

/// The message when SetProcessAffinityMask() (or, with @p cpuSets, SetProcessDefaultCpuSets()) fails
/// with @p error for process @p pid (#1881).
[[nodiscard]] inline std::string affinitySetErrorMessage(uint32_t error, int32_t pid, bool cpuSets)
{
    switch (error)
    {
    case ERROR_ACCESS_DENIED:
        return affinityAccessDeniedMessage(pid);
    case ERROR_INVALID_PARAMETER:
        return cpuSets ? std::format("Windows refused those CPU sets for process {}", pid)
                       : std::format("Windows refused those CPUs for process {}: they must be in its processor group, and a process "
                                     "whose threads run in several groups can't be given one mask",
                                     pid);
    default:
        return std::format("Failed to set the CPU affinity of process {}: error {}", pid, error);
    }
}

/// Why SetProcessInformation(ProcessPowerThrottling) failed with @p error, turning Efficiency mode on
/// (@p on) or off for @p pid (#3071). Nothing was changed.
[[nodiscard]] inline std::string efficiencyModeFailure(uint32_t error, int32_t pid, bool on)
{
    return std::format("Failed to turn Efficiency mode {} for process {}: error {}", on ? "on" : "off", pid, error);
}

/// Why SetPriorityClass failed with @p error for Efficiency mode (#3071): turning it on (@p on), the power
/// throttling was undone again, so nothing changed; turning it off, the throttling is off but the priority
/// it had before wasn't restored.
[[nodiscard]] inline std::string efficiencyModePriorityFailure(uint32_t error, int32_t pid, bool on)
{
    if (on)
    {
        return std::format("Could not lower the priority of process {} to Idle: error {}; Efficiency mode was not turned on", pid, error);
    }
    return std::format("Efficiency mode was turned off for process {}, but its priority could not be restored: error {}", pid, error);
}

/// The priority class to set when Efficiency mode is turned off (#3071), from the class @p remembered when
/// it was turned on (nullopt: unknown) and the process's class now, @p current (0: unreadable). Efficiency
/// mode left it at Idle: back to the remembered class, or Normal when that isn't known. At any other
/// class someone changed it since, so it is left as it is (nullopt).
[[nodiscard]] constexpr std::optional<uint32_t> priorityClassToRestore(std::optional<uint32_t> remembered, uint32_t current) noexcept
{
    if (current != 0 && current != IDLE_PRIORITY_CLASS)
    {
        return std::nullopt;
    }
    return remembered.value_or(static_cast<uint32_t>(NORMAL_PRIORITY_CLASS));
}

/// Whether @p priorityClass is one of Windows' six priority classes (#3327): the only values remembered for an
/// Efficiency mode off to restore, whoever names them (an off's request to the helper carries one).
[[nodiscard]] constexpr bool isPriorityClass(uint32_t priorityClass) noexcept
{
    switch (priorityClass)
    {
    case IDLE_PRIORITY_CLASS:
    case BELOW_NORMAL_PRIORITY_CLASS:
    case NORMAL_PRIORITY_CLASS:
    case ABOVE_NORMAL_PRIORITY_CLASS:
    case HIGH_PRIORITY_CLASS:
    case REALTIME_PRIORITY_CLASS:
        return true;
    default:
        return false;
    }
}

/// The ProcessActionResult::successNote for turning Efficiency mode off (#3281), from the class
/// @p remembered when it was turned on and the class @p restored (priorityClassToRestore()). With nothing
/// remembered, by this TaskSmack or by the helper that ran the on (#3327: TaskSmack restarted since, or the
/// class couldn't be read), the process was put at Normal rather than the class it had before, which may
/// differ; the note says so. Empty otherwise: the remembered class was restored, or the class was left as
/// someone changed it.
[[nodiscard]] constexpr std::string_view efficiencyModeOffNote(std::optional<uint32_t> remembered,
                                                               std::optional<uint32_t> restored) noexcept
{
    if (remembered.has_value() || !restored.has_value())
    {
        return {};
    }
    return "its priority was set to Normal, as the class it had before Efficiency mode isn't known";
}

/// The priority classes Efficiency mode replaced with Idle, per process (#3071), so turning it off
/// restores each. Keyed on the PID and the start time, so a process given a reused PID inherits nothing.
/// One for the whole TaskSmack process (sharedEfficiencyModePriorities()): the Processes table and Process
/// Details each have their own WindowsProcessActions, and either may turn off what the other turned on.
/// TaskSmack's also holds the class of an on the elevated helper ran, from its reply, and sends it with an
/// off the helper finishes (#3327), so the class survives the helper's exit. Thread-safe: actions run on
/// the panels' workers. At most MAX_REMEMBERED processes; past that the lowest PID's is dropped, and that
/// process goes back to Normal instead. Not kept across a restart: an off then puts the process at Normal
/// and says so (efficiencyModeOffNote(), #3281).
class EfficiencyModePriorities
{
  public:
    static constexpr std::size_t MAX_REMEMBERED = 1024;

    /// Efficiency mode was turned on for @p target, whose class was @p priorityClass (0: unreadable, not
    /// remembered). A class other than Idle replaces any remembered before; Idle (Efficiency mode turned
    /// on again, or a process already put at Idle) keeps one already remembered.
    void remember(const ProcessTarget& target, uint32_t priorityClass)
    {
        if (priorityClass == 0)
        {
            return;
        }
        const Key key{target.pid, target.startTimeTicks};
        const std::scoped_lock lock(m_Mutex);
        makeRoomFor(key);
        if (priorityClass == IDLE_PRIORITY_CLASS)
        {
            m_Classes.try_emplace(key, priorityClass);
        }
        else
        {
            m_Classes.insert_or_assign(key, priorityClass);
        }
    }

    /// @p priorityClass is what @p target's off restores, whatever was remembered before (#3327): the class an off's
    /// request to the helper carries, TaskSmack's, which knows of every on since. Unlike remember(), Idle replaces a
    /// class too: TaskSmack's on found the process at Idle, so a class this helper kept from an earlier on is stale.
    /// 0 (unreadable) remembers nothing.
    void assign(const ProcessTarget& target, uint32_t priorityClass)
    {
        if (priorityClass == 0)
        {
            return;
        }
        const Key key{target.pid, target.startTimeTicks};
        const std::scoped_lock lock(m_Mutex);
        makeRoomFor(key);
        m_Classes.insert_or_assign(key, priorityClass);
    }

    /// The class remembered for @p target, still remembered (#3327: sent with an off the helper finishes), or nullopt.
    [[nodiscard]] std::optional<uint32_t> find(const ProcessTarget& target) const
    {
        const std::scoped_lock lock(m_Mutex);
        const auto found = m_Classes.find(Key{target.pid, target.startTimeTicks});
        if (found == m_Classes.end())
        {
            return std::nullopt;
        }
        return found->second;
    }

    /// The class remembered for @p target, forgotten now (Efficiency mode is being turned off), or nullopt.
    [[nodiscard]] std::optional<uint32_t> take(const ProcessTarget& target)
    {
        const std::scoped_lock lock(m_Mutex);
        const auto found = m_Classes.find(Key{target.pid, target.startTimeTicks});
        if (found == m_Classes.end())
        {
            return std::nullopt;
        }
        const uint32_t priorityClass = found->second;
        m_Classes.erase(found);
        return priorityClass;
    }

    [[nodiscard]] std::size_t size() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Classes.size();
    }

  private:
    using Key = std::pair<int32_t, uint64_t>; // PID, start time

    /// Drops the lowest PID's class when @p key is new and MAX_REMEMBERED are held. m_Mutex is held.
    void makeRoomFor(const Key& key)
    {
        if (!m_Classes.contains(key) && m_Classes.size() >= MAX_REMEMBERED)
        {
            m_Classes.erase(m_Classes.begin());
        }
    }

    mutable std::mutex m_Mutex;
    std::map<Key, uint32_t> m_Classes;
};

/// The one EfficiencyModePriorities every WindowsProcessActions in this process shares, unless a test gives one its own
/// (a TaskSmack's and a helper's, #3327).
[[nodiscard]] inline const std::shared_ptr<EfficiencyModePriorities>& sharedEfficiencyModePriorities()
{
    static const std::shared_ptr<EfficiencyModePriorities> priorities = std::make_shared<EfficiencyModePriorities>();
    return priorities;
}

} // namespace Platform

#endif // _WIN32
