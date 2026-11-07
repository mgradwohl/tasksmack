#include "WindowsProcessActions.h"

#include "Domain/PriorityConfig.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/WaitChain.h"
#include "WindowsAffinityMath.h"
#include "WindowsConversions.h"
#include "WindowsDebugPrivilege.h"
#include "WindowsFileLocation.h"
#include "WindowsFolderPicker.h"
#include "WindowsHandles.h"
#include "WindowsNtQuery.h"
#include "WindowsProcessActionsMath.h"
#include "WindowsProcessDump.h"
#include "WindowsProcessDumpMath.h"
#include "WindowsProcessIdentity.h"
#include "WindowsRunProgram.h"
#ifndef TASKSMACK_NO_WAIT_CHAINS
#include "WindowsWaitChain.h"
#endif

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
#include <string_view>
#ifndef TASKSMACK_NO_FILE_LOCATION
#include <system_error>
#endif
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

// The same for wait chains (#3084): the helper never serves one, so it is built with
// TASKSMACK_NO_WAIT_CHAINS and WindowsWaitChain.cpp stays out of the elevated image.
#ifdef TASKSMACK_NO_WAIT_CHAINS
constexpr bool HAS_WAIT_CHAINS = false;
#else
constexpr bool HAS_WAIT_CHAINS = true;
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

/// Whether Open file location can be offered (#3426): the executable's path can be read, and Explorer
/// can be asked to show it, by the shell or by starting explorer.exe.
[[nodiscard]] bool hasFileLocation(const WindowsProcessFunctions& fns) noexcept
{
    const Windows::FileLocationFunctions& shell = fns.fileLocation;
    return fns.queryFullProcessImageName != nullptr &&
           (shell.selectInFolder != nullptr || (shell.startProcess != nullptr && shell.explorerPath != nullptr));
}

#ifndef TASKSMACK_NO_FILE_LOCATION
/// Why Open file location couldn't read the process's executable (#3426): OpenProcess's @p error.
[[nodiscard]] std::string fileLocationOpenFailure(DWORD error)
{
    if (error == ERROR_INVALID_PARAMETER)
    {
        return "Process not found - may have already exited";
    }
    if (error == ERROR_ACCESS_DENIED)
    {
        return "Its executable can't be read: access is denied";
    }
    return std::format("Its executable can't be read: {}", std::system_category().message(static_cast<int>(error)));
}
#endif

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
            cores.push_back({
                .group = record.Processor.GroupMask[0].Group,
                .mask = static_cast<std::uint64_t>(record.Processor.GroupMask[0].Mask),
                .efficiencyClass = record.Processor.EfficiencyClass,
            });
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
    const bool refusesProtected = action == GuardedAction::End || action == GuardedAction::Suspend ||
                                  action == GuardedAction::EfficiencyMode || action == GuardedAction::SuspendThread ||
                                  action == GuardedAction::EndThread;
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

/// Whether the table has every call one thread's actions make (#3069).
[[nodiscard]] bool hasThreadActions(const WindowsProcessFunctions& fns) noexcept
{
    return fns.openThread != nullptr && fns.getProcessIdOfThread != nullptr && fns.suspendThread != nullptr &&
           fns.resumeThread != nullptr && fns.terminateThread != nullptr && fns.getThreadTimes != nullptr;
}

/// Whether the table has the calls Set Realtime priority makes (#3429): the class set, then read back.
[[nodiscard]] bool hasRealtimePriority(const WindowsProcessFunctions& fns) noexcept
{
    return fns.getPriorityClass != nullptr && fns.setPriorityClass != nullptr;
}

/// Whether the table has the calls the I/O priority reads and sets with (#3362).
[[nodiscard]] bool hasIoPriority(const WindowsProcessFunctions& fns) noexcept
{
    return fns.ntQueryInformationProcess != nullptr && fns.ntSetInformationProcess != nullptr;
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
        .openThread = &OpenThread,
        .getProcessIdOfThread = &GetProcessIdOfThread,
        .suspendThread = &SuspendThread,
        .resumeThread = &ResumeThread,
        .terminateThread = &TerminateThread,
        .getThreadTimes = &GetThreadTimes,
        .queryFullProcessImageName = &QueryFullProcessImageNameW,
#ifdef TASKSMACK_NO_FILE_LOCATION
        .fileLocation = {}, // TaskSmackHelper.exe: no shell calls in the elevated image (#3426)
#else
        .fileLocation = Windows::realFileLocationFunctions(),
#endif
#ifdef TASKSMACK_NO_RUN_PROGRAM
        .runProgram = {}, // TaskSmackHelper.exe: no ShellExecuteExW in the elevated image (#3427)
#else
        .runProgram = Windows::realRunProgramFunctions(),
#endif
        .ntQueryInformationProcess = Windows::ntQueryInformationProcess(),
        .ntSetInformationProcess = Windows::ntSetInformationProcess(),
        .enableIncreaseBasePriority = []
        { return Windows::enableIncreaseBasePriorityPrivilegeIfElevated() == Windows::DebugPrivilegeResult::Enabled; },
    };
}

WindowsProcessActions::WindowsProcessActions() : WindowsProcessActions(realWindowsProcessFunctions())
{}

WindowsProcessActions::WindowsProcessActions(WindowsProcessFunctions functions)
    : m_Fns(functions),
      m_GroupSizes((m_Fns.readGroupSizes != nullptr) ? m_Fns.readGroupSizes() : std::vector<std::uint32_t>{}),
      // Constant for the process's life, so read once: actionCapabilities() is asked every frame (#3182).
      m_Elevated(m_Fns.runProgram.shellExecute != nullptr && m_Fns.runProgram.isElevated != nullptr && m_Fns.runProgram.isElevated())
{}

ProcessActionCapabilities WindowsProcessActions::actionCapabilities() const
{
    return ProcessActionCapabilities{
        .canTerminate = true,                                 // WM_CLOSE to the process's windows: a request it may answer (#1094)
        .canKill = true,                                      // TerminateProcess
        .canStop = m_Fns.ntSuspendProcess != nullptr,         // NtSuspendProcess (#2582)
        .canContinue = m_Fns.ntResumeProcess != nullptr,      // NtResumeProcess (#2582)
        .canSetPriority = true,                               // SetPriorityClass
        .canSetRealtimePriority = hasRealtimePriority(m_Fns), // and read back with GetPriorityClass (#3429)
        .canSetIoPriority = hasIoPriority(m_Fns),             // NtSetInformationProcess(ProcessIoPriority) (#3362)
        .canSetRealtimeIoPriority = false,                    // Linux's class: Windows has hints, High through the helper
        .ioPriorityHints = true,
        // SetProcessAffinityMask, or CPU sets across processor groups (#1881); needs the groups' sizes
        .canSetAffinity = !m_GroupSizes.empty(),
        .syscallTrace = SyscallTraceAvailability::Unsupported,
        .canCreateDump = HAS_DUMPS,                    // MiniDumpWriteDump, dbghelp.dll loaded on the first dump (#807)
        .canChooseDumpFolder = HAS_DUMPS,              // IFileOpenDialog with FOS_PICKFOLDERS (#2001)
        .dumpsFromSnapshot = HAS_DUMPS,                // PssCaptureSnapshot, else from the running process (#2001)
        .canAuthorizeInteractively = false,            // BrokeredProcessActions sets it when TaskSmackHelper.exe can be used (#2550)
        .canOpenFileLocation = hasFileLocation(m_Fns), // SHOpenFolderAndSelectItems, or explorer.exe /select (#3426)
        .canRunProgram = m_Fns.runProgram.shellExecute != nullptr, // ShellExecuteExW, as Run new task (#3427)
        // The runas verb's UAC prompt, unless TaskSmack is elevated already, when every task it starts is too.
        .canRunProgramAsAdministrator = m_Fns.runProgram.shellExecute != nullptr && !m_Elevated,
        .canSetEfficiencyMode = hasEfficiencyMode(m_Fns), // EcoQoS and the Idle class, as Task Manager (#3071)
        .canAnalyzeWaitChain = HAS_WAIT_CHAINS,           // GetThreadWaitChain, as Task Manager's Analyze wait chain (#3084)
        .canCloseConnections = false,
        .canActOnThreads = hasThreadActions(m_Fns), // SuspendThread, ResumeThread, TerminateThread (#3069)
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
    // Realtime is confirmed by reading the class back (#3429), so it needs both calls in the table.
    const bool realtime = priorityClass == REALTIME_PRIORITY_CLASS;
    if (realtime && !hasRealtimePriority(m_Fns))
    {
        return ProcessActionResult::error(
            std::format("Realtime priority is not available on this Windows; nothing sent to process {}", target.pid));
    }
    spdlog::debug("Setting priority class {} (nice={}) for PID {}", priorityClass, clampedNice, target.pid);
    if (auto refused = systemPidResult(target, GuardedAction::Priority))
    {
        return std::move(*refused);
    }

    // PROCESS_SET_INFORMATION; openVerified() adds PROCESS_QUERY_LIMITED_INFORMATION, which reading the
    // class back needs.
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
    // is served by raising one either (#2556), least of all to Realtime.
    if (auto refused = guardRefusal(m_Fns, process.handle.get(), target, GuardedAction::Priority))
    {
        return std::move(*refused);
    }

    // Through the table where it has the call (tests' fakes; the real one otherwise), so no test needs to
    // change a real process's class to reach the paths after it.
    const auto setClass = (m_Fns.setPriorityClass != nullptr) ? m_Fns.setPriorityClass : &::SetPriorityClass;
    const BOOL result = setClass(process.handle.get(), priorityClass);
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

    if (realtime)
    {
        // Without SeIncreaseBasePriorityPrivilege, SetPriorityClass() succeeds and quietly sets High: only
        // the class read back says which it was (#3429). An elevated caller needs no privilege enabled
        // first: kernelbase acquires it for the call.
        const DWORD readBack = m_Fns.getPriorityClass(process.handle.get());
        ProcessActionResult confirmed = realtimeReadBackResult(readBack, target.pid);
        spdlog::info("Set Realtime priority for PID {}: Windows set class 0x{:X}", target.pid, readBack);
        return confirmed;
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

    const std::optional<std::uint32_t> remembered = priorities.take(target);
    const std::optional<std::uint32_t> restore = priorityClassToRestore(remembered, current);
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
    const std::string_view note = efficiencyModeOffNote(remembered, restore);
    spdlog::info("Turned Efficiency mode off for PID {}{}", target.pid, note.empty() ? std::string{} : std::format(" ({})", note));
    return ProcessActionResult::ok(std::string{note});
}

ProcessActionResult WindowsProcessActions::setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level)
{
    if (!hasIoPriority(m_Fns))
    {
        return ProcessActionResult::error(
            std::format("I/O priority is not available on this Windows; nothing sent to process {}", target.pid));
    }
    // Only Very low to High, and only Windows' hints: refused before anything is opened.
    std::expected<IoPriorityHint, std::string> hint = ioPriorityHintToSet(ioClass, level, target.pid);
    if (!hint.has_value())
    {
        spdlog::warn("{}", hint.error());
        return ProcessActionResult::error(std::move(hint.error()));
    }
    spdlog::debug("Setting I/O priority {} for PID {}", ioPriorityHintName(*hint), target.pid);
    if (auto refused = systemPidResult(target, GuardedAction::IoPriority))
    {
        return std::move(*refused);
    }

    VerifiedProcess process = openForAction(m_Fns,
                                            target,
                                            PROCESS_SET_INFORMATION,
                                            [&target](DWORD error, bool isProtected)
                                            { return actionOpenFailure(error, target.pid, GuardedAction::IoPriority, isProtected); });
    if (!process.result.success)
    {
        spdlog::warn("{}", process.result.errorMessage);
        return std::move(process.result);
    }
    // Very low starves a critical process's paging and file I/O as surely as the Idle class starves its CPU (#2556).
    if (auto refused = guardRefusal(m_Fns, process.handle.get(), target, GuardedAction::IoPriority))
    {
        return std::move(*refused);
    }

    // NtSetInformationProcess checks SeIncreaseBasePriorityPrivilege for High and doesn't enable it for the call,
    // so an elevated TaskSmack (or the helper) enables it first. Not elevated, Windows refuses, and the refusal is
    // a permission one.
    if (*hint == IoPriorityHint::High && m_Fns.enableIncreaseBasePriority != nullptr)
    {
        static_cast<void>(m_Fns.enableIncreaseBasePriority());
    }
    auto value = static_cast<ULONG>(*hint);
    const auto status =
        static_cast<std::uint32_t>(m_Fns.ntSetInformationProcess(process.handle.get(), PROCESS_IO_PRIORITY_CLASS, &value, sizeof(value)));
    const bool isProtectedProcess = status == NT_STATUS_ACCESS_DENIED && isProtected(m_Fns, process.handle.get());
    ProcessActionResult result = ioPrioritySetResult(status, target.pid, *hint, isProtectedProcess);
    if (result.success)
    {
        spdlog::info("Set I/O priority {} for PID {}", ioPriorityHintName(*hint), target.pid);
    }
    else
    {
        spdlog::warn("{}", result.errorMessage);
    }
    return result;
}

IoPriorityReadResult WindowsProcessActions::getIoPriority(const ProcessTarget& target)
{
    if (!hasIoPriority(m_Fns))
    {
        return std::unexpected(std::format("I/O priority is not available on this Windows (process {})", target.pid));
    }
    // The identity check every action makes, so the value read is the target's, never a reused PID's.
    const VerifiedProcess process =
        openVerified(m_Fns,
                     target,
                     PROCESS_QUERY_LIMITED_INFORMATION,
                     [&target](DWORD error)
                     {
                         return error == ERROR_ACCESS_DENIED
                                  ? std::format("Access denied: can't read the I/O priority of process {}", target.pid)
                                  : actionOpenFailure(error, target.pid, GuardedAction::IoPriority, false);
                     });
    if (!process.result.success)
    {
        return std::unexpected(process.result.errorMessage);
    }
    ULONG value = 0;
    ULONG written = 0;
    const auto status = static_cast<std::uint32_t>(
        m_Fns.ntQueryInformationProcess(process.handle.get(), PROCESS_IO_PRIORITY_CLASS, &value, sizeof(value), &written));
    return ioPriorityReadResult(status, value, target.pid);
}

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

ProcessActionResult
WindowsProcessActions::actOnThread(const ProcessTarget& target, std::int32_t tid, std::uint64_t threadCreateTime, ThreadAction action)
{
    const char* verb = threadActionVerb(action);
    spdlog::info("Asked to {} thread {} of process {}", verb, tid, target.pid);
    if (!hasThreadActions(m_Fns))
    {
        return ProcessActionResult::error("Acting on one thread isn't available on this Windows");
    }
    // Before anything is opened: never the kernel's threads, nor TaskSmack's own (or, in the helper, its
    // TaskSmack's), whose suspended or ended thread would leave nothing to undo it.
    if (std::string refusal = threadActionRefusal(target.pid, tid, action, m_Fns.getCurrentProcessId(), m_ThreadActionRefusedPid);
        !refusal.empty())
    {
        spdlog::warn("{}", refusal);
        return ProcessActionResult::error(std::move(refusal));
    }
    // The process first, by its identity, so its handle pins it while its thread is checked and acted on.
    VerifiedProcess process = openForAction(m_Fns,
                                            target,
                                            PROCESS_QUERY_LIMITED_INFORMATION,
                                            [&target, tid, action](DWORD error, bool isProtected)
                                            {
                                                return error == ERROR_INVALID_PARAMETER
                                                         ? std::string("Process not found - may have already exited")
                                                         : threadOpenFailure(error, target.pid, tid, action, isProtected);
                                            });
    if (!process.result.success)
    {
        spdlog::warn("{}", process.result.errorMessage);
        return std::move(process.result);
    }
    if (const std::optional<GuardedAction> guard = threadActionGuard(action); guard.has_value())
    {
        if (auto refused = guardRefusal(m_Fns, process.handle.get(), target, *guard))
        {
            return std::move(*refused);
        }
    }

    // THREAD_QUERY_LIMITED_INFORMATION too, for GetProcessIdOfThread.
    const DWORD access = (action == ThreadAction::End ? THREAD_TERMINATE : THREAD_SUSPEND_RESUME) | THREAD_QUERY_LIMITED_INFORMATION;
    const Windows::UniqueHandle thread(m_Fns.openThread(access, FALSE, static_cast<DWORD>(tid)));
    if (!thread)
    {
        const DWORD error = GetLastError();
        const bool isProtectedProcess = error == ERROR_ACCESS_DENIED && isProtected(m_Fns, process.handle.get());
        std::string message = threadOpenFailure(error, target.pid, tid, action, isProtectedProcess);
        spdlog::warn("{}", message);
        return (error == ERROR_ACCESS_DENIED && !isProtectedProcess) ? ProcessActionResult::denied(std::move(message))
                                                                     : ProcessActionResult::error(std::move(message));
    }
    // The handle's own process: a TID reused by another process's thread since the list was read is refused.
    if (const DWORD owner = m_Fns.getProcessIdOfThread(thread.get()); std::cmp_not_equal(owner, target.pid))
    {
        std::string refusal = threadNotInProcessRefusal(tid, target.pid, owner);
        spdlog::warn("{}", refusal);
        return ProcessActionResult::error(std::move(refusal));
    }
    // And the thread the user confirmed: a TID the process reused for a newer thread since the list was read is
    // refused, as a reused PID is. An unknown confirmed time is refused, never skipped.
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    const bool timesRead = m_Fns.getThreadTimes(thread.get(), &created, &exited, &kernel, &user) != FALSE;
    const std::uint64_t actualCreateTime = timesRead ? Windows::fileTimeTicks(created) : 0;
    if (std::string refusal = threadIdentityRefusal(tid, target.pid, threadCreateTime, actualCreateTime); !refusal.empty())
    {
        spdlog::warn("{}", refusal);
        return ProcessActionResult::error(std::move(refusal));
    }

    if (action == ThreadAction::End)
    {
        // TerminateThread: the thread ends at once, with no chance to release what it holds (the confirm says so).
        constexpr DWORD THREAD_EXIT_CODE = 1;
        if (m_Fns.terminateThread(thread.get(), THREAD_EXIT_CODE) == FALSE)
        {
            const DWORD error = GetLastError();
            std::string message = threadCallFailure(error, target.pid, tid, action);
            spdlog::warn("{}", message);
            return error == ERROR_ACCESS_DENIED ? ProcessActionResult::denied(std::move(message))
                                                : ProcessActionResult::error(std::move(message));
        }
        spdlog::info("Ended thread {} of process {}", tid, target.pid);
        return ProcessActionResult::ok();
    }
    const DWORD previous = (action == ThreadAction::Suspend) ? m_Fns.suspendThread(thread.get()) : m_Fns.resumeThread(thread.get());
    if (constexpr DWORD CALL_FAILED = ~DWORD{0}; previous == CALL_FAILED) // (DWORD)-1, as both document
    {
        const DWORD error = GetLastError();
        std::string message = threadCallFailure(error, target.pid, tid, action);
        spdlog::warn("{}", message);
        return error == ERROR_ACCESS_DENIED ? ProcessActionResult::denied(std::move(message))
                                            : ProcessActionResult::error(std::move(message));
    }
    spdlog::info("{} thread {} of process {} (suspend count was {})",
                 action == ThreadAction::Suspend ? "Suspended" : "Resumed",
                 tid,
                 target.pid,
                 previous);
    return ProcessActionResult::ok(threadActionNote(action, previous));
}

ProcessActionResult WindowsProcessActions::runProgram(const RunProgramRequest& request)
{
#ifdef TASKSMACK_NO_RUN_PROGRAM
    // TaskSmackHelper.exe: BrokeredProcessActions never sends it one, and WindowsRunProgram.cpp isn't built in.
    return IProcessActions::runProgram(request);
#else
    if (m_Fns.runProgram.shellExecute == nullptr)
    {
        return IProcessActions::runProgram(request);
    }
    return Windows::runShellProgram(m_Fns.runProgram, m_Elevated, request);
#endif
}

ProcessActionResult WindowsProcessActions::openFileLocation(const ProcessTarget& target)
{
#ifdef TASKSMACK_NO_FILE_LOCATION
    // TaskSmackHelper.exe: BrokeredProcessActions never sends it one, and WindowsFileLocation.cpp isn't built in.
    return IProcessActions::openFileLocation(target);
#else
    if (!hasFileLocation(m_Fns))
    {
        return IProcessActions::openFileLocation(target);
    }
    if (target.pid == SYSTEM_PROCESS_PID)
    {
        return ProcessActionResult::error("The System process has no executable file");
    }
    // The identity check every action makes, so the path read is the target's, never a reused PID's.
    VerifiedProcess process =
        openVerified(m_Fns, target, PROCESS_QUERY_LIMITED_INFORMATION, [](DWORD error) { return fileLocationOpenFailure(error); });
    if (!process.result.success)
    {
        spdlog::warn("Not opening the file location of PID {}: {}", target.pid, process.result.errorMessage);
        return std::move(process.result);
    }
    constexpr DWORD MAX_PATH_CHARS = 32768; // the longest path Windows allows, with its terminator
    std::wstring path(MAX_PATH_CHARS, L'\0');
    DWORD size = MAX_PATH_CHARS;
    if (m_Fns.queryFullProcessImageName(process.handle.get(), 0, path.data(), &size) == 0)
    {
        // The kernel's own processes (Registry, Memory Compression, Secure System) have no image file.
        const DWORD error = GetLastError();
        std::string message = std::format("Its executable can't be read: {}", std::system_category().message(static_cast<int>(error)));
        spdlog::warn("Not opening the file location of PID {}: {}", target.pid, message);
        return ProcessActionResult::error(std::move(message));
    }
    path.resize(size);
    return Windows::revealInExplorer(m_Fns.fileLocation, path);
#endif
}

WaitChainReadResult WindowsProcessActions::readWaitChains(const ProcessTarget& target, const std::stop_token& stopToken)
{
#ifdef TASKSMACK_NO_WAIT_CHAINS
    return IProcessActions::readWaitChains(target, stopToken);
#else
    // As TaskSmack's user, with the system's calls: the same identity check every action makes (#2149).
    return Windows::readWaitChains(Windows::WaitChainFunctions{}, target, stopToken);
#endif
}

} // namespace Platform
