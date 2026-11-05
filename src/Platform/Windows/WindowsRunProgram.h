#pragma once

// Run new task on Windows (#3427), as Task Manager's File > Run new task: the typed command line split as
// the Run box splits it (CommandWords::splitProgram()), then opened with ShellExecuteExW, so a document,
// a folder, a web address or a shell: folder works as well as a program. "Run as administrator" is the
// runas verb, which raises the UAC consent prompt; dismissing it is a cancelled result, not an error.
// Each call returns once Windows has started it, never waiting for it, and runs on the row action worker,
// never the UI thread. The calls are a table so tests can answer as Windows would without starting
// anything or raising UAC.

#include "Platform/IProcessActions.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include <string>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// One ShellExecuteExW call: the verb (L"open", or L"runas" for administrator), what to open, and the
/// parameters passed to it as typed.
struct ShellRun
{
    std::wstring verb;
    std::wstring file;
    std::wstring parameters;
};

/// The calls runShellProgram() makes. realRunProgramFunctions() is the real table.
struct RunProgramFunctions
{
    /// ShellExecuteExW on @p run: environment variables in the file expanded, no error dialog of the
    /// shell's, the work done before it returns, COM on the calling thread for the call (ComScope), and
    /// any UAC prompt in front of TaskSmack's window. 0 once started, or the Win32 error.
    DWORD (*shellExecute)(const ShellRun& run) = nullptr;
    /// Whether TaskSmack's own token is elevated (isProcessElevated()), asked once, at construction. Null
    /// reads as not elevated.
    bool (*isElevated)() = nullptr;
};

/// The real calls.
[[nodiscard]] RunProgramFunctions realRunProgramFunctions() noexcept;

/// The ShellExecuteExW verb for a task (#3427): L"runas" when @p asAdministrator and TaskSmack isn't
/// already @p elevated, else L"open"; an elevated TaskSmack's tasks are elevated already.
[[nodiscard]] const wchar_t* runVerb(bool asAdministrator, bool elevated) noexcept;

/// Starts @p request through @p fns (#3427): splits its command line (CommandWords::splitProgram()), turns
/// it to UTF-16 and opens it with runVerb()'s verb. ok() once Windows has started it; ERROR_CANCELLED
/// (the UAC prompt dismissed) is ProcessActionResult::cancelledByUser(); any other failure an error
/// saying what was wrong (not found, no app for it, not a program).
[[nodiscard]] ProcessActionResult runShellProgram(const RunProgramFunctions& fns, bool elevated, const RunProgramRequest& request);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
