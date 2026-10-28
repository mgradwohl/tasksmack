#include "WindowsProcessActions.h"

#include "Domain/PriorityConfig.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "WindowsAffinityMath.h"
#include "WindowsFolderPicker.h"
#include "WindowsHandles.h"
#include "WindowsNtQuery.h"
#include "WindowsProcessActionsMath.h"
#include "WindowsProcessDump.h"
#include "WindowsProcessDumpMath.h"
#include "WindowsProcessIdentity.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{

// TaskSmackHelper.exe builds this file with TASKSMACK_NO_PROCESS_DUMPS (CMakeLists.txt, #2557): the
// helper never serves a dump or a folder dialog, so the dump writer (dbghelp.dll) and the COM folder
// picker, which loads shell extensions in-process, stay out of the elevated image. There the three dump
// members are IProcessActions' defaults, which refuse as unsupported.
#ifdef TASKSMACK_NO_PROCESS_DUMPS
constexpr bool HAS_DUMPS = false;
#else
constexpr bool HAS_DUMPS = true;
#endif

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

/// A process handle that has passed the identity check, or the reason there is none.
struct VerifiedProcess
{
    Windows::InjectableHandle handle;
    ProcessActionResult result;
};

/// Open `target` with `access` and confirm it is the process the target names, through
/// Windows::openProcessVerified() (the check every reader shares, #2149), and say why not.
///
/// @param describeOpenFailure  Turns an OpenProcess error code into the message to show.
template<typename DescribeOpenFailure>
[[nodiscard]] VerifiedProcess
openVerified(const WindowsProcessFunctions& fns, const ProcessTarget& target, DWORD access, DescribeOpenFailure describeOpenFailure)
{
    // Closed with the real CloseHandle: this seam's fakes hand out real handles.
    auto opened = Windows::openProcessVerified(
        {.openProcess = fns.openProcess, .getProcessTimes = fns.getProcessTimes, .closeHandle = &::CloseHandle}, target, access);
    if (opened)
    {
        return {.handle = std::move(*opened), .result = ProcessActionResult::ok()};
    }

    const Windows::IdentityFailure failure = opened.error();
    switch (failure.kind)
    {
    case Windows::IdentityFailureKind::InvalidPid:
        return {.handle = {}, .result = ProcessActionResult::error("Invalid PID")};
    case Windows::IdentityFailureKind::IdentityUnknown:
    case Windows::IdentityFailureKind::Mismatch:
        // The start time this PID's process has is not the target's (or the target's is unknown):
        // checkProcessIdentity() words both, given any time other than the target's.
        return {.handle = {}, .result = checkProcessIdentity(target, 0)};
    case Windows::IdentityFailureKind::AccessDenied:
    case Windows::IdentityFailureKind::ProcessGone:
    case Windows::IdentityFailureKind::OpenFailed:
        return {.handle = {}, .result = ProcessActionResult::error(describeOpenFailure(failure.win32Error))};
    case Windows::IdentityFailureKind::Exited:
        return {.handle = {}, .result = ProcessActionResult::error(std::format("Process {} has exited; action not sent", target.pid))};
    case Windows::IdentityFailureKind::TimesFailed:
        break;
    }
    return {
        .handle = {},
        .result = ProcessActionResult::error(
            std::format("Cannot confirm the identity of process {}: error {}; action not sent", target.pid, failure.win32Error)),
    };
}

/// EnumWindows state for Terminate: the process whose windows to close, and how many were asked.
struct CloseRequest
{
    DWORD pid = 0;
    int eligible = 0;     ///< Windows that should be asked
    int failed = 0;       ///< Of those, how many PostMessage refused
    DWORD firstError = 0; ///< GetLastError() for the first refusal
};

/// Posts WM_CLOSE to each of the target's top-level windows that a user would close (see
/// isCloseRequestWindow). PostMessage, not SendMessage: the target may show a "save changes?"
/// prompt, and TaskSmack must not wait on it.
BOOL CALLBACK postCloseToProcessWindow(HWND window, LPARAM param)
{
    auto* request = reinterpret_cast<CloseRequest*>(param); // NOLINT(performance-no-int-to-ptr) - EnumWindows' LPARAM
    DWORD windowPid = 0;
    GetWindowThreadProcessId(window, &windowPid);
    const bool isToolWindow = (static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)) & WS_EX_TOOLWINDOW) != 0;
    if (!isCloseRequestWindow(windowPid, request->pid, IsWindowVisible(window) != 0, GetWindow(window, GW_OWNER) != nullptr, isToolWindow))
    {
        return TRUE;
    }
    ++request->eligible;
    // GetLastError() straight after the refused PostMessage, before any other call can reset it.
    if (PostMessageW(window, WM_CLOSE, 0, 0) == 0 && request->failed++ == 0)
    {
        request->firstError = GetLastError();
    }
    return TRUE;
}

std::vector<std::uint32_t> realReadGroupSizes()
{
    const WORD groups = GetMaximumProcessorGroupCount();
    std::vector<std::uint32_t> sizes;
    sizes.reserve(groups);
    WORD group = 0;
    while (group < groups)
    {
        sizes.push_back(GetMaximumProcessorCount(group));
        ++group;
    }
    return sizes;
}

std::vector<ProcessorCoreRecord> realReadCores()
{
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) != FALSE || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        length == 0)
    {
        return {};
    }
    std::vector<std::byte> buffer(length);
    if (GetLogicalProcessorInformationEx(
            RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length) == FALSE)
    {
        return {};
    }
    const std::size_t total = std::min<std::size_t>(length, buffer.size());
    std::vector<ProcessorCoreRecord> cores;
    constexpr std::size_t RECORD_HEADER_BYTES = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
    std::size_t offset = 0;
    while (offset + RECORD_HEADER_BYTES <= total)
    {
        // Copied out rather than cast in place, as WindowsSystemProbe does: records are variable-sized.
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX record{};
        const std::size_t available = total - offset;
        std::memcpy(&record, &buffer[offset], std::min(available, sizeof(record)));
        if (record.Size == 0 || record.Size > available)
        {
            break;
        }
        // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - Relationship selects the union member
        if (record.Relationship == RelationProcessorCore && record.Processor.GroupCount > 0)
        {
            // A core's processors are in one group, so its first GroupMask is all of them.
            cores.push_back({.group = record.Processor.GroupMask[0].Group,
                             .mask = static_cast<std::uint64_t>(record.Processor.GroupMask[0].Mask),
                             .efficiencyClass = record.Processor.EfficiencyClass});
        }
        // NOLINTEND(cppcoreguidelines-pro-type-union-access)
        offset += record.Size;
    }
    return cores;
}

std::vector<CpuSetRecord> realReadCpuSets()
{
    ULONG length = 0;
    if (GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0) != FALSE || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        length == 0)
    {
        return {};
    }
    std::vector<std::byte> buffer(length);
    if (GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length, &length, GetCurrentProcess(), 0) ==
        FALSE)
    {
        return {};
    }
    const std::size_t total = std::min<std::size_t>(length, buffer.size());
    std::vector<CpuSetRecord> sets;
    constexpr std::size_t RECORD_HEADER_BYTES = offsetof(SYSTEM_CPU_SET_INFORMATION, CpuSet);
    std::size_t offset = 0;
    while (offset + RECORD_HEADER_BYTES <= total)
    {
        SYSTEM_CPU_SET_INFORMATION record{};
        const std::size_t available = total - offset;
        std::memcpy(&record, &buffer[offset], std::min(available, sizeof(record)));
        if (record.Size == 0 || record.Size > available)
        {
            break;
        }
        // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - Type selects the union member
        if (record.Type == CpuSetInformation)
        {
            sets.push_back(
                {.id = record.CpuSet.Id, .group = record.CpuSet.Group, .logicalProcessorIndex = record.CpuSet.LogicalProcessorIndex});
        }
        // NOLINTEND(cppcoreguidelines-pro-type-union-access)
        offset += record.Size;
    }
    return sets;
}

/// Whether `process` is protected (PPL or PP), through the seam; false when that can't be read.
[[nodiscard]] bool isProtected(const WindowsProcessFunctions& fns, HANDLE process)
{
    if (fns.getProcessInformation == nullptr)
    {
        return false;
    }
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    if (fns.getProcessInformation(process, ProcessProtectionLevelInfo, &protection, sizeof(protection)) == 0)
    {
        return false;
    }
    return protection.ProtectionLevel != PROTECTION_LEVEL_NONE;
}

/// Whether `pid`'s process is protected, to word a refused open: a protected process still lets
/// anyone open it for a limited query. Not identity-checked, since it decides no action, only how one
/// Windows already refused is explained.
[[nodiscard]] bool isProtectedPid(const WindowsProcessFunctions& fns, std::int32_t pid)
{
    if (fns.openProcess == nullptr)
    {
        return false;
    }
    const Windows::UniqueHandle process(fns.openProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
    return process && isProtected(fns, process.get());
}

/// openVerified() for an action an administrator can do (#2602): an access-denied open is
/// permissionDenied, so BrokeredProcessActions retries it through a running elevated helper, or the UI
/// offers "Retry as administrator". Not for a protected process, which Windows refuses even an
/// administrator: that refusal is a plain error, worded by @p describeOpenFailure.
///
/// @param describeOpenFailure  (DWORD error, bool isProtected) -> std::string: the message to show for
///                             an OpenProcess error; isProtected only ever true with ERROR_ACCESS_DENIED.
template<typename DescribeOpenFailure>
[[nodiscard]] VerifiedProcess
openForAction(const WindowsProcessFunctions& fns, const ProcessTarget& target, DWORD access, DescribeOpenFailure describeOpenFailure)
{
    bool retryable = false;
    VerifiedProcess process = openVerified(fns,
                                           target,
                                           access,
                                           [&fns, &target, &retryable, &describeOpenFailure](DWORD error)
                                           {
                                               const bool isProtected = error == ERROR_ACCESS_DENIED && isProtectedPid(fns, target.pid);
                                               retryable = error == ERROR_ACCESS_DENIED && !isProtected;
                                               return describeOpenFailure(error, isProtected);
                                           });
    process.result.permissionDenied = !process.result.success && retryable;
    return process;
}

/// The failure, logged, of @p action's Win32 call on the verified @p process with @p error (#2602):
/// ERROR_ACCESS_DENIED is permissionDenied, which running as administrator could cure, unless the process
/// is protected, which refuses an administrator too; any other error is a plain error, @p otherMessage.
[[nodiscard]] ProcessActionResult callFailure(const WindowsProcessFunctions& fns,
                                              HANDLE process,
                                              const ProcessTarget& target,
                                              GuardedAction action,
                                              DWORD error,
                                              std::string otherMessage)
{
    ProcessActionResult result = ProcessActionResult::error(std::move(otherMessage));
    if (error == ERROR_ACCESS_DENIED)
    {
        result = isProtected(fns, process) ? ProcessActionResult::error(protectedProcessRefusal(target.pid, action))
                                           : ProcessActionResult::denied(accessDeniedMessage(target.pid, action));
    }
    spdlog::warn("{}", result.errorMessage);
    return result;
}

/// The refusal for PID 4, the System process (#2556), logged, or nullopt for any other PID.
[[nodiscard]] std::optional<ProcessActionResult> systemPidResult(const ProcessTarget& target, GuardedAction action)
{
    std::string refusal = systemPidRefusal(target.pid, action);
    if (refusal.empty())
    {
        return std::nullopt;
    }
    spdlog::warn("{}", refusal);
    return ProcessActionResult::error(std::move(refusal));
}

/// Why `action` must not reach the verified `process` (#2556), logged, or nullopt: a protected process
/// is never ended or suspended, and a critical one (or one whose criticality can't be read) never
/// ended, suspended, re-prioritised or re-pinned. Errors, not permissionDenied: no administrator
/// password makes them safe.
[[nodiscard]] std::optional<ProcessActionResult>
guardRefusal(const WindowsProcessFunctions& fns, HANDLE process, const ProcessTarget& target, GuardedAction action)
{
    std::string refusal;
    BOOL critical = FALSE;
    const bool refusesProtected =
        action == GuardedAction::End || action == GuardedAction::Suspend || action == GuardedAction::EfficiencyMode;
    if (refusesProtected && isProtected(fns, process))
    {
        refusal = protectedProcessRefusal(target.pid, action);
    }
    else if (fns.isProcessCritical == nullptr)
    {
        refusal = criticalCheckFailure(target.pid, ERROR_CALL_NOT_IMPLEMENTED);
    }
    else if (fns.isProcessCritical(process, &critical) == 0)
    {
        refusal = criticalCheckFailure(target.pid, GetLastError());
    }
    else if (critical != FALSE)
    {
        refusal = criticalProcessRefusal(target.pid, action);
    }
    if (refusal.empty())
    {
        return std::nullopt;
    }
    spdlog::warn("{}", refusal);
    return ProcessActionResult::error(std::move(refusal));
}

/// Whether the table has every call Efficiency mode makes (#3071).
[[nodiscard]] bool hasEfficiencyMode(const WindowsProcessFunctions& fns) noexcept
{
    return fns.getPriorityClass != nullptr && fns.setPriorityClass != nullptr && fns.setProcessInformation != nullptr;
}

/// Sets @p process's execution-speed power throttling (EcoQoS) on, or hands it back to Windows (off: no
/// mechanism controlled), through the seam; FALSE with GetLastError() set when refused.
[[nodiscard]] BOOL setExecutionSpeedThrottling(const WindowsProcessFunctions& fns, HANDLE process, bool on)
{
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    const ULONG mask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0U;
    state.ControlMask = mask;
    state.StateMask = mask;
    return fns.setProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
}

} // namespace

WindowsProcessFunctions realWindowsProcessFunctions() noexcept
{
    return {
        .openProcess = &OpenProcess,
        .getProcessTimes = &GetProcessTimes,
        .setProcessAffinityMask = &SetProcessAffinityMask,
        .setProcessDefaultCpuSets = &SetProcessDefaultCpuSets,
        .readGroupSizes = &realReadGroupSizes,
        .readCores = &realReadCores,
        .readCpuSets = &realReadCpuSets,
        .pssCaptureSnapshot = &PssCaptureSnapshot,
        .isProcessCritical = &IsProcessCritical,
        .getProcessInformation = &GetProcessInformation,
        .ntSuspendProcess = Windows::ntSuspendProcess(),
        .ntResumeProcess = Windows::ntResumeProcess(),
        .getCurrentProcessId = &GetCurrentProcessId,
        .getPriorityClass = &GetPriorityClass,
        .setPriorityClass = &SetPriorityClass,
        .setProcessInformation = &SetProcessInformation,
    };
}

WindowsProcessActions::WindowsProcessActions() : WindowsProcessActions(realWindowsProcessFunctions())
{}

WindowsProcessActions::WindowsProcessActions(WindowsProcessFunctions functions)
    : m_Fns(functions), m_GroupSizes((m_Fns.readGroupSizes != nullptr) ? m_Fns.readGroupSizes() : std::vector<std::uint32_t>{})
{}

ProcessActionCapabilities WindowsProcessActions::actionCapabilities() const
{
    return ProcessActionCapabilities{
        .canTerminate = true,                            // WM_CLOSE to the process's windows: a request it may answer (#1094)
        .canKill = true,                                 // TerminateProcess
        .canStop = m_Fns.ntSuspendProcess != nullptr,    // NtSuspendProcess (#2582)
        .canContinue = m_Fns.ntResumeProcess != nullptr, // NtResumeProcess (#2582)
        .canSetPriority = true,                          // SetPriorityClass
        .canSetIoPriority = false,                       // No ionice equivalent (#803)
        .canSetRealtimeIoPriority = false,
        // SetProcessAffinityMask, or CPU sets across processor groups (#1881); needs the groups' sizes
        .canSetAffinity = !m_GroupSizes.empty(),
        .syscallTrace = SyscallTraceAvailability::Unsupported,
        .canCreateDump = HAS_DUMPS,                       // MiniDumpWriteDump, dbghelp.dll loaded on the first dump (#807)
        .canChooseDumpFolder = HAS_DUMPS,                 // IFileOpenDialog with FOS_PICKFOLDERS (#2001)
        .dumpsFromSnapshot = HAS_DUMPS,                   // PssCaptureSnapshot, else from the running process (#2001)
        .canAuthorizeInteractively = false,               // BrokeredProcessActions sets it when TaskSmackHelper.exe can be used (#2550)
        .canSetEfficiencyMode = hasEfficiencyMode(m_Fns), // EcoQoS and the Idle class, as Task Manager (#3071)
    };
}

ProcessActionResult WindowsProcessActions::terminate(const ProcessTarget& target)
{
    // A request to exit, as SIGTERM is on Linux: close the process's windows, so it can save, ask
    // to, or refuse. It used to be TerminateProcess, the same as Kill, though the UI promised a
    // graceful shutdown and users pick Terminate over Kill to keep unsaved work (#1094).
    spdlog::info("Asking process {} to close", target.pid);
    if (auto refused = systemPidResult(target, GuardedAction::End))
    {
        return std::move(*refused);
    }
    // The verified handle is held while the windows are enumerated, so the PID cannot be reused
    // by another process between the identity check and the close requests.
    VerifiedProcess process = openForAction(m_Fns,
                                            target,
                                            SYNCHRONIZE,
                                            [&target](DWORD error, bool isProtected)
                                            { return actionOpenFailure(error, target.pid, GuardedAction::End, isProtected); });
    if (!process.result.success)
    {
        spdlog::warn("{}", process.result.errorMessage);
        return std::move(process.result);
    }
    // A critical process that closes when asked stops Windows as surely as one killed (#2556).
    if (auto refused = guardRefusal(m_Fns, process.handle.get(), target, GuardedAction::End))
    {
        return std::move(*refused);
    }

    CloseRequest request{.pid = static_cast<DWORD>(target.pid)};
    // The callback never stops the enumeration, so a zero return is a failure: some windows may not
    // have been examined, and the counts below would describe only part of the process.
    if (EnumWindows(postCloseToProcessWindow, reinterpret_cast<LPARAM>(&request)) == 0)
    {
        return ProcessActionResult::error(
            std::format("Could not list the windows of process {} to ask it to close: error {}", target.pid, GetLastError()));
    }
    if (std::string failure = closeRequestFailure(target.pid, request.eligible, request.failed, request.firstError); !failure.empty())
    {
        spdlog::info("{}", failure);
        // Every request refused for want of access (UIPI: the process runs elevated) is one the elevated
        // helper can post (#2602).
        return isCloseRequestAccessDenied(request.eligible, request.failed, request.firstError)
                 ? ProcessActionResult::denied(std::move(failure))
                 : ProcessActionResult::error(std::move(failure));
    }
    spdlog::info("Asked {} window(s) of process {} to close", request.eligible, target.pid);
    return ProcessActionResult::ok();
}

ProcessActionResult WindowsProcessActions::kill(const ProcessTarget& target)
{
    spdlog::info("Killing process {}", target.pid);
    return terminateProcess(target, 9);
}

ProcessActionResult WindowsProcessActions::stop(const ProcessTarget& target)
{
    spdlog::info("Suspending process {}", target.pid);
    return suspendOrResume(target, true);
}

ProcessActionResult WindowsProcessActions::resume(const ProcessTarget& target)
{
    spdlog::info("Resuming process {}", target.pid);
    return suspendOrResume(target, false);
}

ProcessActionResult WindowsProcessActions::suspendOrResume(const ProcessTarget& target, bool suspend) const
{
    // NtSuspendProcess and NtResumeProcess act on every thread in one call, so a thread the process
    // starts meanwhile isn't missed, as it would be by SuspendThread on each (#2582).
    Windows::NtProcessSuspendResumeFn call = suspend ? m_Fns.ntSuspendProcess : m_Fns.ntResumeProcess;
    if (call == nullptr)
    {
        return ProcessActionResult::error(
            std::format("{} is not available on this Windows; nothing sent to process {}", suspend ? "Suspend" : "Resume", target.pid));
    }
    if (suspend)
    {
        if (auto refused = systemPidResult(target, GuardedAction::Suspend))
        {
            return std::move(*refused);
        }
        // TaskSmack (or, in the elevated helper, the helper) suspended would hang with nothing left to
        // resume it. Checked by PID before anything is opened (isSamePid(): Windows ignores a PID's
        // low two bits when it looks one up, so PID+1 would open this process too). The own PID is
        // the table's, so a test can name one that is never its fake target's (#2785).
        if (isSamePid(static_cast<std::uint32_t>(target.pid), m_Fns.getCurrentProcessId()))
        {
            std::string refusal = selfSuspendRefusal(target.pid);
            spdlog::warn("{}", refusal);
            return ProcessActionResult::error(std::move(refusal));
        }
    }

    // A protected process refuses PROCESS_SUSPEND_RESUME even to an administrator: said so, with no retry.
    VerifiedProcess process = openForAction(m_Fns,
                                            target,
                                            PROCESS_SUSPEND_RESUME,
                                            [&target, suspend](DWORD error, bool isProtected)
                                            { return suspendResumeOpenFailure(error, target.pid, suspend, isProtected); });
    if (!process.result.success)
    {
        spdlog::warn("{}", process.result.errorMessage);
        return std::move(process.result);
    }
    // Checked on the handle the call goes through, so the process checked is the process suspended.
    if (suspend)
    {
        if (auto refused = guardRefusal(m_Fns, process.handle.get(), target, GuardedAction::Suspend))
        {
            return std::move(*refused);
        }
    }

    const LONG status = call(process.handle.get());
    if (status < 0) // NT_SUCCESS() is a non-negative NTSTATUS
    {
        const auto code = static_cast<std::uint32_t>(status);
        std::string msg = suspendResumeFailure(code, target.pid, suspend);
        spdlog::warn("{}", msg);
        return isSuspendResumeAccessDenied(code) ? ProcessActionResult::denied(std::move(msg)) : ProcessActionResult::error(std::move(msg));
    }
    spdlog::info("{} process {}", suspend ? "Suspended" : "Resumed", target.pid);
    return ProcessActionResult::ok();
}

ProcessActionResult WindowsProcessActions::terminateProcess(const ProcessTarget& target, uint32_t exitCode) const
{
    if (auto refused = systemPidResult(target, GuardedAction::End))
    {
        return std::move(*refused);
    }
    // A protected process refuses PROCESS_TERMINATE even to an administrator: said so, with no retry.
    VerifiedProcess process = openForAction(m_Fns,
                                            target,
                                            PROCESS_TERMINATE,
                                            [&target](DWORD error, bool isProtected)
                                            { return actionOpenFailure(error, target.pid, GuardedAction::End, isProtected); });
    if (!process.result.success)
    {
        spdlog::warn("{}", process.result.errorMessage);
        return std::move(process.result);
    }
    // Checked on the handle the kill goes through, so the process checked is the process ended (#2556).
    if (auto refused = guardRefusal(m_Fns, process.handle.get(), target, GuardedAction::End))
    {
        return std::move(*refused);
    }

    const BOOL result = TerminateProcess(process.handle.get(), exitCode);
    const DWORD error = GetLastError();

    if (result == 0)
    {
        return callFailure(m_Fns,
                           process.handle.get(),
                           target,
                           GuardedAction::End,
                           error,
                           std::format("Failed to terminate process {}: error {}", target.pid, error));
    }

    spdlog::info("Successfully terminated process {} with exit code {}", target.pid, exitCode);
    return ProcessActionResult::ok();
}

ProcessActionResult WindowsProcessActions::setPriority(const ProcessTarget& target, int32_t nice)
{
    // Clamp nice value to valid range for consistency with Linux
    const int32_t clampedNice = Domain::Priority::clampNice(nice);

    const uint32_t priorityClass = niceToPriorityClass(clampedNice);
    spdlog::debug("Setting priority class {} (nice={}) for PID {}", priorityClass, clampedNice, target.pid);
    if (auto refused = systemPidResult(target, GuardedAction::Priority))
    {
        return std::move(*refused);
    }

    VerifiedProcess process = openForAction(m_Fns,
                                            target,
                                            PROCESS_SET_INFORMATION,
                                            [&target](DWORD error, bool isProtected)
                                            { return actionOpenFailure(error, target.pid, GuardedAction::Priority, isProtected); });
    if (!process.result.success)
    {
        spdlog::warn("{}", process.result.errorMessage);
        return std::move(process.result);
    }
    // Idle or Below normal starves a critical process, and the session with it; nothing a user needs
    // is served by raising one either (#2556). Realtime is never set at all (niceToPriorityClass()).
    if (auto refused = guardRefusal(m_Fns, process.handle.get(), target, GuardedAction::Priority))
    {
        return std::move(*refused);
    }

    const BOOL result = SetPriorityClass(process.handle.get(), priorityClass);
    const DWORD error = result == 0 ? GetLastError() : 0; // Capture error before the handle closes

    if (result == 0)
    {
        return callFailure(m_Fns,
                           process.handle.get(),
                           target,
                           GuardedAction::Priority,
                           error,
                           std::format("Failed to set priority for process {}: error {}", target.pid, error));
    }

    spdlog::info("Successfully set priority (nice={}) for PID {}", clampedNice, target.pid);
    return ProcessActionResult::ok();
}

ProcessActionResult WindowsProcessActions::setEfficiencyMode(const ProcessTarget& target, bool on)
{
    if (!hasEfficiencyMode(m_Fns))
    {
        return ProcessActionResult::error(
            std::format("Efficiency mode is not available on this Windows; nothing sent to process {}", target.pid));
    }
    spdlog::debug("Turning Efficiency mode {} for PID {}", on ? "on" : "off", target.pid);
    if (auto refused = systemPidResult(target, GuardedAction::EfficiencyMode))
    {
        return std::move(*refused);
    }
    // PROCESS_SET_INFORMATION for both calls; openVerified() adds PROCESS_QUERY_LIMITED_INFORMATION, which
    // GetPriorityClass() needs.
    VerifiedProcess process = openForAction(m_Fns,
                                            target,
                                            PROCESS_SET_INFORMATION,
                                            [&target](DWORD error, bool isProtected)
                                            { return actionOpenFailure(error, target.pid, GuardedAction::EfficiencyMode, isProtected); });
    if (!process.result.success)
    {
        spdlog::warn("{}", process.result.errorMessage);
        return std::move(process.result);
    }
    // Idle starves a critical process as surely as a priority change does (#2556), and a protected one is
    // never touched.
    if (auto refused = guardRefusal(m_Fns, process.handle.get(), target, GuardedAction::EfficiencyMode))
    {
        return std::move(*refused);
    }

    const DWORD current = m_Fns.getPriorityClass(process.handle.get()); // 0 when it can't be read
    if (setExecutionSpeedThrottling(m_Fns, process.handle.get(), on) == FALSE)
    {
        const DWORD error = GetLastError();
        return callFailure(
            m_Fns, process.handle.get(), target, GuardedAction::EfficiencyMode, error, efficiencyModeFailure(error, target.pid, on));
    }
    EfficiencyModePriorities& priorities = sharedEfficiencyModePriorities();
    if (on)
    {
        if (m_Fns.setPriorityClass(process.handle.get(), IDLE_PRIORITY_CLASS) == FALSE)
        {
            const DWORD error = GetLastError();
            static_cast<void>(setExecutionSpeedThrottling(m_Fns, process.handle.get(), false)); // a refused "on" changes nothing
            return callFailure(m_Fns,
                               process.handle.get(),
                               target,
                               GuardedAction::EfficiencyMode,
                               error,
                               efficiencyModePriorityFailure(error, target.pid, true));
        }
        priorities.remember(target, current);
        spdlog::info("Turned Efficiency mode on for PID {} (priority class was 0x{:X})", target.pid, current);
        return ProcessActionResult::ok();
    }

    const std::optional<std::uint32_t> restore = priorityClassToRestore(priorities.take(target), current);
    if (restore.has_value() && m_Fns.setPriorityClass(process.handle.get(), *restore) == FALSE)
    {
        const DWORD error = GetLastError();
        return callFailure(m_Fns,
                           process.handle.get(),
                           target,
                           GuardedAction::EfficiencyMode,
                           error,
                           efficiencyModePriorityFailure(error, target.pid, false));
    }
    spdlog::info("Turned Efficiency mode off for PID {}", target.pid);
    return ProcessActionResult::ok();
}

// NOLINTEND(misc-include-cleaner)
ProcessActionResult
WindowsProcessActions::setIoPriority(const ProcessTarget& target, [[maybe_unused]] IoPriorityClass ioClass, [[maybe_unused]] int32_t level)
{
    return ProcessActionResult::error(std::format("I/O priority is not supported on Windows; nothing sent to process {}", target.pid));
}

IoPriorityReadResult WindowsProcessActions::getIoPriority(const ProcessTarget& target)
{
    return std::unexpected(std::format("I/O priority is not supported on Windows (process {})", target.pid));
}

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
ProcessActionResult WindowsProcessActions::setAffinity(const ProcessTarget& target, const CpuAffinity& cpus)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (auto refused = systemPidResult(target, GuardedAction::Affinity))
    {
        return std::move(*refused);
    }
    auto masks = groupMasksForAffinity(cpus, m_GroupSizes);
    if (!masks.has_value())
    {
        return ProcessActionResult::error(std::move(masks.error()));
    }
    const bool useCpuSets = m_GroupSizes.size() > 1;

    // CPU sets need only PROCESS_SET_LIMITED_INFORMATION; the affinity mask needs PROCESS_SET_INFORMATION.
    VerifiedProcess process = openForAction(m_Fns,
                                            target,
                                            useCpuSets ? PROCESS_SET_LIMITED_INFORMATION : PROCESS_SET_INFORMATION,
                                            [&target](DWORD error, bool isProtected)
                                            { return actionOpenFailure(error, target.pid, GuardedAction::Affinity, isProtected); });
    if (!process.result.success)
    {
        spdlog::warn("Not setting CPU affinity for PID {}: {}", target.pid, process.result.errorMessage);
        return std::move(process.result);
    }
    // Pinning a critical process to one CPU can hang the session as surely as starving it (#2556).
    if (auto refused = guardRefusal(m_Fns, process.handle.get(), target, GuardedAction::Affinity))
    {
        return std::move(*refused);
    }

    BOOL ok = FALSE;
    std::string note; // Default CPU sets are a preference: say so rather than claim a hard affinity (#2173)
    if (!useCpuSets)
    {
        // One group: masks holds exactly group 0's mask (an empty choice was refused above).
        ok = m_Fns.setProcessAffinityMask(process.handle.get(), static_cast<DWORD_PTR>(masks->front().mask));
    }
    else if (cpus == affinityLayout().online)
    {
        // Every CPU: clear the default CPU sets rather than pin the process to today's processors.
        ok = m_Fns.setProcessDefaultCpuSets(process.handle.get(), nullptr, 0);
        note = "by clearing its preferred CPUs (CPU sets); any affinity mask or thread affinity set earlier still applies";
    }
    else
    {
        auto ids = cpuSetIdsFor(*masks, m_Fns.readCpuSets());
        if (!ids.has_value())
        {
            return ProcessActionResult::error(std::move(ids.error()));
        }
        const std::vector<ULONG> cpuSetIds(ids->begin(), ids->end()); // ULONG is 32 bits, but not uint32_t's type
        ok = m_Fns.setProcessDefaultCpuSets(process.handle.get(), cpuSetIds.data(), static_cast<ULONG>(cpuSetIds.size()));
        note = "as preferred CPUs (CPU sets); threads with their own affinity are not moved";
    }
    if (ok == FALSE)
    {
        const DWORD error = GetLastError();
        return callFailure(
            m_Fns, process.handle.get(), target, GuardedAction::Affinity, error, affinitySetErrorMessage(error, target.pid, useCpuSets));
    }
    if (note.empty())
    {
        spdlog::info("Set CPU affinity ({} CPUs) for PID {}", cpus.count(), target.pid);
    }
    else
    {
        spdlog::info("Set CPU affinity ({} CPUs) for PID {} {}", cpus.count(), target.pid, note);
    }
    return ProcessActionResult::ok(std::move(note));
}

CpuAffinityLayout WindowsProcessActions::affinityLayout() const
{
    const std::vector<ProcessorCoreRecord> cores = (m_Fns.readCores != nullptr) ? m_Fns.readCores() : std::vector<ProcessorCoreRecord>{};
    return affinityLayoutFromCores(m_GroupSizes, cores);
}
// NOLINTEND(misc-include-cleaner)

#ifdef TASKSMACK_NO_PROCESS_DUMPS
ProcessDumpResult
WindowsProcessActions::createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken)
{
    return IProcessActions::createDump(target, directory, stopToken);
}

DumpSpace WindowsProcessActions::dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory)
{
    return IProcessActions::dumpSpace(target, directory);
}

FolderChoice WindowsProcessActions::chooseDumpFolder(const std::filesystem::path& initial, void* ownerWindow)
{
    return IProcessActions::chooseDumpFolder(initial, ownerWindow);
}
#else
// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
ProcessDumpResult
WindowsProcessActions::createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken)
{
    // A snapshot needs more access than a live dump (Windows::SNAPSHOT_PROCESS_ACCESS). A process that
    // refuses it may still allow a live dump's, so that is asked for next, and the dump says why it
    // is live.
    Windows::DumpSource source{.captureSnapshot = m_Fns.pssCaptureSnapshot, .liveReason = {}, .processMachine = nullptr};
    VerifiedProcess process;
    bool snapshotOpenRefused = false;
    if (source.captureSnapshot != nullptr)
    {
        process = openVerified(m_Fns,
                               target,
                               Windows::SNAPSHOT_PROCESS_ACCESS,
                               [&snapshotOpenRefused](DWORD /*error*/)
                               {
                                   snapshotOpenRefused = true;
                                   return std::string{};
                               });
    }
    if (source.captureSnapshot == nullptr || snapshotOpenRefused)
    {
        process = openVerified(m_Fns,
                               target,
                               Windows::DUMP_PROCESS_ACCESS,
                               [&target](DWORD error)
                               {
                                   // Only a refusal asks whether the process is protected: the one case
                                   // where running as administrator would not help.
                                   const bool isProtected =
                                       error == ERROR_ACCESS_DENIED && Windows::isProtectedProcess(static_cast<DWORD>(target.pid));
                                   return DumpMath::openFailureMessage(error, target.pid, isProtected);
                               });
        if (snapshotOpenRefused)
        {
            source.captureSnapshot = nullptr;
            source.liveReason = DumpMath::snapshotFailureReason(0, true);
        }
    }
    if (!process.result.success)
    {
        spdlog::warn("Not writing a dump of PID {}: {}", target.pid, process.result.errorMessage);
        return std::unexpected(std::move(process.result.errorMessage));
    }
    return Windows::writeProcessDump(process.handle.get(), target.pid, directory, stopToken, source);
}

DumpSpace WindowsProcessActions::dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory)
{
    DumpSpace space{.freeBytes = Windows::freeBytesInFolder(directory), .processBytes = std::nullopt};
    // The private bytes are read from the target itself, through the identity check, never from
    // whatever holds its PID now.
    const VerifiedProcess process =
        openVerified(m_Fns, target, PROCESS_QUERY_LIMITED_INFORMATION, [](DWORD error) { return std::format("error {}", error); });
    if (process.result.success)
    {
        space.processBytes = Windows::processPrivateBytes(process.handle.get());
    }
    return space;
}

FolderChoice WindowsProcessActions::chooseDumpFolder(const std::filesystem::path& initial, void* ownerWindow)
{
    return Windows::pickFolder(ownerWindow, initial, L"Save dump files to");
}
// NOLINTEND(misc-include-cleaner)
#endif // TASKSMACK_NO_PROCESS_DUMPS

} // namespace Platform
