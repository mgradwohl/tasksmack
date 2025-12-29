#include "LinuxProcessActions.h"

#include "Domain/PriorityConfig.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <csignal> // POSIX signals
#include <system_error>

#include <sys/resource.h>

namespace Platform
{

ProcessActionCapabilities LinuxProcessActions::actionCapabilities() const
{
    return {
        .canTerminate = true,
        .canKill = true,
        .canStop = true,
        .canContinue = true,
        .canSetPriority = true,
    };
}

ProcessActionResult LinuxProcessActions::terminate(int32_t pid)
{
    return sendSignal(pid, SIGTERM, "SIGTERM");
}

ProcessActionResult LinuxProcessActions::kill(int32_t pid)
{
    return sendSignal(pid, SIGKILL, "SIGKILL");
}

ProcessActionResult LinuxProcessActions::stop(int32_t pid)
{
    return sendSignal(pid, SIGSTOP, "SIGSTOP");
}

ProcessActionResult LinuxProcessActions::resume(int32_t pid)
{
    return sendSignal(pid, SIGCONT, "SIGCONT");
}

ProcessActionResult LinuxProcessActions::setPriority(int32_t pid, int32_t nice)
{
    if (pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }

    // Clamp nice value to valid range
    const int32_t clampedNice = Domain::Priority::clampNice(nice);

    spdlog::debug("Setting priority (nice={}) for PID {}", clampedNice, pid);

    // Clear errno before call since setpriority can legitimately return -1 on success
    // when the current nice value is -1, so we must check errno to distinguish success from failure.
    errno = 0;
    const int result = setpriority(PRIO_PROCESS, static_cast<id_t>(pid), clampedNice);
    if (result == 0 || (result == -1 && errno == 0))
    {
        spdlog::info("Successfully set priority (nice={}) for PID {}", clampedNice, pid);
        return ProcessActionResult::ok();
    }

    // Handle error
    const int err = errno;
    std::string errorMsg;

    switch (err)
    {
    case EPERM:
        errorMsg = "Permission denied - cannot lower priority without root privileges";
        break;
    case ESRCH:
        errorMsg = "Process not found - may have already exited";
        break;
    case EACCES:
        errorMsg = "Permission denied - cannot change priority of this process";
        break;
    default:
        errorMsg = std::system_category().message(err);
        break;
    }

    spdlog::warn("Failed to set priority for PID {}: {}", pid, errorMsg);
    return ProcessActionResult::error(errorMsg);
}

ProcessActionResult LinuxProcessActions::sendSignal(int32_t pid, int signal, std::string_view signalName)
{
    if (pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }

    spdlog::debug("Sending {} to PID {}", signalName, pid);

    if (::kill(pid, signal) == 0)
    {
        spdlog::info("Successfully sent {} to PID {}", signalName, pid);
        return ProcessActionResult::ok();
    }

    // Handle error
    const int err = errno;
    std::string errorMsg;

    switch (err)
    {
    case EPERM:
        errorMsg = "Permission denied - process belongs to another user";
        break;
    case ESRCH:
        errorMsg = "Process not found - may have already exited";
        break;
    case EINVAL:
        errorMsg = "Invalid signal";
        break;
    default:
        errorMsg = std::system_category().message(err);
        break;
    }

    spdlog::warn("Failed to send {} to PID {}: {}", signalName, pid, errorMsg);
    return ProcessActionResult::error(errorMsg);
}

} // namespace Platform
