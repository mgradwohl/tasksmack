#include "PanelPlatforms.h"

#include "NativeWindow.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/ServicesPanel.h"
#include "Panels/StartupPanel.h"
#include "Panels/SystemMetricsPanel.h"
#include "Platform/BrokeredProcessActions.h"
#include "Platform/BrokeredSystemInfoProbe.h"
#include "Platform/Factory.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/ISystemInfoProbe.h"
#include "SyntheticScenario.h"

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

ProcessesPanelPlatform makeProcessesPanelPlatform(const Synthetic::Scenario* scenario)
{
    // PSS and USS of another user's process through the helper, once it runs (#2309).
    return {.probe = (scenario == nullptr) ? Platform::withPrivilegedBroker(Synthetic::makeProcessProbe(scenario), sharedPrivilegedBroker())
                                           : Synthetic::makeProcessProbe(scenario),
            .actions = processActionsFor(scenario),
            .udpCollector = Synthetic::makeUdpTrafficCollector(scenario),
            .icons = Synthetic::makeProcessIconSource(scenario)};
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
    return {.actions = processActionsFor(scenario),
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
                           : Synthetic::makeProcessOpenFilesReader(scenario)};
}

ServicesPanelPlatform makeServicesPanelPlatform(const Synthetic::Scenario* /*scenario*/)
{
    return {.probe = Platform::makeServiceProbe(), .actions = Platform::makeServiceActions()};
}

StartupPanelPlatform makeStartupPanelPlatform(const Synthetic::Scenario* /*scenario*/)
{
    return {.probe = Platform::makeStartupProbe(), .actions = Platform::makeStartupActions()};
}

std::unique_ptr<Platform::ISystemInfoProbe> makeSystemInfoPanelProbe(const Synthetic::Scenario* /*scenario*/)
{
    // The facts readable by root only, through the helper (#2310)
    return Platform::withPrivilegedBroker(Platform::makeSystemInfoProbe(), sharedPrivilegedBroker());
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
    return crashHistoryProbe(Platform::makeSystemInfoProbe(), sharedPrivilegedBroker());
}

std::shared_ptr<Platform::IPrivilegedBroker> privilegedBroker(const Synthetic::Scenario* scenario)
{
    return scenario != nullptr ? nullptr : sharedPrivilegedBroker();
}

void Detail::setSharedPrivilegedBrokerForTesting(std::shared_ptr<Platform::IPrivilegedBroker> broker)
{
    brokerForTesting() = std::move(broker);
}

std::shared_ptr<Platform::ISystemInfoProbe> crashHistoryProbe(std::unique_ptr<Platform::ISystemInfoProbe> probe,
                                                              std::shared_ptr<Platform::IPrivilegedBroker> broker)
{
    // Every user's crashes once the helper runs, as System Information reads them: both publish to the
    // one crash history, so an unbrokered read here would take the other users' crashes away again
    // (#2416). readCrashes() uses only a helper already running; it never asks for the password.
    return Platform::withPrivilegedBroker(std::move(probe), std::move(broker));
}

} // namespace App
