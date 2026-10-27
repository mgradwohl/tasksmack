#include "Platform/Factory.h"

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
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/Windows/HelperLauncher.h"
#include "Platform/Windows/PipeBroker.h"
#include "WindowsDebugPrivilege.h"
#include "WindowsDiskProbe.h"
#include "WindowsElevation.h"
#include "WindowsEtwNetCollector.h"
#include "WindowsGPUProbe.h"
#include "WindowsPathProvider.h"
#include "WindowsPowerProbe.h"
#include "WindowsProcessActions.h"
#include "WindowsProcessConnections.h"
#include "WindowsProcessEnvironmentReader.h"
#include "WindowsProcessIconSource.h"
#include "WindowsProcessModules.h"
#include "WindowsProcessOpenFiles.h"
#include "WindowsProcessProbe.h"
#include "WindowsProcessSecurity.h"
#include "WindowsServiceActions.h"
#include "WindowsServiceProbe.h"
#include "WindowsStartupActions.h"
#include "WindowsStartupProbe.h"
#include "WindowsSystemInfoProbe.h"
#include "WindowsSystemProbe.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace Platform
{

std::unique_ptr<IProcessProbe> makeProcessProbe()
{
    return std::make_unique<WindowsProcessProbe>();
}

std::shared_ptr<IUdpTrafficCollector> makeUdpTrafficCollector()
{
    return std::make_shared<WindowsEtwNetCollector>();
}

std::unique_ptr<IProcessActions> makeProcessActions()
{
    return std::make_unique<WindowsProcessActions>();
}

std::shared_ptr<IPrivilegedBroker> makePrivilegedBroker(void* ownerWindow)
{
    // As on Linux, nothing is offered that can't work (#2550): an elevated TaskSmack needs no helper, a
    // build without TaskSmackHelper.exe has none, and the helper refuses a TaskSmack its user can change
    // (#2572), which a portable or per-user copy is. That is asked now, before any UAC prompt.
    if (const auto available = Windows::HelperLauncher::availability(); !available.has_value())
    {
        spdlog::info("Acting as an administrator isn't offered: {}", available.error());
        return nullptr;
    }
    // A standard user's elevation runs as an administrator, which reads only the user's own processes:
    // no "Read as administrator" is offered there; actions still are (#2550). An administrator's
    // elevation that turns out to run as another account too (Windows 11's Administrator Protection, a
    // credentials prompt answered with another administrator's account) is learned from the helper's
    // first refusal, and then isn't offered either (#2769): until a new helper starts, since a later
    // prompt may be answered with the user's own account, or, under Administrator Protection, where every
    // prompt runs the helper as another account, while TaskSmack runs (#2993).
    const auto lasts = Windows::HelperLauncher::administratorProtection() ? Windows::HelperLauncher::RefusalLasts::ForTheProcess
                                                                          : Windows::HelperLauncher::RefusalLasts::ForItsHelper;
    return std::make_shared<Windows::HelperLauncher::LearnedReadsBroker>(
        std::make_unique<Windows::PipeBroker>(Windows::HelperLauncher::makeLauncher({}, static_cast<HWND>(ownerWindow)),
                                              Windows::HelperLauncher::readsOtherUsers()),
        lasts);
}

std::unique_ptr<IProcessIconSource> makeProcessIconSource()
{
    return std::make_unique<WindowsProcessIconSource>();
}

std::unique_ptr<IProcessEnvironmentReader> makeProcessEnvironmentReader()
{
    // The target's environment block, read out of its PEB with ReadProcessMemory (#2569).
    return std::make_unique<Windows::WindowsProcessEnvironmentReader>();
}

std::unique_ptr<IProcessConnectionsReader> makeProcessConnectionsReader()
{
    // GetExtendedTcpTable/GetExtendedUdpTable owner-PID rows (#1489).
    return std::make_unique<Windows::WindowsProcessConnectionsReader>();
}

std::unique_ptr<IProcessModulesReader> makeProcessModulesReader()
{
    return std::make_unique<Windows::WindowsProcessModulesReader>();
}

std::unique_ptr<IProcessThreadsReader> makeProcessThreadsReader()
{
    // The Threads section (#3069) is hidden until the Windows lane adds a Toolhelp reader.
    return std::make_unique<UnsupportedProcessThreadsReader>();
}

std::unique_ptr<IProcessOpenFilesReader> makeProcessOpenFilesReader()
{
    return std::make_unique<Windows::WindowsProcessOpenFilesReader>();
}

std::unique_ptr<IProcessSecurityReader> makeProcessSecurityReader()
{
    return std::make_unique<Windows::WindowsProcessSecurityReader>();
}

std::unique_ptr<IStartupProbe> makeStartupProbe()
{
    return std::make_unique<WindowsStartupProbe>();
}

std::unique_ptr<IStartupActions> makeStartupActions()
{
    return std::make_unique<WindowsStartupActions>(Windows::isProcessElevated());
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
    return std::make_unique<WindowsServiceActions>(Windows::isProcessElevated());
}

bool enableDebugPrivilege()
{
    return Windows::enableDebugPrivilegeIfElevated() == Windows::DebugPrivilegeResult::Enabled;
}

} // namespace Platform
