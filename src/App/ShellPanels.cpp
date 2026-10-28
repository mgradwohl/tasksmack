#include "ShellPanels.h"

#include "App/PanelPlatforms.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/CrashHistory.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/ISystemInfoProbe.h"
#include "ProcessColumnConfig.h"
#include "SelectOverride.h"
#include "SyntheticScenario.h"
#include "UserConfig.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace App
{

ShellPanels::ShellPanels()
{
    m_Services.setIconView(&m_Processes.iconView());
    m_Startup.setIconView(&m_Processes.iconView());
}

void ShellPanels::prepareAttach()
{
    // What the panel platforms share and must be made on this thread, before the samplers' threads build
    // the rest of them (#2731).
    preparePanelPlatforms(Synthetic::activeScenario());
    std::shared_ptr<Platform::ISystemInfoProbe> probe = makeCrashHistoryProbe(Synthetic::activeScenario());
    if (probe == nullptr)
    {
        return; // a synthetic run (#2137: PanelPlatforms decides)
    }
    // MSVC STL false positive, as in ProcessModel::computeSnapshotsLocked(): the analyzer loses track
    // of the probe's shared_ptr control block (_Rep) once it is moved into the lambda.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    auto crashHistory = std::make_shared<Domain::CrashHistory>(
        [probe = std::move(probe)] { return probe->capabilities().hasOs ? probe->readCrashes() : Platform::CrashesInfo{}; });
    m_SystemInfo.setCrashHistory(crashHistory);
    m_ProcessDetails.setCrashHistory(std::move(crashHistory));
}

void ShellPanels::connectModels()
{
    if (const auto processModel = m_Processes.processModel(); processModel != nullptr)
    {
        m_SystemMetrics.setProcessModel(processModel);

        // Share GPU model with ProcessModel for per-process GPU data
        if (const auto gpuModel = m_SystemMetrics.gpuModel(); gpuModel != nullptr)
        {
            processModel->setGPUModel(gpuModel);
            // Its probe also decides whether the GPU columns can be filled at all (#1210).
            m_Processes.setGpuModel(gpuModel);
        }

        // The Services tab's list says which services each process hosts on Windows (#1880): the Services
        // column, and the display names and states in Process Details' Services section. Sampled by the
        // Services panel alone, while one of them or the tab needs it (update()). Not in synthetic runs:
        // their processes are made up, and a real service's PID would match one by accident.
        if (Synthetic::activeScenario() == nullptr)
        {
            processModel->setServiceModel(m_Services.serviceModel());
            m_ProcessDetails.setServiceModel(m_Services.serviceModel());
        }
    }

    // The details pane draws only the series the process probe can fill (#1028, #1035). Refreshed
    // every update too, since a probe can withdraw a capability after the first sample (#1254).
    m_ProcessDetails.setProcessCapabilities(m_Processes.processCapabilities());
}

void ShellPanels::update(float deltaTime)
{
    // TASKSMACK_DETAILS_TAB (#1559): handed over only once the startup selection fired, so a selection
    // that gave up leaves no stale tab request behind.
    if (const std::optional<SelectOverride::DetailsTab> startupTab = m_Processes.takeStartupDetailsTab(); startupTab.has_value())
    {
        m_ProcessDetails.requestTab(*startupTab);
    }

    // The details pane follows the capabilities published with the latest generation (#1254): a
    // plain copy of what ProcessesPanel fetched with its snapshots, so no lock is taken here.
    m_ProcessDetails.setProcessCapabilities(m_Processes.processCapabilities());
    // Its NPU tab shows while the machine lists an NPU or compute accelerator, as the machine view's does (#1869).
    m_ProcessDetails.setHasAccelerator(m_Processes.hasAccelerator());
    // With several processes selected, the Details tab names the shown one's PID (#1926).
    m_ProcessDetails.setSelectionCount(m_Processes.selectionCount());
    // Per-process GPU support reaches the details pane with each sample, as of the generation it
    // came from (Domain::ProcessSample, #1210), not from here.

    // Hand Process Details the selected process's new samples: one per generation the sampler
    // published since its last frame, each with its own sample time (#1098). The model keeps them for
    // the watched PID, so a frame with nothing new costs one atomic load and copies nothing -- this
    // used to look the process up and deep-copy its snapshot twice every frame (#1172).
    const std::int32_t selectedPid = m_ProcessDetails.selectedPid();
    if (selectedPid != m_WatchedPid)
    {
        m_Processes.watchProcess(selectedPid);
        m_WatchedPid = selectedPid;
    }
    m_PendingSamples.clear();
    if (selectedPid != -1)
    {
        static_cast<void>(m_Processes.watchedSamplesSince(m_ProcessDetails.lastSampleVersion(), m_PendingSamples));
    }
    m_ProcessDetails.updateWithSamples(m_PendingSamples, deltaTime);

    // The service list is read while the Services tab is shown, and also while the Processes table
    // shows its Services column or Process Details' Services section is open (#1880) -- at the tab's
    // cadence, on its sampler, and not at all once none of them needs it. Only a change does anything.
    m_Services.setBackgroundDemand(m_Processes.columnSettings().isVisible(ProcessColumn::Services) ||
                                   m_ProcessDetails.servicesSectionOpen());

    // Debug: Log when GPU data becomes available for the selected PID.
    // This avoids spamming logs every frame while a GPU-using process is selected.
    if (const Domain::ProcessSnapshot* selected = m_ProcessDetails.displayedSnapshot(); selected != nullptr && !m_PendingSamples.empty())
    {
        const bool hasGpuData = (!selected->gpuDevices.empty() || (selected->gpuMemoryBytes > 0));
        if ((selectedPid != m_LastGpuLogPid) || !m_LastGpuLogHasData)
        {
            if (hasGpuData)
            {
                spdlog::debug("ShellLayer: Selected PID {} has GPU data: devices='{}', mem={}",
                              selectedPid,
                              selected->gpuDevices,
                              selected->gpuMemoryBytes);
            }

            m_LastGpuLogPid = selectedPid;
            m_LastGpuLogHasData = hasGpuData;
        }
    }
    // Drop this frame's references now: the panel keeps the one it shows.
    m_PendingSamples.clear();
}

void ShellPanels::raiseStartupSettings(const std::function<void(Core::Event&)>& raise)
{
    const auto& settings = UserConfig::get().settings();
    // The synthetic scenario (#1413) may start at its own window and interval, for this run only.
    const Synthetic::Scenario* scenario = Synthetic::activeScenario();
    Core::RefreshRateChangedEvent refreshEvent(Synthetic::startupRefreshIntervalMs(scenario, settings.refreshIntervalMs), /*initial=*/true);
    raise(refreshEvent);
    Core::HistoryDurationChangedEvent historyEvent(Synthetic::startupHistorySeconds(scenario, settings.maxHistorySeconds),
                                                   /*initial=*/true);
    raise(historyEvent);
}

} // namespace App
