#include "WindowsProcessActions.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <format>

namespace Platform
{

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

uint32_t WindowsProcessActions::niceToPriorityClass(int32_t nice)
{
    // Map Unix nice values (-20 to 19) to Windows priority classes.
    // We intentionally never use REALTIME_PRIORITY_CLASS to avoid system instability.
    // nice < -10  -> HIGH_PRIORITY_CLASS
    // nice < -5   -> ABOVE_NORMAL_PRIORITY_CLASS
    // nice < 5    -> NORMAL_PRIORITY_CLASS
    // nice < 15   -> BELOW_NORMAL_PRIORITY_CLASS
    // nice >= 15  -> IDLE_PRIORITY_CLASS
    if (nice < -10)
    {
        return HIGH_PRIORITY_CLASS;
    }
    if (nice < -5)
    {
        return ABOVE_NORMAL_PRIORITY_CLASS;
    }
    if (nice < 5)
    {
        return NORMAL_PRIORITY_CLASS;
    }
    if (nice < 15)
    {
        return BELOW_NORMAL_PRIORITY_CLASS;
    }
    return IDLE_PRIORITY_CLASS;
}

ProcessActionResult WindowsProcessActions::setPriority(int32_t pid, int32_t nice)
{
    if (pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }

    // Clamp nice value to valid range (-20 to 19) for consistency with Linux
    constexpr int32_t MIN_NICE = -20;
    constexpr int32_t MAX_NICE = 19;
    const int32_t clampedNice = std::clamp(nice, MIN_NICE, MAX_NICE);

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
    CloseHandle(hProcess);

    if (result == 0)
    {
        const DWORD error = GetLastError();
        std::string msg = std::format("Failed to set priority for process {}: error {}", pid, error);
        spdlog::warn("{}", msg);
        return ProcessActionResult::error(std::move(msg));
    }

    spdlog::info("Successfully set priority (nice={}) for PID {}", nice, pid);
    return ProcessActionResult::ok();
}

} // namespace Platform
