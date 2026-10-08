#pragma once

#include "Platform/IProcessActions.h"

#include <cerrno>
#include <cstdint>
#include <format>
#include <string>
#include <system_error>

namespace Platform
{

/// The message shown when setpriority(2) fails with @p err for process @p pid, asked for nice
/// @p nice. Free of platform calls so the mapping is unit-testable.
///
/// setpriority(2): EPERM means the process belongs to another user; EACCES means raising priority
/// (lowering niceness) without CAP_SYS_NICE. The advice for the two was the other way round (#1155).
/// EACCES is not in POSIX for setpriority(), but the Linux man page documents it.
///
/// The advice is to run TaskSmack with privilege, not `renice -p`: that applies setpriority(2) to
/// the PID alone, which changes only the main thread -- the #1104 bug (#1228 review).
[[nodiscard]] inline std::string priorityErrorMessage(int err, std::int32_t nice, std::int32_t pid)
{
    switch (err)
    {
    case EPERM:
        return std::format(
            "Permission denied: process {} belongs to another user. Run TaskSmack as root to set its nice value to {}.", pid, nice);
    case ESRCH:
        return "Process not found - may have already exited";
    case EACCES:
        return std::format(
            "Permission denied: raising priority (nice {} for process {}) needs root or CAP_SYS_NICE. Run TaskSmack as root.", nice, pid);
    default:
        return std::system_category().message(err);
    }
}

/// The message shown when ioprio_set(2) fails with @p err for process @p pid, asked for class
/// @p ioClass (#803). Free of platform calls so the mapping is unit-testable, as priorityErrorMessage().
///
/// ioprio_set(2) gives EPERM for both causes it has: the Realtime class without CAP_SYS_NICE or
/// CAP_SYS_ADMIN (ioprio_check_cap() accepts either), and a process of another user without
/// CAP_SYS_NICE. A Realtime request is told the first, since that check comes first in the kernel;
/// anything else the second. Both name the least privilege that suffices, CAP_SYS_NICE, rather than
/// running the whole app as root.
[[nodiscard]] inline std::string ioPriorityErrorMessage(int err, IoPriorityClass ioClass, std::int32_t pid)
{
    switch (err)
    {
    case EPERM:
        if (ioClass == IoPriorityClass::Realtime)
        {
            return std::format("Permission denied: the Realtime I/O class needs CAP_SYS_NICE (or root) to set it for process {}.", pid);
        }
        return std::format("Permission denied: process {} belongs to another user. Changing its I/O priority needs CAP_SYS_NICE (or root).",
                           pid);
    case ESRCH:
        return "Process not found - may have already exited";
    case EINVAL:
        return std::format("Invalid I/O priority class or level for process {}", pid);
    default:
        return std::system_category().message(err);
    }
}

} // namespace Platform
