#pragma once

// Opening a link, file or folder as the signed-in user from an elevated TaskSmack (#3658, #2728's design).
// ShellExecuteW in an elevated process starts the browser, editor or Explorer window elevated too, and with
// it whatever that program loads: a web page's downloads, an editor's plugins, a shell extension, and a File
// Open dialog that can start anything. So an elevated TaskSmack asks Explorer to do the open instead, the
// documented "ShellWindows / IShellDispatch2" technique:
//   - IShellWindows::FindWindowSW(SWC_DESKTOP) finds the desktop's window in Explorer;
//   - its top-level browser's active IShellView gives the desktop folder view's automation object
//     (GetItemObject(SVGIO_BACKGROUND)), an IShellFolderViewDual;
//   - its Application is Explorer's IShellDispatch2, whose ShellExecute runs in Explorer, at the
//     signed-in user's (unelevated) integrity level.
// Each step is a COM call into Explorer's process, so it blocks: App::PlatformOpen calls this on its open
// worker (App::BackgroundOpen), never on the UI thread (#3155). It fails when Explorer isn't running (another
// shell, or Explorer restarting); the caller never falls back to opening elevated.
//
// The real call starts a program, so no test makes it; the choice between this and ShellExecuteW is tested
// in App::PlatformOpen::Detail::openRouted() (test_PlatformOpenRouting.cpp).

#ifdef _WIN32

#include <expected>
#include <string>

namespace Platform::Windows::UnelevatedShell
{

/// Opens @p target (a URL, file or folder, as ShellExecuteW's lpFile) with its default handler through
/// Explorer's shell, so the handler runs as the signed-in user, unelevated. Blocks while Explorer answers.
/// The error names the step that failed: Explorer not running, or its answer.
[[nodiscard]] std::expected<void, std::string> open(const std::wstring& target);

} // namespace Platform::Windows::UnelevatedShell

#endif
