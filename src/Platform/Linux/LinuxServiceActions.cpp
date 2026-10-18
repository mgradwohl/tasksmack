#include "LinuxServiceActions.h"

#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/Linux/SystemdBus.h"

#include <filesystem>
#include <stop_token>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace Platform
{

LinuxServiceActions::LinuxServiceActions(const std::filesystem::path& root, bool built, SystemdBus::UnitWait wait) : m_Wait(wait)
{
    // As sd_booted() decides it: systemd is the init system when /run/systemd/system exists.
    std::error_code ec;
    m_Available = built && std::filesystem::is_directory(root / "run" / "systemd" / "system", ec);
}

ServiceActionCapabilities LinuxServiceActions::capabilities() const
{
    ServiceActionCapabilities caps;
    caps.canStart = m_Available;
    caps.canStop = m_Available;
    caps.canRestart = m_Available;
    caps.canSetStartType = m_Available;
    caps.canDelayStart = false; // systemd has no delayed automatic start (#2006)
    caps.elevated = ::geteuid() == 0;
    caps.canAuthorizeInteractively = m_Available; // polkit, through systemd (#2304)
    return caps;
}

ServiceActionResult LinuxServiceActions::start(std::string_view name, const std::stop_token& stopToken)
{
    if (!m_Available)
    {
        return ServiceActionResult::failed("systemd isn't running");
    }
    return SystemdBus::startUnit(name, stopToken, m_Wait);
}

ServiceActionResult LinuxServiceActions::stop(std::string_view name, const std::stop_token& stopToken)
{
    if (!m_Available)
    {
        return ServiceActionResult::failed("systemd isn't running");
    }
    return SystemdBus::stopUnit(name, stopToken, m_Wait);
}

ServiceActionResult LinuxServiceActions::restart(std::string_view name, const std::stop_token& stopToken)
{
    // stop(), then start(), as the interface has it: so a restart, like a stop, never takes running
    // dependents down, and a stop request between the two skips the start.
    ServiceActionResult stopped = stop(name, stopToken);
    if (!stopped.ok)
    {
        return stopped;
    }
    if (stopToken.stop_requested())
    {
        return ServiceActionResult::stopRequested();
    }
    return start(name, stopToken);
}

ServiceActionResult LinuxServiceActions::setStartType(std::string_view name, ServiceStartType startType)
{
    if (!m_Available)
    {
        return ServiceActionResult::failed("systemd isn't running");
    }
    return SystemdBus::setUnitStartType(name, startType);
}

ServiceActionResult LinuxServiceActions::runAuthorized(ServiceAction action,
                                                       std::string_view name,
                                                       ServiceStartType startType,
                                                       const std::stop_token& stopToken)
{
    if (!m_Available)
    {
        return ServiceActionResult::failed("systemd isn't running");
    }
    constexpr bool PROMPT = true;
    switch (action)
    {
    case ServiceAction::Start:
        return SystemdBus::startUnit(name, stopToken, m_Wait, PROMPT);
    case ServiceAction::Stop:
        return SystemdBus::stopUnit(name, stopToken, m_Wait, PROMPT);
    case ServiceAction::Restart:
    {
        // As restart(): stop, then start, both allowed to prompt (polkit keeps the first answer for the
        // second with its usual auth_admin_keep), and a stop request between the two skips the start.
        ServiceActionResult stopped = SystemdBus::stopUnit(name, stopToken, m_Wait, PROMPT);
        if (!stopped.ok)
        {
            return stopped;
        }
        if (stopToken.stop_requested())
        {
            return ServiceActionResult::stopRequested();
        }
        return SystemdBus::startUnit(name, stopToken, m_Wait, PROMPT);
    }
    case ServiceAction::SetStartType:
        return SystemdBus::setUnitStartType(name, startType, PROMPT, stopToken);
    }
    return ServiceActionResult::failed("Unknown action");
}

} // namespace Platform
