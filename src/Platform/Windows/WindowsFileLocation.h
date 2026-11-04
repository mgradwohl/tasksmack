#pragma once

// Open file location on Windows (#3426): Explorer opened on a process's executable, selected, as Task
// Manager's "Open file location". The shell's SHOpenFolderAndSelectItems first, then, if that fails,
// explorer.exe /select,"<path>". Each call returns once Explorer has been asked, never waiting for it,
// and runs on the row action worker, never the UI thread. The two calls are a table so tests can make
// either fail without starting Explorer.

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
#include <string_view>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The calls revealInExplorer() makes. realFileLocationFunctions() is the real table.
struct FileLocationFunctions
{
    /// Explorer's window on @p path's folder with @p path selected: COM on the calling thread for the call
    /// (ComScope), then ILCreateFromPathW, SHOpenFolderAndSelectItems and ILFree. S_OK, or why not.
    HRESULT (*selectInFolder)(const std::wstring& path) = nullptr;
    /// Starts @p application with @p commandLine (CreateProcessW, which may write to it), detached: its
    /// handles are closed at once and it is never waited for. 0, or the Win32 error.
    DWORD (*startProcess)(const std::wstring& application, std::wstring& commandLine) = nullptr;
    /// explorer.exe's full path, in the Windows folder; empty when that can't be read.
    std::wstring (*explorerPath)() = nullptr;
};

/// The real calls.
[[nodiscard]] FileLocationFunctions realFileLocationFunctions() noexcept;

/// explorer.exe's command line for @p path selected: `explorer.exe /select,"<path>"`. The path is quoted
/// whole, so spaces and commas in it stay one argument; Windows paths can't hold a '"', which is why
/// revealInExplorer() refuses one rather than escaping it.
[[nodiscard]] std::wstring explorerSelectCommandLine(std::wstring_view path);

/// Shows @p path selected in Explorer through @p fns: SHOpenFolderAndSelectItems, then explorer.exe
/// /select if that fails. ok() once either has been asked; an error saying what failed otherwise.
[[nodiscard]] ProcessActionResult revealInExplorer(const FileLocationFunctions& fns, const std::wstring& path);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
