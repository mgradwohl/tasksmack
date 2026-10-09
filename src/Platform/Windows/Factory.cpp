#include "Platform/Factory.h"

#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ISystemProbe.h"
#include "WindowsDiskProbe.h"
#include "WindowsGPUProbe.h"
#include "WindowsPathProvider.h"
#include "WindowsPowerProbe.h"
#include "WindowsProcessActions.h"
#include "WindowsProcessModules.h"
#include "WindowsProcessProbe.h"
#include "WindowsServiceActions.h"
#include "WindowsServiceProbe.h"
#include "WindowsStartupActions.h"
#include "WindowsStartupProbe.h"
#include "WindowsSystemInfoProbe.h"
#include "WindowsSystemProbe.h"

#include <memory>

namespace Platform
{

std::unique_ptr<IProcessProbe> makeProcessProbe()
{
    return std::make_unique<WindowsProcessProbe>();
}

std::unique_ptr<IProcessActions> makeProcessActions()
{
    return std::make_unique<WindowsProcessActions>();
}

std::unique_ptr<IProcessEnvironmentReader> makeProcessEnvironmentReader()
{
    // Reading another process's environment on Windows needs ReadProcessMemory on its PEB: out of
    // scope for now (#179), so the Environment section is hidden.
    return std::make_unique<UnsupportedProcessEnvironmentReader>();
}

std::unique_ptr<IProcessConnectionsReader> makeProcessConnectionsReader()
{
    // GetExtendedTcpTable/GetExtendedUdpTable rows are not wired up yet (#1489), so the Connections
    // section (#799) is hidden.
    return std::make_unique<UnsupportedProcessConnectionsReader>();
}

std::unique_ptr<IProcessModulesReader> makeProcessModulesReader()
{
    return std::make_unique<Windows::WindowsProcessModulesReader>();
}

std::unique_ptr<IStartupProbe> makeStartupProbe()
{
    return std::make_unique<WindowsStartupProbe>();
}

std::unique_ptr<IStartupActions> makeStartupActions()
{
    return std::make_unique<WindowsStartupActions>(WindowsServiceActions::isCurrentProcessElevated());
}

std::unique_ptr<ISystemInfoProbe> makeSystemInfoProbe()
{
    return std::make_unique<WindowsSystemInfoProbe>();
}

std::unique_ptr<ISystemProbe> makeSystemProbe()
{
    return std::make_unique<WindowsSystemProbe>();
}

std::unique_ptr<IDiskProbe> makeDiskProbe()
{
    return std::make_unique<WindowsDiskProbe>();
}

std::unique_ptr<IPathProvider> makePathProvider()
{
    return std::make_unique<WindowsPathProvider>();
}

std::unique_ptr<IPowerProbe> makePowerProbe()
{
    return std::make_unique<WindowsPowerProbe>();
}

std::unique_ptr<IGPUProbe> makeGPUProbe()
{
    return std::make_unique<WindowsGPUProbe>();
}

std::unique_ptr<IServiceProbe> makeServiceProbe()
{
    return std::make_unique<WindowsServiceProbe>();
}

std::unique_ptr<IServiceActions> makeServiceActions()
{
    return std::make_unique<WindowsServiceActions>(WindowsServiceActions::isCurrentProcessElevated());
}

} // namespace Platform
