#include "LinuxProcessActions.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <csignal> // POSIX signals
#include <system_error>

namespace Platform
{

ProcessActionCapabilities LinuxProcessActions::actionCapabilities() const
{
    return {
        .canTerminate = true,
        .canKill = true,
        .canStop = true,
        .canContinue = true,
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
