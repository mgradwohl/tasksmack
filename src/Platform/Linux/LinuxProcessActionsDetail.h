#pragma once

// What LinuxProcessControl (the process-control half, linked into tasksmack-helper) and
// LinuxProcessActions (which adds the system call trace and dump files) both need (#2640). Internal to
// those two; defined in LinuxProcessControl.cpp.

#include "LinuxProcessControl.h"
#include "Platform/IProcessActions.h"
#include "PosixGuards.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Platform::LinuxProcessDetail
{

/// Result of trying to open a pidfd: the descriptor, or the message to refuse the action with.
struct PidfdOpen
{
    int fd = -1;         ///< The pidfd, or -1.
    std::string refusal; ///< Why there is none; empty when fd is valid.
};

/// Open a pidfd for `pid`.
///
/// Without one there is no way to act on the checked process rather than on whatever holds its PID
/// a moment later, so the actions fail closed: they refuse rather than fall back to the bare PID,
/// as they refuse an unknown start time (#973). The supported Linux target (Ubuntu 24.04, kernel
/// 6.x) always has pidfd_open, so this only refuses on old kernels or under a seccomp filter.
[[nodiscard]] PidfdOpen openPidfd(const LinuxProcessSyscalls& syscalls, std::int32_t pid);

/// Confirm the process holding the target's PID now is the one the target names.
[[nodiscard]] ProcessActionResult verifyIdentity(const ProcessTarget& target);

/// Send `signal` through `pidfd`; 0 on success, -1 with errno set on failure.
[[nodiscard]] inline int sendThroughPidfd(const LinuxProcessSyscalls& syscalls, const Posix::FdGuard& pidfd, int signal)
{
    return syscalls.pidfdSendSignal(pidfd.get(), signal);
}

/// Up to 4 KiB of a small /proc or /sys file, or nullopt when it can't be read.
[[nodiscard]] std::optional<std::string> readSmallFile(const std::string& path);

} // namespace Platform::LinuxProcessDetail
