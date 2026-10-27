#include "Platform/Factory.h"

#include "LinuxDiskProbe.h"
#include "LinuxGPUProbe.h"
#include "LinuxPathProvider.h"
#include "LinuxPowerProbe.h"
#include "LinuxProcessActions.h"
#include "LinuxProcessConnectionsReader.h"
#include "LinuxProcessEnvironmentReader.h"
#include "LinuxProcessIconSource.h"
#include "LinuxProcessModulesReader.h"
#include "LinuxProcessOpenFilesReader.h"
#include "LinuxProcessProbe.h"
#include "LinuxProcessSecurityReader.h"
#include "LinuxProcessThreadsReader.h"
#include "LinuxServiceActions.h"
#include "LinuxServiceProbe.h"
#include "LinuxStartup.h"
#include "LinuxSystemConnectionsReader.h"
#include "LinuxSystemInfoProbe.h"
#include "LinuxSystemProbe.h"
#include "PkexecBroker.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessIconSource.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemConnections.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/IUdpTrafficCollector.h"

#include <memory>

#include <unistd.h>

namespace Platform
{

std::unique_ptr<IProcessProbe> makeProcessProbe()
{
    return std::make_unique<LinuxProcessProbe>();
}

std::shared_ptr<IUdpTrafficCollector> makeUdpTrafficCollector()
{
    // sock_diag has no UDP byte counters, and Linux has no event source like Windows' (#1101).
    return nullptr;
}

std::unique_ptr<IProcessActions> makeProcessActions()
{
    return std::make_unique<LinuxProcessActions>();
}

std::shared_ptr<IPrivilegedBroker> makePrivilegedBroker(void* /*ownerWindow*/)
{
    // Root needs no helper, and a refusal to root is nothing an administrator can cure. Without the
    // installed helper polkit has no action for it, so nothing is offered (#2308).
    if (::geteuid() == 0 || ::access(PkexecBroker::INSTALLED_HELPER_PATH, X_OK) != 0)
    {
        return nullptr;
    }
    return std::make_shared<PkexecBroker>(PkexecBroker::INSTALLED_HELPER_PATH);
}

std::unique_ptr<IProcessIconSource> makeProcessIconSource()
{
    return std::make_unique<LinuxProcessIconSource>();
}

std::unique_ptr<IProcessEnvironmentReader> makeProcessEnvironmentReader()
{
    return std::make_unique<LinuxProcessEnvironmentReader>();
}

std::unique_ptr<IProcessConnectionsReader> makeProcessConnectionsReader()
{
    return std::make_unique<LinuxProcessConnectionsReader>();
}

std::unique_ptr<IProcessModulesReader> makeProcessModulesReader()
{
    return std::make_unique<LinuxProcessModulesReader>();
}

std::unique_ptr<IProcessThreadsReader> makeProcessThreadsReader()
{
    return std::make_unique<LinuxProcessThreadsReader>();
}

std::unique_ptr<ISystemConnectionsReader> makeSystemConnectionsReader()
{
    return std::make_unique<LinuxSystemConnectionsReader>();
}

std::unique_ptr<IProcessOpenFilesReader> makeProcessOpenFilesReader()
{
    return std::make_unique<LinuxProcessOpenFilesReader>();
}

std::unique_ptr<IProcessSecurityReader> makeProcessSecurityReader()
{
    return std::make_unique<LinuxProcessSecurityReader>();
}

std::unique_ptr<IStartupProbe> makeStartupProbe()
{
    // XDG autostart: ~/.config/autostart, then /etc/xdg/autostart (#2541).
    return std::make_unique<LinuxStartupProbe>();
}

std::unique_ptr<IStartupActions> makeStartupActions()
{
    // A user copy in ~/.config/autostart with Hidden set (#2541).
    return std::make_unique<LinuxStartupActions>();
}

std::unique_ptr<ISystemInfoProbe> makeSystemInfoProbe()
{
    return std::make_unique<LinuxSystemInfoProbe>();
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
    // systemd's services over D-Bus (#2006); says why where there is no systemd to ask.
    return std::make_unique<LinuxServiceProbe>();
}

std::unique_ptr<IServiceActions> makeServiceActions()
{
    // systemd's StartUnit/StopUnit and unit-file calls over D-Bus (#2006 phase 2).
    return std::make_unique<LinuxServiceActions>();
}

} // namespace Platform
