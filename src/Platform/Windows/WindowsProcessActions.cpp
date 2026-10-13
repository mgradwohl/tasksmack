#include "WindowsProcessActions.h"

#include "Domain/PriorityConfig.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "WindowsAffinityMath.h"
#include "WindowsHandles.h"
#include "WindowsProcessActionsMath.h"

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
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

/// A process handle that has passed the identity check, or the reason there is none.
struct VerifiedProcess
{
    Windows::UniqueHandle handle;
    ProcessActionResult result;
};

/// Open `target` with `access` and confirm it is the process the target names.
///
/// The creation time is read from the opened handle, not looked up by PID, and the handle pins the
/// process object until it is closed: so the process checked is the process acted on, with no
/// window for the PID to be reused in between. GetProcessTimes reports the same creation time the
/// probe reads from SYSTEM_PROCESS_INFORMATION::CreateTime, in the same 100ns FILETIME units.
///
/// @param describeOpenFailure  Turns an OpenProcess error code into the message to show.
template<typename DescribeOpenFailure>
[[nodiscard]] VerifiedProcess
openVerified(const WindowsProcessFunctions& fns, const ProcessTarget& target, DWORD access, DescribeOpenFailure describeOpenFailure)
{
    if (target.pid <= 0)
    {
        return {.handle = {}, .result = ProcessActionResult::error("Invalid PID")};
    }
    if (target.startTimeTicks == 0)
    {
        return {.handle = {}, .result = checkProcessIdentity(target, 0)};
    }

    // Note: Windows APIs require DWORD for PIDs; explicit cast from a positive int32_t is safe.
    Windows::UniqueHandle handle(fns.openProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(target.pid)));
    if (!handle)
    {
        return {.handle = {}, .result = ProcessActionResult::error(describeOpenFailure(GetLastError()))};
    }

    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (fns.getProcessTimes(handle.get(), &creation, &exitTime, &kernelTime, &userTime) == 0)
    {
        return {
            .handle = {},
            .result = ProcessActionResult::error(
                std::format("Cannot confirm the identity of process {}: error {}; action not sent", target.pid, GetLastError())),
        };
    }
    const std::uint64_t actualTicks =
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | static_cast<std::uint64_t>(creation.dwLowDateTime);

    ProcessActionResult identity = checkProcessIdentity(target, actualTicks);
    if (!identity.success)
    {
        return {.handle = {}, .result = std::move(identity)};
    }
    return {.handle = std::move(handle), .result = ProcessActionResult::ok()};
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

/// Why opening a process to change its CPU affinity failed.
[[nodiscard]] std::string describeAffinityOpenFailure(const ProcessTarget& target, DWORD error)
{
    if (error == ERROR_ACCESS_DENIED)
    {
        return affinityAccessDeniedMessage(target.pid);
    }
    if (error == ERROR_INVALID_PARAMETER)
    {
        return "Process not found - may have already exited";
    }
    return std::format("Failed to open process {}: error {}", target.pid, error);
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
        .canTerminate = true,      // WM_CLOSE to the process's windows: a request it may answer (#1094)
        .canKill = true,           // TerminateProcess
        .canStop = false,          // Windows doesn't have SIGSTOP equivalent
        .canContinue = false,      // Windows doesn't have SIGCONT equivalent
        .canSetPriority = true,    // SetPriorityClass
        .canSetIoPriority = false, // No ionice equivalent (#803)
        .canSetRealtimeIoPriority = false,
        // SetProcessAffinityMask, or CPU sets across processor groups (#1881); needs the groups' sizes
        .canSetAffinity = !m_GroupSizes.empty(),
    };
}

ProcessActionResult WindowsProcessActions::terminate(const ProcessTarget& target)
{
    // A request to exit, as SIGTERM is on Linux: close the process's windows, so it can save, ask
    // to, or refuse. It used to be TerminateProcess, the same as Kill, though the UI promised a
    // graceful shutdown and users pick Terminate over Kill to keep unsaved work (#1094).
    spdlog::info("Asking process {} to close", target.pid);
    // The verified handle is held while the windows are enumerated, so the PID cannot be reused
    // by another process between the identity check and the close requests.
    const VerifiedProcess process =
        openVerified(m_Fns,
                     target,
                     SYNCHRONIZE,
                     [&target](DWORD error) { return std::format("Failed to open process {}: error {}", target.pid, error); });
    if (!process.handle)
    {
        return process.result;
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
        return ProcessActionResult::error(std::move(failure));
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
    // Windows doesn't have a direct equivalent to SIGSTOP
    // Could potentially use SuspendThread on all threads, but that's complex
    spdlog::warn("Stop not supported on Windows for process {}", target.pid);
    return ProcessActionResult::error("Stop (SIGSTOP) is not supported on Windows");
}

ProcessActionResult WindowsProcessActions::resume(const ProcessTarget& target)
{
    // Windows doesn't have a direct equivalent to SIGCONT
    spdlog::warn("Resume not supported on Windows for process {}", target.pid);
    return ProcessActionResult::error("Resume (SIGCONT) is not supported on Windows");
}

ProcessActionResult WindowsProcessActions::terminateProcess(const ProcessTarget& target, uint32_t exitCode) const
{
    VerifiedProcess process =
        openVerified(m_Fns,
                     target,
                     PROCESS_TERMINATE,
                     [&target](DWORD error) { return std::format("Failed to open process {}: error {}", target.pid, error); });
    if (!process.result.success)
    {
        spdlog::error("{}", process.result.errorMessage);
        return std::move(process.result);
    }

    const BOOL result = TerminateProcess(process.handle.get(), exitCode);
    const DWORD error = GetLastError();

    if (result == 0)
    {
        std::string msg = std::format("Failed to terminate process {}: error {}", target.pid, error);
        spdlog::error("{}", msg);
        return ProcessActionResult::error(std::move(msg));
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

    VerifiedProcess process = openVerified(m_Fns,
                                           target,
                                           PROCESS_SET_INFORMATION,
                                           [&target](DWORD error) -> std::string
                                           {
                                               switch (error)
                                               {
                                               case ERROR_ACCESS_DENIED:
                                                   return "Permission denied - cannot change priority of this process";
                                               case ERROR_INVALID_PARAMETER:
                                                   return "Process not found - may have already exited";
                                               default:
                                                   return std::format("Failed to open process {}: error {}", target.pid, error);
                                               }
                                           });
    if (!process.result.success)
    {
        spdlog::warn("{}", process.result.errorMessage);
        return std::move(process.result);
    }

    const BOOL result = SetPriorityClass(process.handle.get(), priorityClass);
    const DWORD error = result == 0 ? GetLastError() : 0; // Capture error before the handle closes

    if (result == 0)
    {
        std::string msg = std::format("Failed to set priority for process {}: error {}", target.pid, error);
        spdlog::warn("{}", msg);
        return ProcessActionResult::error(std::move(msg));
    }

    spdlog::info("Successfully set priority (nice={}) for PID {}", clampedNice, target.pid);
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
    auto masks = groupMasksForAffinity(cpus, m_GroupSizes);
    if (!masks.has_value())
    {
        return ProcessActionResult::error(std::move(masks.error()));
    }
    const bool useCpuSets = m_GroupSizes.size() > 1;

    // CPU sets need only PROCESS_SET_LIMITED_INFORMATION; the affinity mask needs PROCESS_SET_INFORMATION.
    VerifiedProcess process = openVerified(m_Fns,
                                           target,
                                           useCpuSets ? PROCESS_SET_LIMITED_INFORMATION : PROCESS_SET_INFORMATION,
                                           [&target](DWORD error) { return describeAffinityOpenFailure(target, error); });
    if (!process.result.success)
    {
        spdlog::warn("Not setting CPU affinity for PID {}: {}", target.pid, process.result.errorMessage);
        return std::move(process.result);
    }

    BOOL ok = FALSE;
    if (!useCpuSets)
    {
        // One group: masks holds exactly group 0's mask (an empty choice was refused above).
        ok = m_Fns.setProcessAffinityMask(process.handle.get(), static_cast<DWORD_PTR>(masks->front().mask));
    }
    else if (cpus == affinityLayout().online)
    {
        // Every CPU: clear the default CPU sets rather than pin the process to today's processors.
        ok = m_Fns.setProcessDefaultCpuSets(process.handle.get(), nullptr, 0);
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
    }
    if (ok == FALSE)
    {
        std::string msg = affinitySetErrorMessage(GetLastError(), target.pid, useCpuSets);
        spdlog::warn("{}", msg);
        return ProcessActionResult::error(std::move(msg));
    }
    spdlog::info("Set CPU affinity ({} CPUs{}) for PID {}", cpus.count(), useCpuSets ? ", as CPU sets" : "", target.pid);
    return ProcessActionResult::ok();
}

CpuAffinityLayout WindowsProcessActions::affinityLayout() const
{
    const std::vector<ProcessorCoreRecord> cores = (m_Fns.readCores != nullptr) ? m_Fns.readCores() : std::vector<ProcessorCoreRecord>{};
    return affinityLayoutFromCores(m_GroupSizes, cores);
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform
