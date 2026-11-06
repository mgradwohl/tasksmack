#include "WindowsRunProgram.h"

#include "ComScope.h"
#include "Platform/CommandWords.h"
#include "Platform/IProcessActions.h"
#include "WinString.h"
#include "WindowsElevation.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <shellapi.h> // NOLINT(misc-include-cleaner) - ShellExecuteExW, SHELLEXECUTEINFOW, SEE_MASK_*
// clang-format on

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

constexpr const wchar_t* OPEN_VERB = L"open";
constexpr const wchar_t* RUNAS_VERB = L"runas";

// LCOV_EXCL_START - the real calls start a program or raise UAC, which no test may do (#3427)
/// TaskSmack's window when it is in front, which it is while the user presses Run, so a UAC prompt
/// comes up over it rather than behind; else none.
HWND foregroundWindowOfThisProcess() noexcept
{
    HWND window = GetForegroundWindow();
    DWORD owner = 0;
    if (window == nullptr || GetWindowThreadProcessId(window, &owner) == 0 || owner != GetCurrentProcessId())
    {
        return nullptr;
    }
    return window;
}

DWORD realShellExecute(const ShellRun& run)
{
    // Shell extensions may do the work, and they need COM on the calling thread.
    const ComScope com;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // Done before it returns (the worker may run something else next), %VARIABLES% in the file expanded
    // as the Run box does, and no error dialog of the shell's: the error is TaskSmack's to show. No
    // process handle is asked for, so none is held.
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_DOENVSUBST | SEE_MASK_FLAG_NO_UI;
    info.hwnd = foregroundWindowOfThisProcess();
    info.lpVerb = run.verb.c_str();
    info.lpFile = run.file.c_str();
    info.lpParameters = run.parameters.empty() ? nullptr : run.parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info) == FALSE)
    {
        const DWORD error = GetLastError();
        return (error != ERROR_SUCCESS) ? error : static_cast<DWORD>(ERROR_GEN_FAILURE);
    }
    return ERROR_SUCCESS;
}

bool realIsElevated()
{
    return isProcessElevated();
}
// LCOV_EXCL_STOP

/// What @p error from opening @p program means, said as the Run box would.
[[nodiscard]] std::string shellRunFailure(DWORD error, std::string_view program)
{
    switch (error)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
        return std::format("Windows can't find {}. Check the name and try again", program);
    case ERROR_NO_ASSOCIATION:
        return std::format("No app is set to open {}", program);
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH:
        return std::format("{} isn't a program Windows can run", program);
    case ERROR_ELEVATION_REQUIRED:
        return std::format("{} needs administrator rights: tick Run as administrator", program);
    case ERROR_ACCESS_DENIED:
        return std::format("Windows refused to open {}: access is denied", program);
    default:
        return std::system_category().message(static_cast<int>(error));
    }
}

} // namespace

RunProgramFunctions realRunProgramFunctions() noexcept
{
    return {.shellExecute = &realShellExecute, .isElevated = &realIsElevated};
}

const wchar_t* runVerb(bool asAdministrator, bool elevated) noexcept
{
    return (asAdministrator && !elevated) ? RUNAS_VERB : OPEN_VERB;
}

ProcessActionResult runShellProgram(const RunProgramFunctions& fns, bool elevated, const RunProgramRequest& request)
{
    if (fns.shellExecute == nullptr)
    {
        return ProcessActionResult::error("Running a program isn't available on this system");
    }
    const std::expected<CommandWords::ProgramAndParameters, std::string> split = CommandWords::splitProgram(request.commandLine);
    if (!split.has_value())
    {
        return ProcessActionResult::error(split.error());
    }
    const ShellRun run{
        .verb = runVerb(request.asAdministrator, elevated),
        .file = WinString::utf8ToWide(split->program),
        .parameters = WinString::utf8ToWide(split->parameters),
    };
    // utf8ToWide() gives nothing back for text that isn't UTF-8.
    if (run.file.empty() || (run.parameters.empty() && !split->parameters.empty()))
    {
        return ProcessActionResult::error("The command isn't valid text");
    }
    const bool runas = run.verb == RUNAS_VERB;
    const DWORD error = fns.shellExecute(run);
    if (error == ERROR_CANCELLED)
    {
        spdlog::info("Run new task: {} was cancelled", split->program);
        return ProcessActionResult::cancelledByUser(runas ? "the administrator prompt was cancelled" : "it was cancelled");
    }
    if (error != ERROR_SUCCESS)
    {
        std::string message = shellRunFailure(error, split->program);
        spdlog::warn("Run new task: {} (error {})", message, error);
        return ProcessActionResult::error(std::move(message));
    }
    spdlog::info("Run new task: started {}{}", split->program, runas ? " as administrator" : "");
    return ProcessActionResult::ok();
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
