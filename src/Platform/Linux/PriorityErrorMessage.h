#pragma once

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
[[nodiscard]] inline std::string priorityErrorMessage(int err, std::int32_t nice, std::int32_t pid)
{
    switch (err)
    {
    case EPERM:
        return std::format(
            "Permission denied: the process belongs to another user. Run TaskSmack as root, or use: sudo renice -n {} -p {}", nice, pid);
    case ESRCH:
        return "Process not found - may have already exited";
    case EACCES:
        return std::format(
            "Permission denied. Raising priority (lowering niceness) needs root (CAP_SYS_NICE); use: sudo renice -n {} -p {}", nice, pid);
    default:
        return std::system_category().message(err);
    }
}

} // namespace Platform
