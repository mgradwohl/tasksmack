#pragma once

// The app a Windows process belongs to (#3457), for the Processes table's Group: App and Process Details'
// App row: a packaged app's package family name and AppUserModelID (GetPackageFamilyName,
// GetApplicationUserModelId on a limited-query handle), a desktop app's explicit AppUserModelID from its
// top-level window (SHGetPropertyStoreForWindow, PKEY_AppUserModel_ID), and the name the Start menu's
// Apps folder gives an AppUserModelID ("Windows Terminal"). WindowsProcessProbe calls these through
// WindowsProcessProbeFunctions::App on its sampler thread, never the UI thread, and caches the answers
// per process.

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; HANDLE comes from a sub-header
// clang-format on

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Platform::Windows
{

/// The package a process runs in: both empty for an unpackaged process, or one whose package can't be read.
struct PackageIdentity
{
    std::string family;         ///< "Microsoft.WindowsTerminal_8wekyb3d8bbwe"
    std::string appUserModelId; ///< "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"; may be empty in a packaged process
};

/// The AppUserModelID a process's top-level window sets.
struct WindowAppId
{
    std::uint32_t pid = 0;
    std::string appUserModelId;
};

/// One EnumWindows walk's per-window work, for guardedWindowCallback().
struct WindowVisitor
{
    std::function<bool(HWND)> visit; ///< Called for each top-level window; false stops the walk
    bool failed = false;             ///< Whether visit threw, which stopped the walk
};

/// EnumWindows' callback for a WindowVisitor passed as its LPARAM: calls its visit, and catches whatever that
/// throws (an allocation failure, or anything else), so nothing unwinds through user32. A throw stops the walk
/// (FALSE) and sets failed; so does a null visitor or an empty visit.
BOOL CALLBACK guardedWindowCallback(HWND window, LPARAM param) noexcept; // NOLINT(misc-include-cleaner) - Win32 types

/// The package @p process (opened with PROCESS_QUERY_LIMITED_INFORMATION) runs in.
[[nodiscard]] PackageIdentity readPackageIdentity(HANDLE process); // NOLINT(misc-include-cleaner) - HANDLE

/// For each process in @p pids (sorted), the explicit AppUserModelID of the first of its visible, unowned
/// top-level windows that sets one, into @p found (cleared first; at most one per process, in no order).
/// One EnumWindows pass over the desktop (guardedWindowCallback()); a hung window is skipped. A walk that fails
/// part-way (the per-window work threw) finds nothing.
void windowAppUserModelIds(std::span<const std::uint32_t> pids, std::vector<WindowAppId>& found);

/// The name the shell's Apps folder (shell:AppsFolder) shows for @p appUserModelId, empty when it has none
/// or COM isn't initialised on the calling thread.
[[nodiscard]] std::string appDisplayName(const std::string& appUserModelId);

} // namespace Platform::Windows
