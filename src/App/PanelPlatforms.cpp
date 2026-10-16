#include "PanelPlatforms.h"

#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/ServicesPanel.h"
#include "Panels/StartupPanel.h"
#include "Panels/SystemMetricsPanel.h"
#include "Platform/Factory.h"
#include "Platform/ISystemInfoProbe.h"
#include "SyntheticScenario.h"

#include <memory>

namespace App
{

ProcessesPanelPlatform makeProcessesPanelPlatform(const Synthetic::Scenario* scenario)
{
    return {.probe = Synthetic::makeProcessProbe(scenario),
            .actions = Synthetic::makeProcessActions(scenario),
            .udpCollector = Synthetic::makeUdpTrafficCollector(scenario),
            .icons = Synthetic::makeProcessIconSource(scenario)};
}

SystemMetricsProbes makeSystemMetricsProbes(const Synthetic::Scenario* scenario)
{
    return {.system = Synthetic::makeSystemProbe(scenario),
            .power = Synthetic::makePowerProbe(scenario),
            .disk = Synthetic::makeDiskProbe(scenario),
            .gpu = Synthetic::makeGPUProbe(scenario)};
}

ProcessDetailsReaders makeProcessDetailsReaders(const Synthetic::Scenario* scenario)
{
    return {.actions = Synthetic::makeProcessActions(scenario),
            .environment = Synthetic::makeProcessEnvironmentReader(scenario),
            .connections = Synthetic::makeProcessConnectionsReader(scenario),
            .modules = Synthetic::makeProcessModulesReader(scenario),
            .security = Synthetic::makeProcessSecurityReader(scenario),
            .openFiles = Synthetic::makeProcessOpenFilesReader(scenario)};
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

} // namespace App
