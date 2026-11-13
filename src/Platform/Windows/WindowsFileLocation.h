#pragma once

// Open file location on Windows (#3426): Explorer opened on a process's executable, selected, as Task
// Manager's "Open file location". The shell's SHOpenFolderAndSelectItems first, then, if that fails,
// explorer.exe /select,"<path>". Each call returns once Explorer has been asked, never waiting for it,
// and runs on the row action worker, never the UI thread. The calls are a table so tests can make
// any fail without starting Explorer.
//
// An elevated TaskSmack (#3669) does neither: either could start an Explorer window as administrator, with every
// shell extension it loads and everything it can start. It asks the signed-in user's Explorer to run
// explorer.exe /select instead (Platform/Windows/UnelevatedShell.h), and when that can't be done it opens
// nothing: the result names the file (ProcessActionResult::withheldPath) for the user to open.

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
    /// Whether TaskSmack runs elevated (#3669). Null: it doesn't.
    bool (*isElevated)() = nullptr;
    /// Runs `explorer.exe /select,"<path>"` (@p commandArguments, explorerSelectArguments()) through the signed-in
    /// user's Explorer (UnelevatedShell::run()), unelevated. Empty, or why not. Null: there is no such way.
    std::string (*selectUnelevated)(const std::wstring& explorer, const std::wstring& commandArguments) = nullptr;
};

/// The real calls.
[[nodiscard]] FileLocationFunctions realFileLocationFunctions() noexcept;

/// explorer.exe's command line for @p path selected: `explorer.exe /select,"<path>"`. The path is quoted
/// whole, so spaces and commas in it stay one argument; Windows paths can't hold a '"', which is why
/// revealInExplorer() refuses one rather than escaping it.
[[nodiscard]] std::wstring explorerSelectCommandLine(std::wstring_view path);

/// Its arguments alone, for the unelevated run (#3669): `/select,"<path>"`.
[[nodiscard]] std::wstring explorerSelectArguments(std::wstring_view path);

/// The result line for a file an elevated TaskSmack didn't show (#3669), naming @p utf8Path; withheldPath is set.
[[nodiscard]] ProcessActionResult withheldFileLocation(const std::string& utf8Path);

/// Shows @p path selected in Explorer through @p fns: SHOpenFolderAndSelectItems, then explorer.exe
/// /select if that fails. ok() once either has been asked; an error saying what failed otherwise.
/// Elevated (fns.isElevated()), only through the signed-in user's Explorer (fns.selectUnelevated), and
/// withheldFileLocation() when that fails or there is none: never an elevated Explorer (#3669).
[[nodiscard]] ProcessActionResult revealInExplorer(const FileLocationFunctions& fns, const std::wstring& path);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
