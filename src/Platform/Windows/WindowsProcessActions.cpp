#include "WindowsProcessActions.h"

#include "Domain/PriorityConfig.h"
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

#include <cstdint>
#include <format>
#include <string>
#include <utility>

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
[[nodiscard]] VerifiedProcess openVerified(const ProcessTarget& target, DWORD access, DescribeOpenFailure describeOpenFailure)
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
    Windows::UniqueHandle handle(OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(target.pid)));
    if (!handle)
    {
        return {.handle = {}, .result = ProcessActionResult::error(describeOpenFailure(GetLastError()))};
    }

    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (GetProcessTimes(handle.get(), &creation, &exitTime, &kernelTime, &userTime) == 0)
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

} // namespace

ProcessActionCapabilities WindowsProcessActions::actionCapabilities() const
{
    return ProcessActionCapabilities{
        .canTerminate = true,   // WM_CLOSE to the process's windows: a request it may answer (#1094)
        .canKill = true,        // TerminateProcess
        .canStop = false,       // Windows doesn't have SIGSTOP equivalent
        .canContinue = false,   // Windows doesn't have SIGCONT equivalent
        .canSetPriority = true, // SetPriorityClass
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
    const VerifiedProcess process = openVerified(
        target, SYNCHRONIZE, [&target](DWORD error) { return std::format("Failed to open process {}: error {}", target.pid, error); });
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

ProcessActionResult WindowsProcessActions::terminateProcess(const ProcessTarget& target, uint32_t exitCode)
{
    VerifiedProcess process =
        openVerified(target,
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

    VerifiedProcess process = openVerified(target,
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
} // namespace Platform
