#include "PanelPlatforms.h"

#include "NativeWindow.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/ServicesPanel.h"
#include "Panels/StartupPanel.h"
#include "Panels/SystemMetricsPanel.h"
#include "Platform/BrokeredProcessActions.h"
#ifdef _WIN32
#include "Platform/BrokeredUserSessions.h" // the Users tab's helper decorator: Windows only (#3076)
#endif
#include "Platform/Factory.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IHttpsClient.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/IUserSessions.h"
#include "SyntheticScenario.h"
#include "TaskManagerReplacementController.h"

#include <memory>
#include <utility>

namespace App
{

namespace
{

/// The broker a test put in place of the installed helper's (Detail::setSharedPrivilegedBrokerForTesting()).
[[nodiscard]] std::shared_ptr<Platform::IPrivilegedBroker>& brokerForTesting()
{
    static std::shared_ptr<Platform::IPrivilegedBroker> broker;
    return broker;
}

/// The one privileged broker every real run's IProcessActions shares (#2308): made once, on first use,
/// and started only when the user first chooses "Authenticate and retry" ("Retry as administrator" on
/// Windows). Null where there is none. Its prompt belongs to the main window (#2550), which exists
/// before any panel asks for this. A test's broker, while one is set, comes first.
[[nodiscard]] const std::shared_ptr<Platform::IPrivilegedBroker>& sharedPrivilegedBroker()
{
    if (const std::shared_ptr<Platform::IPrivilegedBroker>& replaced = brokerForTesting(); replaced != nullptr)
    {
        return replaced;
    }
    static const std::shared_ptr<Platform::IPrivilegedBroker> broker = Platform::makePrivilegedBroker(mainNativeWindow());
    return broker;
}

/// @p scenario's process actions, or the platform's, which an administrator can finish (#2308).
[[nodiscard]] std::unique_ptr<Platform::IProcessActions> processActionsFor(const Synthetic::Scenario* scenario)
{
    std::unique_ptr<Platform::IProcessActions> actions = Synthetic::makeProcessActions(scenario);
    if (scenario != nullptr)
    {
        return actions;
    }
    return Platform::withPrivilegedBroker(std::move(actions), sharedPrivilegedBroker());
}

} // namespace

void preparePanelPlatforms(const Synthetic::Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return; // a synthetic run uses no helper
    }
    // Made now, on the UI thread: its owner window handle is an SDL query (mainNativeWindow()), and the
    // factories below, which share it, run on the panels' sampler threads from here on (#2731).
    (void) sharedPrivilegedBroker();
}

std::shared_ptr<TaskManagerReplacementController> makeTaskManagerReplacementController(const Synthetic::Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return std::make_shared<TaskManagerReplacementController>(nullptr); // a synthetic run changes nothing real
    }
    return std::make_shared<TaskManagerReplacementController>(Platform::makeTaskManagerReplacement(sharedPrivilegedBroker()));
}

ProcessesPanelPlatform makeProcessesPanelPlatform(const Synthetic::Scenario* scenario)
{
    // The probe reads every process itself: no helper read joins it since protocol v23 (#3580).
    ProcessesPanelPlatform platform{
        .probe = Synthetic::makeProcessProbe(scenario),
        .actions = processActionsFor(scenario),
        // Windows' ETW collector, or on Linux the eBPF counter TaskSmack loads when started as root (#3366, #3580).
        .udpCollector = Synthetic::makeUdpTrafficCollector(scenario),
        .icons = Synthetic::makeProcessIconSource(scenario),
    };
    // Find handles (#3067): the readers Process Details uses, another user's processes through the helper.
    // A search reads the open files in one batch (#3357): on Windows, one pass over the system handle table.
    platform.openFiles = (scenario == nullptr)
                           ? Platform::withPrivilegedBroker(Synthetic::makeProcessOpenFilesReader(scenario), sharedPrivilegedBroker())
                           : Synthetic::makeProcessOpenFilesReader(scenario);
    platform.modules = (scenario == nullptr)
                         ? Platform::withPrivilegedBroker(Synthetic::makeProcessModulesReader(scenario), sharedPrivilegedBroker())
                         : Synthetic::makeProcessModulesReader(scenario);
    return platform;
}

SystemModelProbes makeSystemModelProbes(const Synthetic::Scenario* scenario)
{
    return {.system = Synthetic::makeSystemProbe(scenario), .power = Synthetic::makePowerProbe(scenario)};
}

std::unique_ptr<Platform::IDiskProbe> makeStorageModelProbe(const Synthetic::Scenario* scenario)
{
    return Synthetic::makeDiskProbe(scenario);
}

std::unique_ptr<Platform::IGPUProbe> makeGPUModelProbe(const Synthetic::Scenario* scenario)
{
    return Synthetic::makeGPUProbe(scenario);
}

ProcessDetailsReaders makeProcessDetailsReaders(const Synthetic::Scenario* scenario)
{
    return {
        .actions = processActionsFor(scenario),
        .environment = (scenario == nullptr)
                         ? Platform::withPrivilegedBroker(Synthetic::makeProcessEnvironmentReader(scenario), sharedPrivilegedBroker())
                         : Synthetic::makeProcessEnvironmentReader(scenario),
        .connections = (scenario == nullptr)
                         ? Platform::withPrivilegedBroker(Synthetic::makeProcessConnectionsReader(scenario), sharedPrivilegedBroker())
                         : Synthetic::makeProcessConnectionsReader(scenario),
        .modules = (scenario == nullptr)
                     ? Platform::withPrivilegedBroker(Synthetic::makeProcessModulesReader(scenario), sharedPrivilegedBroker())
                     : Synthetic::makeProcessModulesReader(scenario),
        .security = Synthetic::makeProcessSecurityReader(scenario),
        .openFiles = (scenario == nullptr)
                       ? Platform::withPrivilegedBroker(Synthetic::makeProcessOpenFilesReader(scenario), sharedPrivilegedBroker())
                       : Synthetic::makeProcessOpenFilesReader(scenario),
        .threads = (scenario == nullptr)
                     ? Platform::withPrivilegedBroker(Synthetic::makeProcessThreadsReader(scenario), sharedPrivilegedBroker())
                     : Synthetic::makeProcessThreadsReader(scenario),
    };
}

std::unique_ptr<Platform::IProcessMemoryMapReader> makeProcessMemoryMapReader(const Synthetic::Scenario* scenario)
{
    return Synthetic::makeProcessMemoryMapReader(scenario); // no helper reads memory maps (#3580)
}

std::unique_ptr<Platform::IProcessActions> makeConnectionsPanelActions(const Synthetic::Scenario* scenario)
{
    return processActionsFor(scenario);
}

std::shared_ptr<Platform::IUserSessions> makeUsersPanelSessions(const Synthetic::Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return std::make_shared<Platform::UnsupportedUserSessions>();
    }
#ifdef _WIN32
    // Another user's session needs an administrator (#3076): the helper does it, as polkit does on Linux.
    return std::make_shared<Platform::BrokeredUserSessions>(Platform::makeUserSessions(), sharedPrivilegedBroker());
#else
    return Platform::makeUserSessions();
#endif
}

ServicesPanelPlatform makeServicesPanelPlatform(const Synthetic::Scenario* /*scenario*/)
{
    // No helper does service actions: Windows' doesn't, and Linux has none (#3580).
    return {.probe = Platform::makeServiceProbe(), .actions = Platform::makeServiceActions()};
}

StartupPanelPlatform makeStartupPanelPlatform(const Synthetic::Scenario* /*scenario*/)
{
    return {.probe = Platform::makeStartupProbe(), .actions = Platform::makeStartupActions()};
}

std::unique_ptr<Platform::ISystemInfoProbe> makeSystemInfoPanelProbe(const Synthetic::Scenario* /*scenario*/)
{
    // No helper reads firmware files since #3580: a Linux run as root reads them itself.
    return Platform::makeSystemInfoProbe();
}

std::shared_ptr<Platform::IHttpsClient> makeUpdateCheckClient(const Synthetic::Scenario* scenario, bool testHookActive)
{
    // No request from a made-up machine or an unattended test run (#3093): the check isn't offered there.
    if (scenario != nullptr || testHookActive)
    {
        return nullptr;
    }
    return Platform::makeHttpsClient();
}

std::shared_ptr<Platform::IStartAtLogin> makeStartAtLoginEntry(const Synthetic::Scenario* scenario, bool testHookActive)
{
    // Never from a made-up machine or an unattended test run (#3093 part 2): the row isn't offered there.
    if (scenario != nullptr || testHookActive)
    {
        return nullptr;
    }
    return Platform::makeStartAtLogin();
}

std::unique_ptr<Platform::ISystemInfoProbe> makeSensorProbe(const Synthetic::Scenario* /*scenario*/)
{
    // Unbrokered: reading sensors needs no privilege, and this runs on the sensor sampler's thread (#2737),
    // where the shared broker (UI-thread state) mustn't be touched.
    return Platform::makeSystemInfoProbe();
}

std::shared_ptr<Platform::ISystemInfoProbe> makeCrashHistoryProbe(const Synthetic::Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return nullptr;
    }
    // Its own probe: the System tab's model keeps its probe to itself, under its read lock.
    return Platform::makeSystemInfoProbe();
}

std::shared_ptr<Platform::IPrivilegedBroker> privilegedBroker(const Synthetic::Scenario* scenario)
{
    return scenario != nullptr ? nullptr : sharedPrivilegedBroker();
}

void Detail::setSharedPrivilegedBrokerForTesting(std::shared_ptr<Platform::IPrivilegedBroker> broker)
{
    brokerForTesting() = std::move(broker);
}

} // namespace App
