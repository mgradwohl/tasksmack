#pragma once

// The one place Windows opens a process by PID and confirms it is the process a ProcessTarget names
// (#2149). Every action (WindowsProcessActions) and every Process Details reader (modules, open
// files, connections, security) goes through openProcessVerified(), so a change to how identity is
// checked is made once.

#include "Platform/IProcessActions.h"
#include "WindowsHandles.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// Why openProcessVerified() returned no handle.
enum class IdentityFailureKind : std::uint8_t
{
    InvalidPid,      ///< The target's PID is 0 or negative; nothing was opened.
    IdentityUnknown, ///< The target's start time is unknown (0): refused rather than opened by PID alone.
    AccessDenied,    ///< OpenProcess failed with ERROR_ACCESS_DENIED (protected, elevated, another user's).
    ProcessGone,     ///< OpenProcess failed with ERROR_INVALID_PARAMETER: no process has this PID any more.
    OpenFailed,      ///< OpenProcess failed with any other error.
    TimesFailed,     ///< The process opened, but GetProcessTimes failed, so its identity could not be confirmed.
    Mismatch,        ///< The process holding the PID started at another time: the target exited and the PID was reused.
    Exited,          ///< The target is the process named, but it has exited; its object is only held open (#2206).
};

/// What openProcessVerified() refused, and the Win32 error behind it (0 for the kinds no call failed for).
struct IdentityFailure
{
    IdentityFailureKind kind = IdentityFailureKind::OpenFailed;
    DWORD win32Error = 0;
};

/// The calls openProcessVerified() makes: kernel32's, or a test's fakes (each reader passes its own
/// seam's). The handle returned is closed through closeHandle, so a fake handle never reaches the
/// real CloseHandle; a seam whose fakes hand out anything but real process handles passes its own
/// getExitCodeProcess for the same reason.
struct ProcessIdentityFunctions
{
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using GetProcessTimesFn = BOOL(WINAPI*)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    using GetExitCodeProcessFn = BOOL(WINAPI*)(HANDLE, LPDWORD);

    OpenProcessFn openProcess = &::OpenProcess;
    GetProcessTimesFn getProcessTimes = &::GetProcessTimes;
    InjectedHandleCloser::CloseHandleFn closeHandle = &::CloseHandle;
    GetExitCodeProcessFn getExitCodeProcess = &::GetExitCodeProcess; ///< null: the exit check is skipped
};

/// Open `target` with `access` and confirm it is the process the target names.
///
/// PROCESS_QUERY_LIMITED_INFORMATION is always added to `access`: GetProcessTimes needs it. The
/// creation time is read from the opened handle, not looked up by PID, and the handle pins the
/// process object until it is closed: so the process checked is the process used, with no window
/// for the PID to be reused in between. GetProcessTimes reports the same creation time the probe
/// reads from SYSTEM_PROCESS_INFORMATION::CreateTime, in the same 100ns FILETIME units
/// (ProcessTarget::startTimeTicks).
///
/// An unknown start time is refused, never skipped, as checkProcessIdentity() refuses it. A Win32
/// error is captured straight after the call that failed.
///
/// A process that has exited but whose object someone still holds open (a parent that has not
/// reaped it, a debugger) keeps its PID and creation time, so it passes the identity check: it is
/// refused as Exited when GetExitCodeProcess reports an exit code other than STILL_ACTIVE (#2206).
/// A failed GetExitCodeProcess, or a real exit code of 259 (STILL_ACTIVE), reads as running.
[[nodiscard]] std::expected<InjectableHandle, IdentityFailure>
openProcessVerified(const ProcessIdentityFunctions& fns, const ProcessTarget& target, DWORD access);

/// A Process Details reader's status and detail for a refused identity check. `Status` is one of the
/// readers' *ReadStatus enums, which share these names.
template<typename Status> struct IdentityReadFailure
{
    Status status;
    std::string detail; ///< The system's message for the Win32 error, for Failed; else empty.
};

/// How a Process Details reader reports `failure`: an invalid PID, a vanished process, a reused PID
/// and an exited process still held open read as ProcessExited; an unknown start time as
/// IdentityUnknown; a refused open as PermissionDenied; any other open error, or a failed
/// GetProcessTimes, as Failed with the system's message.
template<typename Status> [[nodiscard]] IdentityReadFailure<Status> toReadFailure(const IdentityFailure& failure)
{
    switch (failure.kind)
    {
    case IdentityFailureKind::IdentityUnknown:
        return {.status = Status::IdentityUnknown, .detail = {}};
    case IdentityFailureKind::AccessDenied:
        return {.status = Status::PermissionDenied, .detail = {}};
    case IdentityFailureKind::OpenFailed:
    case IdentityFailureKind::TimesFailed:
        return {.status = Status::Failed, .detail = std::system_category().message(static_cast<int>(failure.win32Error))};
    case IdentityFailureKind::InvalidPid:
    case IdentityFailureKind::ProcessGone:
    case IdentityFailureKind::Mismatch:
    case IdentityFailureKind::Exited:
        break;
    }
    return {.status = Status::ProcessExited, .detail = {}};
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
