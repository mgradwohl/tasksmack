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

/// A visible top-level window, as the probe's one window walk a sample found it (#3705), with what the Not
/// responding check takes from it. The window AppUserModelID lookup reads the same list, asking each window's
/// process and owner itself only on a sample that looks, so the walk every sample costs what it did alone.
struct TopLevelWindow
{
    HWND window = nullptr;     // NOLINT(misc-include-cleaner) - HWND, from <windows.h>
    std::uint32_t hungPid = 0; ///< The process it shows as not responding (a ghost's: the hung window's); 0 if none
    bool hung = false;         ///< IsHungAppWindow on this window itself
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

/// For each process in @p pids (sorted), the explicit AppUserModelID of the first of its unowned top-level
/// windows in @p windows (this sample's walk, visible windows only; GetWindowThreadProcessId and the owner asked
/// here) that sets one, into @p found (cleared
/// first; at most one per process, in no order). A hung window is skipped. Walks nothing itself (#3705).
void windowAppUserModelIds(std::span<const TopLevelWindow> windows, std::span<const std::uint32_t> pids, std::vector<WindowAppId>& found);

/// The name the shell's Apps folder (shell:AppsFolder) shows for @p appUserModelId, empty when it has none
/// or COM isn't initialised on the calling thread.
[[nodiscard]] std::string appDisplayName(const std::string& appUserModelId);

} // namespace Platform::Windows
