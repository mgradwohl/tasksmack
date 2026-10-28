#pragma once

#ifdef _WIN32

#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
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
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace Platform
{

/// Maps a Unix-style nice value (-20 to 19) to the closest Windows priority class, using
/// the same threshold boundaries as Domain::Priority::getPriorityLabel. Extracted as a free
/// function so its five threshold branches can be unit tested directly, without spawning or
/// opening a real process. We intentionally never return REALTIME_PRIORITY_CLASS, to avoid
/// system instability.
[[nodiscard]] inline uint32_t niceToPriorityClass(int32_t nice)
{
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
/// for the five classes it sets (#1204). An unknown class (or GetPriorityClass() failing with 0) is
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
    if (action == GuardedAction::Priority || action == GuardedAction::Affinity || action == GuardedAction::EfficiencyMode)
    {
        return std::format("Process {} is a protected process (a system or anti-malware service); Windows won't let even an "
                           "administrator {}",
                           pid,
                           guardedActionWording(action));
    }
    return std::format("Process {} is a protected process (a system or anti-malware service), which not even an administrator can {}; "
                       "TaskSmack won't {}",
                       pid,
                       action == GuardedAction::Suspend ? "suspend" : "end",
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

/// The priority classes Efficiency mode replaced with Idle, per process (#3071), so turning it off
/// restores each. One for the whole TaskSmack process (sharedEfficiencyModePriorities()): the Processes
/// table and Process Details each have their own WindowsProcessActions, and either may turn off what the
/// other turned on. Thread-safe: actions run on the panels' workers. At most MAX_REMEMBERED processes;
/// past that the lowest PID's is dropped, and that process goes back to Normal instead.
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
        if (!m_Classes.contains(key) && m_Classes.size() >= MAX_REMEMBERED)
        {
            m_Classes.erase(m_Classes.begin());
        }
        if (priorityClass == IDLE_PRIORITY_CLASS)
        {
            m_Classes.try_emplace(key, priorityClass);
        }
        else
        {
            m_Classes.insert_or_assign(key, priorityClass);
        }
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
    mutable std::mutex m_Mutex;
    std::map<Key, uint32_t> m_Classes;
};

/// The one EfficiencyModePriorities every WindowsProcessActions in this process shares.
[[nodiscard]] inline EfficiencyModePriorities& sharedEfficiencyModePriorities()
{
    static EfficiencyModePriorities priorities;
    return priorities;
}

} // namespace Platform

#endif // _WIN32
