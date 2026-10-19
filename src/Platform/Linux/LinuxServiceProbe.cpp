#include "LinuxServiceProbe.h"

#include "Platform/IServiceProbe.h"
#include "Platform/Linux/SystemdBus.h"
#include "Platform/Linux/SystemdServices.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace Platform
{

LinuxServiceProbe::LinuxServiceProbe(Reader reader, const std::filesystem::path& root, bool built)
    : m_Reader(reader ? std::move(reader) : Reader([cache = m_UnitFiles] { return SystemdBus::readServices(cache.get()); }))
{
    std::error_code ec;
    if (!built)
    {
        m_Capabilities.unavailableReason = NOT_BUILT_REASON;
    }
    else if (!std::filesystem::is_directory(root / "run/systemd/system", ec))
    {
        // Not booted with systemd (a container's init, WSL without systemd): no service manager to ask.
        m_Capabilities.unavailableReason = NOT_RUNNING_REASON;
    }
    else
    {
        m_Capabilities.canEnumerate = true;
        m_Capabilities.hasDisplayName = true; // The unit's Description
        m_Capabilities.hasStartType = true;   // From its unit file's state
        m_Capabilities.hasPid = true;         // A running service's MainPID
        // A loaded service's Type, ExecStart and User (#2590); a unit file that isn't loaded has none.
        m_Capabilities.hasServiceType = true;
        m_Capabilities.hasBinaryPath = true;
        m_Capabilities.hasAccount = true;
    }
}

ServiceCapabilities LinuxServiceProbe::capabilities() const
{
    return m_Capabilities;
}

void LinuxServiceProbe::forgetCachedConfig()
{
    m_UnitFiles->forget();
}

ServiceEnumeration LinuxServiceProbe::enumerate()
{
    if (!m_Capabilities.canEnumerate)
    {
        return ServiceEnumeration::failed(m_Capabilities.unavailableReason);
    }
    SystemdBus::ServicesRead read = m_Reader();
    if (!read.ok)
    {
        return ServiceEnumeration::failed(std::move(read.error));
    }
    return ServiceEnumeration::succeeded(SystemdServices::services(read.listing));
}

} // namespace Platform
