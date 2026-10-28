#pragma once

// Windows' wait chain read (#3084): what each thread of a process waits on, through the Wait Chain
// Traversal API (wct.h), as Task Manager's "Analyze wait chain" and Resource Monitor's do. One
// synchronous WCT session per read; GetThreadWaitChain for each of the process's threads, listed from a
// Toolhelp snapshot, which also names the processes the chains lead into; the session closed when done.
//
// Bounded (WaitChain.h): WAIT_CHAIN_MAX_THREADS threads, WCT_MAX_NODE_COUNT nodes each, and
// WAIT_CHAIN_TIME_BUDGET_MS in all, checked before each thread, as is the stop token. Blocking, so
// WindowsProcessActions::readWaitChains() runs it on the dialog's worker, never the UI thread.
//
// As TaskSmack's user, never through the helper: another user's or an elevated process's threads read
// as access denied when TaskSmack isn't elevated. The calls are a table of function pointers, the
// system's by default, so tests drive the session's open, get and close, and their failures, with fakes
// (tests/Platform/test_WindowsWaitChain.cpp). The decoding is WindowsWaitChainMath.h's.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <wct.h>
// clang-format on

#include "Platform/IProcessActions.h"
#include "Platform/WaitChain.h"
#include "WindowsProcessIdentity.h"

#include <chrono>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// A process's threads, and the name of every process running, from one Toolhelp snapshot.
struct ThreadListing
{
    std::vector<DWORD> threadIds;                            ///< The process's threads, as the snapshot lists them
    std::vector<std::pair<DWORD, std::string>> processNames; ///< Every process's PID and executable file name (UTF-8)
};

/// @p pid's threads and every process's name, from CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD |
/// TH32CS_SNAPPROCESS); nullopt when the snapshot couldn't be taken.
[[nodiscard]] std::optional<ThreadListing> listProcessThreads(DWORD pid);

/// Registers COM's call-state callbacks with WCT (RegisterWaitChainCOMCallback, with ole32's
/// CoGetCallState and CoGetActivationState), so a chain can follow a COM call. Once per process: later
/// calls answer what the first found. False when ole32 or either export couldn't be found.
[[nodiscard]] bool registerWaitChainComCallbacks();

/// The steady clock's now(), the read's time budget's clock.
[[nodiscard]] std::chrono::steady_clock::time_point waitChainClockNow();

/// The calls readWaitChains() makes: advapi32's and kernel32's, or a test's fakes.
struct WaitChainFunctions
{
    using OpenSessionFn = HWCT(WINAPI*)(DWORD, PWAITCHAINCALLBACK);
    using CloseSessionFn = VOID(WINAPI*)(HWCT);
    using GetChainFn = BOOL(WINAPI*)(HWCT, DWORD_PTR, DWORD, DWORD, LPDWORD, PWAITCHAIN_NODE_INFO, LPBOOL);

    /// The target's identity check (openProcessVerified()), and the start time of each process a chain
    /// leads into, for End process.
    ProcessIdentityFunctions identity;
    OpenSessionFn openSession = &::OpenThreadWaitChainSession;
    CloseSessionFn closeSession = &::CloseThreadWaitChainSession;
    GetChainFn getChain = &::GetThreadWaitChain;
    std::optional<ThreadListing> (*listThreads)(DWORD pid) = &listProcessThreads;
    /// Nullable: COM calls are then not followed (the COM flag is left out).
    bool (*registerComCallbacks)() = &registerWaitChainComCallbacks;
    std::chrono::steady_clock::time_point (*now)() = &waitChainClockNow;
};

/// Reads @p target's wait chains through @p fns, as described above. The target's identity is checked
/// first (an unknown start time, a reused PID, an exited process and a refused open are reported as
/// IdentityUnknown, ProcessExited and PermissionDenied); the handle is held for the whole read, so the
/// PID can't be reused meanwhile.
[[nodiscard]] WaitChainReadResult
readWaitChains(const WaitChainFunctions& fns, const ProcessTarget& target, const std::stop_token& stopToken);

/// Thread @p tid's chain from GetThreadWaitChain's @p nodes (WindowsWaitChainMath::decodeChain(), each
/// node copied out of the OS structure: its ObjectName up to its NUL or WCT_OBJNAME_LENGTH, as UTF-8).
[[nodiscard]] ThreadWaitChain decodeWaitChainNodes(DWORD tid, std::span<const WAITCHAIN_NODE_INFO> nodes, bool isCycle, bool truncated);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
