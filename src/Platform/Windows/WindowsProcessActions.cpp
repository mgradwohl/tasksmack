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

#include <format>

namespace Platform
{

ProcessActionCapabilities WindowsProcessActions::actionCapabilities() const
{
    return ProcessActionCapabilities{
        .canTerminate = true, // TerminateProcess
        .canKill = true,      // TerminateProcess (same as terminate on Windows)
        .canStop = false,     // Windows doesn't have SIGSTOP equivalent
        .canContinue = false, // Windows doesn't have SIGCONT equivalent
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

} // namespace Platform
