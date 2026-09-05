#include "WindowsProcessActions.h"

#include "Domain/PriorityConfig.h"
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
#include <utility>

namespace Platform
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
ProcessActionCapabilities WindowsProcessActions::actionCapabilities() const
{
    return ProcessActionCapabilities{
        .canTerminate = true,   // TerminateProcess
        .canKill = true,        // TerminateProcess (same as terminate on Windows)
        .canStop = false,       // Windows doesn't have SIGSTOP equivalent
        .canContinue = false,   // Windows doesn't have SIGCONT equivalent
        .canSetPriority = true, // SetPriorityClass
    };
}

ProcessActionResult WindowsProcessActions::terminate(int32_t pid)
{
    spdlog::info("Terminating process {}", pid);
    return terminateProcess(pid, 1);
}

ProcessActionResult WindowsProcessActions::kill(int32_t pid)
{
    // On Windows, kill is the same as terminate
    spdlog::info("Killing process {}", pid);
    return terminateProcess(pid, 9);
}

ProcessActionResult WindowsProcessActions::stop(int32_t pid)
{
    // Windows doesn't have a direct equivalent to SIGSTOP
    // Could potentially use SuspendThread on all threads, but that's complex
    spdlog::warn("Stop not supported on Windows for process {}", pid);
    return ProcessActionResult::error("Stop (SIGSTOP) is not supported on Windows");
}

ProcessActionResult WindowsProcessActions::resume(int32_t pid)
{
    // Windows doesn't have a direct equivalent to SIGCONT
    spdlog::warn("Resume not supported on Windows for process {}", pid);
    return ProcessActionResult::error("Resume (SIGCONT) is not supported on Windows");
}

ProcessActionResult WindowsProcessActions::terminateProcess(int32_t pid, uint32_t exitCode)
{
    // Note: Windows APIs require DWORD for PIDs; explicit cast from int32_t is safe.
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));

    if (hProcess == nullptr)
    {
        DWORD error = GetLastError();
        std::string msg = std::format("Failed to open process {}: error {}", pid, error);
        spdlog::error("{}", msg);
        return ProcessActionResult::error(std::move(msg));
    }

    const BOOL result = TerminateProcess(hProcess, exitCode);
    const DWORD error = GetLastError();
    CloseHandle(hProcess);

    if (result == 0)
    {
        std::string msg = std::format("Failed to terminate process {}: error {}", pid, error);
        spdlog::error("{}", msg);
        return ProcessActionResult::error(std::move(msg));
    }

    spdlog::info("Successfully terminated process {} with exit code {}", pid, exitCode);
    return ProcessActionResult::ok();
}

ProcessActionResult WindowsProcessActions::setPriority(int32_t pid, int32_t nice)
{
    if (pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }

    // Clamp nice value to valid range for consistency with Linux
    const int32_t clampedNice = Domain::Priority::clampNice(nice);

    const uint32_t priorityClass = niceToPriorityClass(clampedNice);
    spdlog::debug("Setting priority class {} (nice={}) for PID {}", priorityClass, clampedNice, pid);

    // Note: Windows APIs require DWORD for PIDs; explicit cast from int32_t is safe.
    HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(pid));

    if (hProcess == nullptr)
    {
        DWORD error = GetLastError();
        std::string msg;
        switch (error)
        {
        case ERROR_ACCESS_DENIED:
            msg = "Permission denied - cannot change priority of this process";
            break;
        case ERROR_INVALID_PARAMETER:
            msg = "Process not found - may have already exited";
            break;
        default:
            msg = std::format("Failed to open process {}: error {}", pid, error);
            break;
        }
        spdlog::warn("{}", msg);
        return ProcessActionResult::error(std::move(msg));
    }

    const BOOL result = SetPriorityClass(hProcess, priorityClass);
    const DWORD error = result == 0 ? GetLastError() : 0; // Capture error before CloseHandle
    CloseHandle(hProcess);

    if (result == 0)
    {
        std::string msg = std::format("Failed to set priority for process {}: error {}", pid, error);
        spdlog::warn("{}", msg);
        return ProcessActionResult::error(std::move(msg));
    }

    spdlog::info("Successfully set priority (nice={}) for PID {}", clampedNice, pid);
    return ProcessActionResult::ok();
}

// NOLINTEND(misc-include-cleaner)
} // namespace Platform
