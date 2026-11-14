#include "ShellPanels.h"

#include "App/PanelPlatforms.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/WindowConstants.h"
#include "Domain/CrashHistory.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Panels/ConnectionsPanel.h"
#include "Panels/PriorityChangesInFlight.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessGrouping.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/ServicesPanel.h"
#include "Panels/StartupPanel.h"
#include "Panels/SystemInfoPanel.h"
#include "Panels/SystemMetricsPanel.h"
#include "Panels/UserSessionsFeed.h"
#include "Panels/UsersPanel.h"
#include "Platform/ISystemInfoProbe.h"
#include "ProcessColumnConfig.h"
#include "SelectOverride.h"
#include "SyntheticScenario.h"
#include "UserConfig.h"

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace App
{

// Everything ShellPanels owns, in the order it was declared before #3717 moved it here: the members are destroyed in
// reverse, so the sessions' feed and the priority changes still outlive the panels that point at them.
struct ShellPanels::Impl
{
    // The signed-in sessions, before every panel that shows them, so it outlives them (#3377): the Users tab
    // and the Processes table's Group: User. The Users tab starts and stops it, at its attach and detach.
    UserSessionsFeed m_UserSessions;
    // The processes a priority change is asked about or running for, in the Processes table or Process
    // Details (#3467): each panel holds its own priority controls for a process the other has one for.
    // Before both panels, so it outlives them.
    PriorityChangesInFlight m_PriorityChanges;
    // The Processes panel first: its icon view outlives the Services and Startup tabs that draw with it.
    ProcessesPanel m_Processes;
    ProcessDetailsPanel m_ProcessDetails;
    SystemMetricsPanel m_SystemMetrics;
    ServicesPanel m_Services;
    StartupPanel m_Startup;
    ConnectionsPanel m_Connections;     // #3066
    UsersPanel m_Users{m_UserSessions}; // #3076
    SystemInfoPanel m_SystemInfo;

    // The PID the process model watches for Process Details, and the scratch buffer its new samples
    // are collected into each update, reused to avoid allocating.
    std::int32_t m_WatchedPid = -1;
    std::vector<Domain::ProcessSample> m_PendingSamples;

    // GPU debug logging throttling.
    std::int32_t m_LastGpuLogPid = -1;
    bool m_LastGpuLogHasData = false;
};

ShellPanels::ShellPanels() : m_Impl(std::make_unique<Impl>())
{
    m_Impl->m_Services.setIconView(&m_Impl->m_Processes.iconView());
    m_Impl->m_Startup.setIconView(&m_Impl->m_Processes.iconView());
    // One priority change at a time reaches a process, from whichever panel (#3467).
    m_Impl->m_Processes.setPriorityChanges(&m_Impl->m_PriorityChanges);
    m_Impl->m_ProcessDetails.setPriorityChanges(&m_Impl->m_PriorityChanges);
    // Group: User draws each account's sessions under its header, with their actions (#3377).
    m_Impl->m_Processes.setUserSessions(&m_Impl->m_UserSessions);
}

ShellPanels::~ShellPanels() = default;

ProcessesPanel& ShellPanels::processes() noexcept
{
    return m_Impl->m_Processes;
}

ProcessDetailsPanel& ShellPanels::processDetails() noexcept
{
    return m_Impl->m_ProcessDetails;
}

SystemMetricsPanel& ShellPanels::systemMetrics() noexcept
{
    return m_Impl->m_SystemMetrics;
}

ServicesPanel& ShellPanels::services() noexcept
{
    return m_Impl->m_Services;
}

StartupPanel& ShellPanels::startup() noexcept
{
    return m_Impl->m_Startup;
}

ConnectionsPanel& ShellPanels::connections() noexcept
{
    return m_Impl->m_Connections;
}

UsersPanel& ShellPanels::users() noexcept
{
    return m_Impl->m_Users;
}

SystemInfoPanel& ShellPanels::systemInfo() noexcept
{
    return m_Impl->m_SystemInfo;
}

std::array<Panel*, 8> ShellPanels::inTabOrder() noexcept
{
    Impl& p = *m_Impl;
    return {
        &p.m_SystemMetrics,
        &p.m_Processes,
        &p.m_ProcessDetails,
        &p.m_Services,
        &p.m_Startup,
        &p.m_Connections,
        &p.m_Users,
        &p.m_SystemInfo,
    };
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
    m_Impl->m_SystemInfo.setCrashHistory(crashHistory);
    m_Impl->m_ProcessDetails.setCrashHistory(std::move(crashHistory));
}

void ShellPanels::connectModels()
{
    if (const auto processModel = m_Impl->m_Processes.processModel(); processModel != nullptr)
    {
        m_Impl->m_SystemMetrics.setProcessModel(processModel);
        m_Impl->m_Users.setProcessModel(processModel); // the Users tab's per-user totals (#3076)

        // Share GPU model with ProcessModel for per-process GPU data
        if (const auto gpuModel = m_Impl->m_SystemMetrics.gpuModel(); gpuModel != nullptr)
        {
            processModel->setGPUModel(gpuModel);
            // Its probe also decides whether the GPU columns can be filled at all (#1210).
            m_Impl->m_Processes.setGpuModel(gpuModel);
        }

        // The Services tab's list says which services each process hosts on Windows (#1880): the Services
        // column, and the display names and states in Process Details' Services section. Sampled by the
        // Services panel alone, while one of them or the tab needs it (update()). Not in synthetic runs:
        // their processes are made up, and a real service's PID would match one by accident.
        if (Synthetic::activeScenario() == nullptr)
        {
            processModel->setServiceModel(m_Impl->m_Services.serviceModel());
            m_Impl->m_ProcessDetails.setServiceModel(m_Impl->m_Services.serviceModel());
        }
    }

    // The details pane draws only the series the process probe can fill (#1028, #1035). Refreshed
    // every update too, since a probe can withdraw a capability after the first sample (#1254).
    m_Impl->m_ProcessDetails.setProcessCapabilities(m_Impl->m_Processes.processCapabilities());
}

void ShellPanels::update(float deltaTime)
{
    // TASKSMACK_DETAILS_TAB (#1559): handed over only once the startup selection fired, so a selection
    // that gave up leaves no stale tab request behind.
    if (const std::optional<SelectOverride::DetailsTab> startupTab = m_Impl->m_Processes.takeStartupDetailsTab(); startupTab.has_value())
    {
        m_Impl->m_ProcessDetails.requestTab(*startupTab);
    }

    // The details pane follows the capabilities published with the latest generation (#1254): a
    // plain copy of what ProcessesPanel fetched with its snapshots, so no lock is taken here.
    m_Impl->m_ProcessDetails.setProcessCapabilities(m_Impl->m_Processes.processCapabilities());
    // Its NPU tab shows while the machine lists an NPU or compute accelerator, as the machine view's does (#1869).
    m_Impl->m_ProcessDetails.setHasAccelerator(m_Impl->m_Processes.hasAccelerator());
    // With several processes selected, the Details tab names the shown one's PID (#1926).
    m_Impl->m_ProcessDetails.setSelectionCount(m_Impl->m_Processes.selectionCount());
    // Per-process GPU support reaches the details pane with each sample, as of the generation it
    // came from (Domain::ProcessSample, #1210), not from here.

    // Hand Process Details the selected process's new samples: one per generation the sampler
    // published since its last frame, each with its own sample time (#1098). The model keeps them for
    // the watched PID, so a frame with nothing new costs one atomic load and copies nothing -- this
    // used to look the process up and deep-copy its snapshot twice every frame (#1172).
    const std::int32_t selectedPid = m_Impl->m_ProcessDetails.selectedPid();
    if (selectedPid != m_Impl->m_WatchedPid)
    {
        m_Impl->m_Processes.watchProcess(selectedPid);
        m_Impl->m_WatchedPid = selectedPid;
    }
    m_Impl->m_PendingSamples.clear();
    if (selectedPid != -1)
    {
        static_cast<void>(m_Impl->m_Processes.watchedSamplesSince(m_Impl->m_ProcessDetails.lastSampleVersion(), m_Impl->m_PendingSamples));
    }
    m_Impl->m_ProcessDetails.updateWithSamples(m_Impl->m_PendingSamples, deltaTime);
    // Its App row names the App group the table puts it in (#3457), worked out from the whole generation.
    m_Impl->m_ProcessDetails.setAppMembership(
        m_Impl->m_Processes.appMembership(selectedPid, m_Impl->m_ProcessDetails.selectedStartTicks()));

    // The service list is read while the Services tab is shown, and also while the Processes table
    // shows its Services column or Process Details' Services section is open (#1880), or, on Windows,
    // while the table is grouped by Unit, whose units come from the list (#3386) -- at the tab's
    // cadence, on its sampler, and not at all once none of them needs it. Only a change does anything.
    m_Impl->m_Services.setBackgroundDemand(
        ProcessGrouping::servicesBackgroundDemand(m_Impl->m_Processes.columnSettings().isVisible(ProcessColumn::Services),
                                                  m_Impl->m_ProcessDetails.servicesSectionOpen(),
                                                  m_Impl->m_Processes.groupBy(),
                                                  ProcessGrouping::UNITS_FROM_SERVICE_LIST));

    // Debug: Log when GPU data becomes available for the selected PID.
    // This avoids spamming logs every frame while a GPU-using process is selected.
    if (const Domain::ProcessSnapshot* selected = m_Impl->m_ProcessDetails.displayedSnapshot();
        selected != nullptr && !m_Impl->m_PendingSamples.empty())
    {
        const bool hasGpuData = (!selected->gpuDevices.empty() || (selected->gpuMemoryBytes > 0));
        if ((selectedPid != m_Impl->m_LastGpuLogPid) || !m_Impl->m_LastGpuLogHasData)
        {
            if (hasGpuData)
            {
                spdlog::debug("ShellLayer: Selected PID {} has GPU data: devices='{}', mem={}",
                              selectedPid,
                              selected->gpuDevices,
                              selected->gpuMemoryBytes);
            }

            m_Impl->m_LastGpuLogPid = selectedPid;
            m_Impl->m_LastGpuLogHasData = hasGpuData;
        }
    }
    // Drop this frame's references now: the panel keeps the one it shows.
    m_Impl->m_PendingSamples.clear();
}

// A hidden window's loop wakes at least every WINDOW_HIDDEN_WAIT_TIMEOUT, and must take the watched
// process's samples before the model's ring of them, one per refresh, wraps at the fastest interval.
static_assert(Core::WINDOW_HIDDEN_WAIT_TIMEOUT < std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS) *
                                                     static_cast<std::int64_t>(Domain::ProcessModel::WATCHED_SAMPLE_CAPACITY));

void ShellPanels::updateWhileHidden()
{
    // The PID watched since the last frame's update(): a selection only changes in a frame.
    const std::int32_t selectedPid = m_Impl->m_ProcessDetails.selectedPid();
    m_Impl->m_PendingSamples.clear();
    if (selectedPid != -1 && selectedPid == m_Impl->m_WatchedPid &&
        m_Impl->m_Processes.watchedSamplesSince(m_Impl->m_ProcessDetails.lastSampleVersion(), m_Impl->m_PendingSamples))
    {
        m_Impl->m_ProcessDetails.takeSamples(m_Impl->m_PendingSamples);
    }
    m_Impl->m_PendingSamples.clear();
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
