#include "WindowsProcessIdentity.h"

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

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
std::expected<InjectableHandle, IdentityFailure>
openProcessVerified(const ProcessIdentityFunctions& fns, const ProcessTarget& target, DWORD access)
{
    if (target.pid <= 0)
    {
        return std::unexpected(IdentityFailure{.kind = IdentityFailureKind::InvalidPid, .win32Error = 0});
    }
    if (target.startTimeTicks == 0)
    {
        // Unconfirmed identity (Idle, System): refused rather than opened by PID alone.
        return std::unexpected(IdentityFailure{.kind = IdentityFailureKind::IdentityUnknown, .win32Error = 0});
    }

    // Windows APIs take a DWORD PID; the cast from a positive int32_t is safe.
    InjectableHandle process(fns.openProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(target.pid)),
                             InjectedHandleCloser{.close = fns.closeHandle});
    if (!process)
    {
        const DWORD error = GetLastError();
        switch (error)
        {
        case ERROR_ACCESS_DENIED:
            return std::unexpected(IdentityFailure{.kind = IdentityFailureKind::AccessDenied, .win32Error = error});
        case ERROR_INVALID_PARAMETER: // no process has this PID any more
            return std::unexpected(IdentityFailure{.kind = IdentityFailureKind::ProcessGone, .win32Error = error});
        default:
            return std::unexpected(IdentityFailure{.kind = IdentityFailureKind::OpenFailed, .win32Error = error});
        }
    }

    // The identity, from the open handle (which pins the process): the process checked is the one used.
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (fns.getProcessTimes(process.get(), &creation, &exitTime, &kernelTime, &userTime) == 0)
    {
        return std::unexpected(IdentityFailure{.kind = IdentityFailureKind::TimesFailed, .win32Error = GetLastError()});
    }
    const std::uint64_t actualTicks =
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | static_cast<std::uint64_t>(creation.dwLowDateTime);
    if (actualTicks != target.startTimeTicks)
    {
        return std::unexpected(
            IdentityFailure{.kind = IdentityFailureKind::Mismatch, .win32Error = 0}); // the PID now names another process
    }

    // The target, but exited and only held open by someone: every caller reports it as exited.
    DWORD exitCode = STILL_ACTIVE;
    if (fns.getExitCodeProcess != nullptr && fns.getExitCodeProcess(process.get(), &exitCode) != 0 && exitCode != STILL_ACTIVE)
    {
        return std::unexpected(IdentityFailure{.kind = IdentityFailureKind::Exited, .win32Error = 0});
    }
    return process;
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
