#include "Platform/Factory.h"

#include "LinuxDiskProbe.h"
#include "LinuxGPUProbe.h"
#include "LinuxPathProvider.h"
#include "LinuxPowerProbe.h"
#include "LinuxProcessActions.h"
#include "LinuxProcessConnectionsReader.h"
#include "LinuxProcessEnvironmentReader.h"
#include "LinuxProcessProbe.h"
#include "LinuxSystemProbe.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemProbe.h"

#include <memory>

namespace Platform
{

std::unique_ptr<IProcessProbe> makeProcessProbe()
{
    return std::make_unique<LinuxProcessProbe>();
}

std::unique_ptr<IProcessActions> makeProcessActions()
{
    return std::make_unique<LinuxProcessActions>();
}

std::unique_ptr<IProcessEnvironmentReader> makeProcessEnvironmentReader()
{
    return std::make_unique<LinuxProcessEnvironmentReader>();
}

std::unique_ptr<IProcessConnectionsReader> makeProcessConnectionsReader()
{
    return std::make_unique<LinuxProcessConnectionsReader>();
}

std::unique_ptr<IStartupProbe> makeStartupProbe()
{
    // XDG autostart (~/.config/autostart, /etc/xdg/autostart) is the Linux lane's follow-up to #801.
    return std::make_unique<UnsupportedStartupProbe>();
}

std::unique_ptr<ISystemProbe> makeSystemProbe()
{
    return std::make_unique<LinuxSystemProbe>();
}

std::unique_ptr<IDiskProbe> makeDiskProbe()
{
    return std::make_unique<LinuxDiskProbe>();
}

std::unique_ptr<IPathProvider> makePathProvider()
{
    return std::make_unique<LinuxPathProvider>();
}

std::unique_ptr<IPowerProbe> makePowerProbe()
{
    return std::make_unique<LinuxPowerProbe>();
}

std::unique_ptr<IGPUProbe> makeGPUProbe()
{
    return std::make_unique<LinuxGPUProbe>();
}

std::unique_ptr<IServiceProbe> makeServiceProbe()
{
    // systemd (D-Bus org.freedesktop.systemd1) is the Linux lane's follow-up to #800.
    return std::make_unique<UnsupportedServiceProbe>();
}

std::unique_ptr<IServiceActions> makeServiceActions()
{
    // systemd control (D-Bus StartUnit/StopUnit/...) is the Linux lane's follow-up to #1577.
    return std::make_unique<UnsupportedServiceActions>();
}

} // namespace Platform
