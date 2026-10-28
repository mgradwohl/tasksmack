#include "ProcessDetailsPanel.h"

#include "App/Panel.h"
#include "App/PanelPlatforms.h"
#include "App/Panels/ProcessArchitectureNote.h"
#include "App/Panels/ProcessStateColor.h"
#include "App/Panels/ProcessTypeColor.h"
#include "App/SelectOverride.h"
#include "App/ShellMetrics.h"
#include "App/SyntheticScenario.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessServices.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/ServiceModel.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IServiceProbe.h"
#include "ProcessActionsBlock.h"
#include "ProcessActionsView.h"
#include "ProcessConnectionsView.h"
#include "ProcessCrashHistoryView.h"
#include "ProcessDetailsCharts.h"
#include "ProcessDetailsHistory.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessEnvironmentView.h"
#include "ProcessModulesView.h"
#include "ProcessOpenFilesView.h"
#include "ProcessOverviewCard.h"
#include "ProcessPriorityView.h"
#include "ProcessRowSelection.h"
#include "ProcessSecurityView.h"
#include "ProcessSmoothedUsage.h"
#include "ServicesView.h"
#include "UI/ChartWidgets.h"
#include "UI/EmptyState.h"
#include "UI/FillPlotLayout.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/TabContent.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

// Constructor (inside App namespace)
ProcessDetailsPanel::ProcessDetailsPanel() : ProcessDetailsPanel(makeProcessDetailsReaders(Synthetic::activeScenario()))
{}

ProcessDetailsPanel::ProcessDetailsPanel(ProcessDetailsReaders readers)
    : ProcessDetailsPanel(std::move(readers.actions),
                          std::move(readers.environment),
                          std::move(readers.connections),
                          std::move(readers.modules),
                          std::move(readers.security),
                          std::move(readers.openFiles))
{
    setThreadsReader(std::move(readers.threads)); // the one reader not passed above: untouched by those moves
}

ProcessDetailsPanel::ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions)
    : ProcessDetailsPanel(std::move(processActions), nullptr, nullptr)
{}

ProcessDetailsPanel::ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions,
                                         std::unique_ptr<Platform::IProcessEnvironmentReader> environmentReader)
    : ProcessDetailsPanel(std::move(processActions), std::move(environmentReader), nullptr)
{}

ProcessDetailsPanel::ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions,
                                         std::unique_ptr<Platform::IProcessEnvironmentReader> environmentReader,
                                         std::unique_ptr<Platform::IProcessConnectionsReader> connectionsReader)
    : ProcessDetailsPanel(std::move(processActions), std::move(environmentReader), std::move(connectionsReader), nullptr)
{}

ProcessDetailsPanel::ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions,
                                         std::unique_ptr<Platform::IProcessEnvironmentReader> environmentReader,
                                         std::unique_ptr<Platform::IProcessConnectionsReader> connectionsReader,
                                         std::unique_ptr<Platform::IProcessModulesReader> modulesReader)
    : ProcessDetailsPanel(
          std::move(processActions), std::move(environmentReader), std::move(connectionsReader), std::move(modulesReader), nullptr)
{}

ProcessDetailsPanel::ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions,
                                         std::unique_ptr<Platform::IProcessEnvironmentReader> environmentReader,
                                         std::unique_ptr<Platform::IProcessConnectionsReader> connectionsReader,
                                         std::unique_ptr<Platform::IProcessModulesReader> modulesReader,
                                         std::unique_ptr<Platform::IProcessSecurityReader> securityReader,
                                         std::unique_ptr<Platform::IProcessOpenFilesReader> openFilesReader)
    : Panel("Process Details"),
      m_ProcessActions(std::move(processActions)),
      m_ActionCapabilities(m_ProcessActions ? m_ProcessActions->actionCapabilities() : Platform::ProcessActionCapabilities{}),
      m_EnvironmentReader(std::move(environmentReader)),
      m_HasEnvironment(m_EnvironmentReader != nullptr && m_EnvironmentReader->hasEnvironment()),
      m_ConnectionsReader(std::move(connectionsReader)),
      m_HasConnections(m_ConnectionsReader != nullptr && m_ConnectionsReader->hasConnections()),
      m_ModulesReader(std::move(modulesReader)),
      m_HasModules(m_ModulesReader != nullptr && m_ModulesReader->hasModules()),
      m_SecurityReader(std::move(securityReader)),
      m_HasSecurity(m_SecurityReader != nullptr && m_SecurityReader->hasSecurity()),
      m_OpenFilesReader(std::move(openFilesReader)),
      m_HasOpenFiles(m_OpenFilesReader != nullptr && m_OpenFilesReader->hasOpenFiles())
{}

void ProcessDetailsPanel::updateWithSamples(std::span<const Domain::ProcessSample> samples, float deltaTime)
{
    m_LastDeltaSeconds = deltaTime;

    // Fade out action result message
    m_ActionsView.tick(deltaTime);
    // The Set affinity dialog's CPU layout (#3182): read once on its worker, started here, before any
    // selection, so the dialog normally finds it read. Never read on the UI thread.
    m_ActionsView.updateAffinityLayout(m_ActionCapabilities.canSetAffinity ? m_ProcessActions.get() : nullptr);

    // The Services section wants the service list kept current only while it was drawn open on the last
    // frame (#1880): not while collapsed, on another Details tab, or with this tab hidden.
    m_ServicesSectionOpen = std::exchange(m_ServicesDrawnOpen, false);

    if (m_SelectedPid == -1)
    {
        m_HasSnapshot = false;
        return;
    }

    // One history point per generation the sampler published, stamped with when it was sampled --
    // not one per frame that noticed a new generation, stamped with the frame's time, which lost
    // generations published between two frames and placed the rest up to a frame late (#1098).
    // A sample only counts if it is of the selected process itself, not of a different process that
    // has since been given its PID (Detail::takeSamples()).
    const std::uint64_t versionBefore = m_SampleIntake.lastVersion;
    bool recorded = false;
    Detail::takeSamples(samples,
                        m_SelectedPid,
                        m_SelectedStartTicks,
                        m_SampleIntake,
                        [this, &recorded](const Domain::ProcessSample& sample, bool gapBefore)
                        {
                            m_CachedSnapshot = sample.snapshot; // shared, not copied (#1172)
                            // Each sample's rates judged by its own generation's probe support (#1210)
                            m_CachedRateReadings = Detail::rateReadings(sample);
                            recordHistoryPoint(*sample.snapshot, sample.sampleTimeSeconds, gapBefore, m_CachedRateReadings);
                            recorded = true;
                        });
    if (recorded)
    {
        m_History.trimToWindow(m_MaxHistorySeconds);
        m_HistoryGeneration = UI::Widgets::nextChartDataGeneration();
    }

    if (m_SampleIntake.lastVersion != versionBefore)
    {
        // The newest generation decides whether the process is still there.
        if (m_SampleIntake.present)
        {
            m_HasSnapshot = true;
            m_ProcessExited = false;
        }
        // A sample recorded earlier in this same batch counts as having seen the process: a batch
        // that accepted it and then ends with it absent means it exited, not "not seen yet".
        else if (Detail::exitedAfterBatch(false, m_HasSnapshot, recorded))
        {
            // The cached snapshot is kept (the tab still names the process) but no longer drawn as
            // if it were live; renderContent() shows the exited state instead.
            m_ProcessExited = true;
        }
    }

    if (m_HasSnapshot && !m_ProcessExited)
    {
        m_SmoothedUsage.update(*m_CachedSnapshot, m_CachedRateReadings, deltaTime, m_RefreshInterval);
    }

    // The Environment section's on-demand read (#179): here in the update path, never in render(), and
    // only when the section was drawn open last frame and a read is due (ProcessEnvironmentView::update()),
    // for the selected process alone, identified by PID and start time so a reused PID is not read.
    const bool canReadEnvironment = m_HasEnvironment && m_HasSnapshot && !m_ProcessExited;
    static_cast<void>(m_EnvironmentView.update(canReadEnvironment ? m_EnvironmentReader.get() : nullptr, selectedTarget(), deltaTime));

    // The Connections section's on-demand read (#799), on the same terms.
    const bool canReadConnections = m_HasConnections && m_HasSnapshot && !m_ProcessExited;
    static_cast<void>(m_ConnectionsView.update(canReadConnections ? m_ConnectionsReader.get() : nullptr, selectedTarget(), deltaTime));

    // The Modules section's on-demand read (#802), on the same terms.
    const bool canReadModules = m_HasModules && m_HasSnapshot && !m_ProcessExited;
    static_cast<void>(m_ModulesView.update(canReadModules ? m_ModulesReader.get() : nullptr, selectedTarget(), deltaTime));

    // The Threads section's on-demand read (#3069), on the same terms, at the Settings interval: its CPU %
    // is a live figure.
    const bool canReadThreads = m_HasThreads && m_HasSnapshot && !m_ProcessExited;
    static_cast<void>(m_ThreadsView.update(canReadThreads ? m_ThreadsReader.get() : nullptr,
                                           selectedTarget(),
                                           deltaTime,
                                           static_cast<int>(m_RefreshInterval.count()),
                                           std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()));

    // The Security section's on-demand read (#1526), on the same terms.
    const bool canReadSecurity = m_HasSecurity && m_HasSnapshot && !m_ProcessExited;
    static_cast<void>(m_SecurityView.update(canReadSecurity ? m_SecurityReader.get() : nullptr, selectedTarget(), deltaTime));
    // The Open files section's on-demand read (#183), on the same terms.
    const bool canReadOpenFiles = m_HasOpenFiles && m_HasSnapshot && !m_ProcessExited;
    static_cast<void>(m_OpenFilesView.update(canReadOpenFiles ? m_OpenFilesReader.get() : nullptr, selectedTarget(), deltaTime));

    // The Priority card's I/O priority read (#2734): on its worker, started here and never from render(),
    // and only when the control was drawn last frame (ProcessIoPriorityView::updateRead()).
    static_cast<void>(
        m_PriorityView.updateReads(m_ActionCapabilities.canSetIoPriority ? m_ProcessActions.get() : nullptr, selectedTarget(), deltaTime));

    // The Recent crashes line's shared cache (#1675): read on a worker only while the line was drawn and
    // the cache is stale, whatever the selection; matching it to the process is done per read, not here.
    static_cast<void>(m_CrashHistoryView.update(deltaTime));
}

void ProcessDetailsPanel::recordHistoryPoint(const Domain::ProcessSnapshot& snapshot,
                                             double sampleTimeSeconds,
                                             bool gapBefore,
                                             Detail::SampleRateReadings rateReadings)
{
    // One value per series at each point, a gap where the probe had no reading (Detail::historyPointFrom()).
    m_History.append(sampleTimeSeconds, Detail::historyPointFrom(snapshot, rateReadings), gapBefore);

    // Peak working set in bytes, like the Used line it caps (never decreases)
    m_PeakMemoryBytes = std::max(m_PeakMemoryBytes, Domain::Numeric::toDouble(snapshot.peakMemoryBytes));
}

const Domain::ProcessSnapshot& ProcessDetailsPanel::cachedSnapshot() const
{
    static const Domain::ProcessSnapshot empty{};
    return m_CachedSnapshot ? *m_CachedSnapshot : empty;
}

ProcessChartContext ProcessDetailsPanel::chartContext() const
{
    return ProcessChartContext{
        .history = &m_History,
        .smoothed = &m_SmoothedUsage,
        .snapshot = &cachedSnapshot(),
        .historyGeneration = m_HistoryGeneration,
        .maxHistorySeconds = m_MaxHistorySeconds,
        .peakMemoryBytes = m_PeakMemoryBytes,
        .rateReadings = m_CachedRateReadings,
        .hasSharedMemory = m_ProcessCapabilities.hasSharedMemory,
        .hasPowerUsage = m_ProcessCapabilities.hasPowerUsage,
        .hasPageFaults = m_ProcessCapabilities.hasPageFaults,
        .hasPrivateBytes = m_ProcessCapabilities.hasPrivateBytes,
        .hasSwapBytes = m_ProcessCapabilities.hasSwapBytes,
    };
}

void ProcessDetailsPanel::renderServicesSection(const Domain::ProcessSnapshot& proc)
{
    if (proc.services.empty() || !ImGui::CollapsingHeader(ICON_FA_GEARS "  Services###ProcessServices"))
    {
        return;
    }
    // Open: ShellLayer keeps the service list current (#1880), whose display names and states are shown
    // beside the names when it lists them (Windows; on Linux until systemd's list is read, the names alone).
    m_ServicesDrawnOpen = true;
    std::shared_ptr<const Domain::ServicePublication> list;
    if (m_ServiceModel != nullptr)
    {
        list = m_ServiceModel->publication();
        if (!Domain::ProcessServices::isCurrent(*list, std::chrono::steady_clock::now()))
        {
            list.reset();
        }
    }
    const auto& scheme = UI::Theme::get().scheme();
    // One line per service ("a.service, b.service" on a platform that hosts several in one process).
    std::string_view rest = proc.services;
    while (!rest.empty())
    {
        const std::size_t comma = rest.find(", ");
        const std::string name(rest.substr(0, comma));
        rest = (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 2);
        ImGui::PushID(name.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name.c_str());
        if (const Platform::ServiceInfo* service = (list != nullptr) ? Domain::findService(*list, name) : nullptr; service != nullptr)
        {
            if (!service->displayName.empty())
            {
                ImGui::SameLine();
                ImGui::TextColored(scheme.textMuted, "%s", service->displayName.c_str());
            }
            ImGui::SameLine();
            const std::string_view state = serviceStateLabel(service->state);
            ImGui::PushStyleColor(ImGuiCol_Text, serviceStateColor(service->state, scheme));
            ImGui::TextUnformatted(state.data(), state.data() + state.size());
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_ARROW_RIGHT "  Show in Services"))
        {
            Core::ShowServiceEvent show(name);
            if (m_RaiseEvent)
            {
                m_RaiseEvent(show);
            }
            else
            {
                Core::Application::get().raiseEvent(show);
            }
        }
        ImGui::SetItemTooltip("Select %s in the Services tab", name.c_str());
        ImGui::PopID();
    }
}

const std::string& ProcessDetailsPanel::tabLabel() const
{
    if (m_HasSnapshot && (m_SelectedPid != -1) && !cachedSnapshot().name.empty())
    {
        if (m_SelectionCount < 2)
        {
            return cachedSnapshot().name;
        }
        // Several processes selected, often some of one name: the PID says which one this is (#1926).
        ProcessRowSelection::formatDetailsTabText(m_TabLabelWithPid, cachedSnapshot().name, m_SelectedPid, m_SelectionCount);
        return m_TabLabelWithPid;
    }
    // Use static string to avoid heap allocation every frame for the default label
    static const std::string defaultLabel{NO_SELECTION_TAB_TEXT};
    return defaultLabel;
}

void ProcessDetailsPanel::renderContent()
{
    // F9 (#170), taken now so it can only act on this frame: by the time renderPane() draws the tabs,
    // every reason not to (no selection, not on show, exited, not yet sampled) has returned early.
    const bool killRequested = m_KillShortcut.take();

    // What the actions can do, as of this frame: not all of it is fixed at construction. Once the
    // privileged helper connects (an "Authenticate and retry"), BrokeredProcessActions can set the
    // Realtime I/O class, so the I/O slider gains its Realtime stops (#2407). A struct built from what
    // the actions know already and one atomic read of the helper's connection as its broker last saw it:
    // no pipe or socket call here (IPrivilegedBroker::lastSeenConnected(), #3182).
    if (m_ProcessActions)
    {
        m_ActionCapabilities = m_ProcessActions->actionCapabilities();
    }

    renderPane(killRequested);

    // The Actions block's confirm dialog, submitted here every frame rather than from the block: the
    // block's child is skipped while the Overview is scrolled so that it is out of view, which left
    // an F9 Kill confirm pending with no dialog, refusing further F9 presses (#1493). Submitted after
    // the pane whatever it showed, the exited and empty states included: skipped there, a Kill confirm
    // stayed pending and refused F9, a modal could be left on ImGui's popup stack, which turns off the
    // shortcuts, and a dump's progress (its only Cancel) and result went with it (#2117). Once the pane no longer shows a running process,
    // the dialogs are given no live target, so a confirm or the affinity dialog closes unconfirmed, while a dump in flight keeps its
    // progress and then shows its result.
    const Platform::ProcessTarget noTarget{.pid = -1, .startTimeTicks = 0};
    m_ActionsView.renderConfirmation(m_ProcessActions.get(), showsRunningProcess() ? selectedTarget() : noTarget);
}

void ProcessDetailsPanel::renderPane(bool killRequested)
{
    if (m_SelectedPid == -1)
    {
        UI::Widgets::renderEmptyState(ICON_FA_CIRCLE_INFO "  No process selected",
                                      "Select a process in the Processes tab to see its details, history and actions here.");
        return;
    }

    // Skip rendering when tab is inactive (data collection continues in updateWithSamples)
    if (!m_IsActiveTab)
    {
        return;
    }

    if (m_ProcessExited)
    {
        // Replaces the whole pane, the Overview's Actions block included: nothing here may act on a
        // PID that no longer belongs to this process.
        const std::string detail = std::format(
            "{} (PID {}) is no longer running. Select another process in the Processes tab.", cachedSnapshot().name, m_SelectedPid);
        UI::Widgets::renderEmptyState(ICON_FA_TRIANGLE_EXCLAMATION "  Process exited", detail.c_str());
        return;
    }

    if (!m_HasSnapshot)
    {
        // Selected, but no snapshot of it has arrived: normally the frame or two before the first
        // one does, otherwise a PID that is not in the process list at all.
        const std::string detail = std::format("Process {} is not in the current process list.", m_SelectedPid);
        UI::Widgets::renderEmptyState(ICON_FA_TRIANGLE_EXCLAMATION "  Process not found", detail.c_str());
        return;
    }

    // F9: the Actions block's Kill confirm for the process shown, its target captured now; the
    // Overview, which holds the block (#1493), is brought forward below.
    if (killRequested && m_ActionsView.requestKillShortcut(m_ActionCapabilities, selectedTarget(), cachedSnapshot().name))
    {
        m_RequestedTab = SelectOverride::DetailsTab::Overview;
    }

    // Tabs for different info sections
    // Add padding inside tabs for better spacing, scaled like the style it overrides (#971)
    const float tabPaddingScale = UI::Theme::get().styleScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ShellMetrics::TAB_PADDING_X * tabPaddingScale, ShellMetrics::SUB_TAB_PADDING_Y * tabPaddingScale));

    if (ImGui::BeginTabBar("DetailsTabs", ImGuiTabBarFlags_DrawSelectedOverline))
    {
        // 1. Overview, with the Actions block beside Identity and Runtime (#1493). Brought forward by
        // F9, so its Kill confirm shows over the block it belongs to (#170).
        // Each tab's body scrolls in its own child, so the tab bar itself stays in view (#968).
        // The test hook's TASKSMACK_DETAILS_TAB may ask for another tab instead (#1559).
        const std::optional<SelectOverride::DetailsTab> requestedTab = std::exchange(m_RequestedTab, std::nullopt);
        const auto tabFlags = [&requestedTab](const SelectOverride::DetailsTab tab) -> ImGuiTabItemFlags
        {
            return requestedTab == tab ? ImGuiTabItemFlags_SetSelected : 0;
        };
        const ImGuiTabItemFlags overviewFlags = tabFlags(SelectOverride::DetailsTab::Overview);
        if (ImGui::BeginTabItem(ICON_FA_CIRCLE_INFO "  Overview", nullptr, overviewFlags))
        {
            {
                const UI::Widgets::TabContentScope content("##OverviewContent");
                // The charts on this tab share its height (#959). The Identity/Runtime/Actions row
                // above them is inside the scope, so it is counted as non-plot height.
                UI::Widgets::FillPlotLayout fill(m_OverviewFill);
                // Its charts share their plot edges, with or without a right-hand axis (#1206).
                const UI::Widgets::AlignedChartStack alignedCharts("##ProcOverviewCharts");
                renderBasicInfo(cachedSnapshot());
                // A section asked for (requestSection(), TASKSMACK_DEMO's section=, #2827) is drawn open
                // this frame through ImGui's own open state, as a click on its header would: SetNextItemOpen()
                // applies to the very next item, each view's CollapsingHeader(), so it is set only for a
                // section the view will draw. Taken here, so a request made while another tab shows waits.
                const std::optional<SelectOverride::DetailsSection> section = std::exchange(m_RequestedSection, std::nullopt);
                const auto openIfRequested = [section](const SelectOverride::DetailsSection which, const bool drawn)
                {
                    if (section == which && drawn)
                    {
                        ImGui::SetNextItemOpen(true);
                    }
                };
                // Collapsed by default, and hidden where the platform cannot read environments (#179)
                openIfRequested(SelectOverride::DetailsSection::Environment, m_HasEnvironment);
                m_EnvironmentView.render(m_HasEnvironment);
                // Likewise collapsed by default, and hidden where the platform cannot list sockets (#799)
                openIfRequested(SelectOverride::DetailsSection::Connections, m_HasConnections);
                m_ConnectionsView.render(m_HasConnections);
                // Likewise collapsed by default; hidden only in synthetic runs (#802)
                openIfRequested(SelectOverride::DetailsSection::Modules, m_HasModules);
                m_ModulesView.render(m_HasModules);
                // Likewise collapsed by default; hidden in synthetic runs and, for now, on Windows (#3069)
                m_ThreadsView.render(m_HasThreads);
                // Likewise collapsed by default; hidden only in synthetic runs (#1526)
                openIfRequested(SelectOverride::DetailsSection::Security, m_HasSecurity);
                m_SecurityView.render(m_HasSecurity);
                // Likewise collapsed by default; hidden only in synthetic runs (#183). Opened on request, its
                // scan starts too, as its Scan button does: the section lists nothing until a scan.
                openIfRequested(SelectOverride::DetailsSection::OpenFiles, m_HasOpenFiles);
                m_OpenFilesView.render(m_HasOpenFiles);
                if (section == SelectOverride::DetailsSection::OpenFiles && m_HasOpenFiles)
                {
                    m_OpenFilesView.requestScan();
                }
                // Likewise collapsed by default; only for a process that runs a service (#1886)
                openIfRequested(SelectOverride::DetailsSection::Services, !cachedSnapshot().services.empty());
                renderServicesSection(cachedSnapshot());
                ImGui::Separator();
                // Ensure smoothing is initialized even if render is called before an update tick
                if (!m_SmoothedUsage.initialized)
                {
                    m_SmoothedUsage.update(cachedSnapshot(), m_CachedRateReadings, m_LastDeltaSeconds, m_RefreshInterval);
                }
                // The CPU, Memory, Power and Resources charts are ProcessDetailsCharts' (#1179).
                m_Charts.renderOverviewCharts(chartContext(), fill);
            }
            ImGui::EndTabItem();
        }

        // 2. GPU (always show, with message if data unavailable)
        if (ImGui::BeginTabItem(ICON_FA_MICROCHIP "  GPU", nullptr, tabFlags(SelectOverride::DetailsTab::Gpu)))
        {
            {
                const UI::Widgets::TabContentScope content("##GpuContent");
                m_Charts.renderGpuTab(chartContext());
            }
            ImGui::EndTabItem();
        }

        // 3. NPU, beside the GPU tab, only on a machine with an NPU or compute accelerator, like the
        // machine view's NPU tab (#1869). The GPU tab is GPU-only.
        if (m_HasAccelerator && ImGui::BeginTabItem(ICON_FA_BRAIN "  NPU", nullptr, tabFlags(SelectOverride::DetailsTab::Npu)))
        {
            {
                const UI::Widgets::TabContentScope content("##NpuContent");
                m_Charts.renderNpuTab(chartContext());
            }
            ImGui::EndTabItem();
        }

        // 4. Network and I/O. Always present, like the GPU tab, so the tab set does not change while a
        // process stays selected; an empty state stands in until there is data (#1210).
        if (ImGui::BeginTabItem(ICON_FA_NETWORK_WIRED "  Network and I/O", nullptr, tabFlags(SelectOverride::DetailsTab::Network)))
        {
            {
                const UI::Widgets::TabContentScope content("##NetworkContent");
                m_Charts.renderNetworkTab(chartContext());
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::PopStyleVar(); // FramePadding
}

void ProcessDetailsPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);

    // Listen for active tab changes
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            m_IsActiveTab = (e.tabName() == "ProcessDetails");
            return false;
        });

    // Listen for process selection events
    dispatcher.dispatch<Core::ProcessSelectedEvent>(
        [this](Core::ProcessSelectedEvent& e)
        {
            setSelectedPid(e.getPid(), e.getStartTimeTicks());
            return false; // Don't consume - other panels might care
        });

    // Listen for refresh interval changes (NowBar smoothing)
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            m_RefreshInterval = std::chrono::milliseconds(e.getIntervalMs());
            return false;
        });

    // Listen for history duration changes
    dispatcher.dispatch<Core::HistoryDurationChangedEvent>(
        [this](Core::HistoryDurationChangedEvent& e)
        {
            // Clamped like the models' windows, and applied now: trimmed only by the next sample, the
            // charts kept the old window's data and scale until then -- indefinitely for a process
            // that is no longer sampled (#1145).
            m_MaxHistorySeconds = Domain::Sampling::clampHistorySeconds(Domain::Numeric::toDouble(e.getSeconds()));
            if (!m_History.empty())
            {
                m_History.trimToWindow(m_MaxHistorySeconds);
                m_HistoryGeneration = UI::Widgets::nextChartDataGeneration();
            }
            return false;
        });
}

void ProcessDetailsPanel::setProcessCapabilities(const Platform::ProcessCapabilities& capabilities)
{
    m_ProcessCapabilities = capabilities;
}

void ProcessDetailsPanel::setSelectedPid(std::int32_t pid, std::uint64_t startTimeTicks)
{
    // A different start time under the same PID is a different process (the PID was reused), so it is
    // a new selection and resets the pane like any other. Re-selecting what is already shown must not
    // wipe its history.
    if (ProcessDetailsLayout::snapshotIsSelectedProcess(m_SelectedPid, m_SelectedStartTicks, pid, startTimeTicks))
    {
        return;
    }

    m_SelectedPid = pid;
    m_SelectedStartTicks = startTimeTicks;
    m_History.clear();
    m_HistoryGeneration = UI::Widgets::nextChartDataGeneration();
    m_SampleIntake = {};
    m_CachedSnapshot.reset();
    m_HasSnapshot = false;
    m_ProcessExited = false;
    m_ActionsView.onSelectionChanged();
    m_SmoothedUsage.reset();
    m_CachedRateReadings = {};
    m_PeakMemoryBytes = 0.0;
    m_PriorityView.onSelectionChanged();    // Drops an edited priority, so it cannot reach the new process
    m_EnvironmentView.onSelectionChanged(); // Drops the variables and every revealed value (#179)
    m_ConnectionsView.onSelectionChanged(); // Drops the previous process's sockets (#799)
    m_ModulesView.onSelectionChanged();     // Drops the previous process's modules (#802)
    m_ThreadsView.onSelectionChanged();     // And its threads (#3069)
    m_SecurityView.onSelectionChanged();    // Drops the previous process's security context (#1526)
    m_OpenFilesView.onSelectionChanged();   // Drops the previous process's open files (#183)

    if (pid != -1)
    {
        spdlog::debug("ProcessDetailsPanel: selected PID {}", pid);
    }
}

void ProcessDetailsPanel::renderBasicInfo(const Domain::ProcessSnapshot& proc)
{
    const auto& theme = UI::Theme::get();

    // Note: ImGui requires null-terminated const char*; .c_str() is the correct approach here.
    const char* titleCommand = !proc.command.empty() ? proc.command.c_str() : proc.name.c_str();
    ImGui::TextWrapped("Command Line: %s", titleCommand);
    // Beside the command line, which names the executable, rather than as a row of the Identity card:
    // the card is full with a Publisher, and the line's value is wider than a card's (#1675).
    m_CrashHistoryView.render(proc.name);
    ImGui::Spacing();

    const auto computeLabelColumnWidth = [] -> float
    {
        // Use std::to_array for automatic size deduction — no manual count to maintain.
        constexpr auto labels = std::to_array<const char*>({
            "Name",
            "PID",
            "Parent PID",
            "User",
            "Started",
            "State",
            "Threads",
            "Handles",
            "CPU Time",
            "Priority",
            "Publisher",
            "Type",
            "Container",
            "Affinity",
            "Services",
            "Architecture",
        });

        // Measured, with the em-scaled gap every label column has (UI::LineLayout::labelColumnWidth()),
        // in place of a fixed 8px that stayed 8px at every font size and display density (#1200).
        return UI::Widgets::measureLabelColumnWidth(labels) + (ImGui::GetStyle().CellPadding.x * 2.0F);
    };

    const float labelColWidth = computeLabelColumnWidth();
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float halfWidth = (contentWidth - spacing) * 0.5F;

    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();

    auto rightAlignedText = [](std::string_view text, const ImVec4& color)
    {
        const float colWidth = ImGui::GetColumnWidth();
        const float textWidth = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
        const float padding = ImGui::GetStyle().CellPadding.x * 2.0F;
        const float targetX = ImGui::GetCursorPosX() + std::max(0.0F, colWidth - textWidth - padding);
        ImGui::SetCursorPosX(targetX);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopStyleColor();
    };

    // One label/value row of the two tables. Views: the values are the snapshot's own strings or the
    // text built from it below, both alive for the frame, so building the rows allocates nothing.
    struct InfoRow
    {
        std::string_view label;
        std::string_view value;
        ImVec4 color;
        // Shown on hovering the value; else a clipped value's own text. The initializer keeps the rows that
        // leave it out free of -Wmissing-designated-field-initializers.
        std::string_view tooltip{}; // NOLINT(readability-redundant-member-init)
    };
    // At most eight rows a table (Publisher, Affinity, Services, Type, Container and Architecture are optional), held in place.
    struct InfoRows
    {
        std::array<InfoRow, 8> rows{};
        std::size_t count = 0;

        void add(InfoRow row) noexcept
        {
            if (count < rows.size())
            {
                rows[count++] = row;
            }
        }
        [[nodiscard]] std::span<const InfoRow> view() const noexcept
        {
            return {rows.data(), count};
        }
    };

    auto renderInfoTable = [&](const char* tableId, std::span<const InfoRow> rows)
    {
        if (ImGui::BeginTable(tableId, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBody))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, labelColWidth);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            for (const auto& row : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
                ImGui::TextUnformatted(row.label.data(), row.label.data() + row.label.size());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                rightAlignedText(row.value, row.color);
                if (!row.tooltip.empty())
                {
                    ImGui::SetItemTooltip("%.*s", static_cast<int>(row.tooltip.size()), row.tooltip.data());
                }
                // A value wider than its column (a long affinity list, #1878) is clipped: its tooltip has it all.
                else if (ImGui::CalcTextSize(row.value.data(), row.value.data() + row.value.size()).x > ImGui::GetColumnWidth())
                {
                    ImGui::SetItemTooltip("%.*s", static_cast<int>(row.value.size()), row.value.data());
                }
            }

            ImGui::EndTable();
        }
    };

#ifdef _WIN32
    constexpr const char* handleLabel = "Handles";
#else
    constexpr const char* handleLabel = "FDs";
#endif

    // The values formatted from the snapshot -- PID, start time, counts, CPU time, priority -- are
    // built when a different snapshot is shown, once per sample, not every frame (#1171).
    // Only the panel's own snapshot is cached: holding it keeps its address from being reused by a
    // later one, which would otherwise look like the same key. Anything else (the empty placeholder
    // before a first sample) is rebuilt every time, since nothing pins its address. The values are
    // built in a fresh BasicInfoText and moved in whole, key last: a render exception is caught and
    // the app carries on, so a rebuild that throws part-way must not leave a matching key behind.
    BasicInfoText& text = m_BasicInfoText;
    const bool ownedSnapshot = m_CachedSnapshot.get() == &proc;
    if (!ownedSnapshot || text.key != &proc)
    {
        BasicInfoText fresh;
        const auto formatCountLocale = [](std::int64_t value) -> std::string
        {
            return UI::Format::formatOrDash(value, [](auto v) { return UI::Format::formatIntLocalized(v); });
        };
        fresh.pid = std::to_string(proc.pid);
        fresh.parentPid = std::to_string(proc.parentPid);
        fresh.started = (proc.startTimeEpoch > 0) ? UI::Format::formatEpochDateTimeShort(proc.startTimeEpoch) : std::string("-");
        fresh.threads = proc.threadCount > 0 ? formatCountLocale(proc.threadCount) : std::string("-");
        fresh.handles = "N/A"; // unreadable, e.g. another user's process without root (#1110)
        if (proc.handleCountAvailable)
        {
            fresh.handles = proc.handleCount > 0 ? formatCountLocale(proc.handleCount) : std::string("-");
        }
        fresh.cpuTime = UI::Format::formatDuration(proc.cpuTimeSeconds);
        fresh.priority = Detail::priorityDisplayText(proc.nice, Detail::PRIORITY_USES_WINDOWS_CLASSES); // No nice on Windows (#1204)
        // The Processes table's Status column (Suspended, Efficiency Mode) joins the State it qualifies,
        // as the Runtime card is full (#1878).
        fresh.state = proc.displayState;
        if (m_ProcessCapabilities.hasStatus && !proc.status.empty())
        {
            fresh.state += (fresh.state.empty() ? "" : " \xC2\xB7 ") + proc.status; // a middle dot
        }
        // An empty affinity was not read (another user's process), as the Affinity column says.
        fresh.affinity = proc.cpuAffinity.empty() ? std::string("N/A") : UI::Format::formatCpuAffinity(proc.cpuAffinity.words());
        fresh.keepAlive = ownedSnapshot ? m_CachedSnapshot : nullptr;
        fresh.key = ownedSnapshot ? &proc : nullptr;
        text = std::move(fresh);
    }

    // Build identity rows (conditionally include Publisher if available)
    InfoRows identityRows;
    identityRows.add({.label = "Name", .value = proc.name, .color = theme.scheme().textPrimary});
    identityRows.add({.label = "PID", .value = text.pid, .color = theme.scheme().textPrimary});
    identityRows.add({.label = "Parent PID", .value = text.parentPid, .color = theme.scheme().textPrimary});
    identityRows.add({.label = "User",
                      .value = proc.user.empty() ? std::string_view{"-"} : std::string_view{proc.user},
                      .color = theme.scheme().textPrimary});
    if (!proc.realUser.empty())
    {
        // A setuid program run by another user: User is who it runs as, this who started it (#2475).
        identityRows.add({.label = "Started by", .value = proc.realUser, .color = theme.scheme().textPrimary});
    }
    identityRows.add({.label = "Started", .value = text.started, .color = theme.scheme().textMuted});
    if (!proc.publisher.empty())
    {
        identityRows.add({.label = "Publisher", .value = proc.publisher, .color = theme.scheme().textMuted});
    }
    if (!proc.services.empty())
    {
        // The Processes table's Services column (#1886): the systemd service it runs under, on Linux.
        identityRows.add({.label = "Services", .value = proc.services, .color = theme.scheme().textPrimary});
    }
    if (m_ProcessCapabilities.hasCpuAffinity)
    {
        // The Processes table's Affinity column (#1878); a long list is clipped, its tooltip in full.
        identityRows.add({.label = "Affinity", .value = text.affinity, .color = theme.scheme().textPrimary});
    }

    // Build runtime rows (conditionally include Type if available)
    InfoRows runtimeRows;
    // Same name as the table's State column (#1203)
    runtimeRows.add({.label = "State", .value = text.state, .color = processStateColor(proc.displayState, theme.scheme())});
    runtimeRows.add({.label = "Threads", .value = text.threads, .color = theme.scheme().textPrimary});
    runtimeRows.add({.label = handleLabel, .value = text.handles, .color = theme.scheme().textPrimary});
    runtimeRows.add({.label = "CPU Time", .value = text.cpuTime, .color = theme.scheme().textPrimary});
    runtimeRows.add({.label = "Priority", .value = text.priority, .color = theme.scheme().textPrimary});
    if (!proc.processType.empty())
    {
        // The same colour as the table's Type column (#1180)
        runtimeRows.add({.label = "Type", .value = proc.processType, .color = processTypeColor(proc.processType, theme.scheme())});
    }
    if (!proc.container.empty())
    {
        runtimeRows.add({.label = "Container", .value = proc.container, .color = theme.scheme().textMuted});
    }
    if (!proc.architecture.empty())
    {
        // The Processes table's Architecture column (#2394), with what emulation costs on hover.
        runtimeRows.add({.label = "Architecture",
                         .value = proc.architecture,
                         .color = theme.scheme().textPrimary,
                         .tooltip = ProcessArchitectureNote::architectureNote(proc.architecture)});
    }
    // Identity and Runtime are cards with their header inside (#1537), both as tall as the taller of
    // the two, so their edges line up -- or as tall as Actions or Priority beside them, if taller.
    const auto tallerRowCount = static_cast<float>(std::max(identityRows.count, runtimeRows.count));
    const float cardHeight = ProcessDetailsLayout::computeInfoCardHeight(tallerRowCount, rowHeight, ImGui::GetStyle().WindowPadding.y);

    // Each block is capped at a readable width instead of taking half the pane, so a label and its
    // value stay together however wide the window is (#925). The blocks pack to the left and the
    // remaining width is left empty.
    const auto blockWidthFor = [&](std::span<const InfoRow> rows) -> float
    {
        float widestValue = 0.0F;
        for (const auto& row : rows)
        {
            widestValue = std::max(widestValue, ImGui::CalcTextSize(row.value.data(), row.value.data() + row.value.size()).x);
        }
        const ImGuiStyle& style = ImGui::GetStyle();
        const float contentNeeded = labelColWidth + widestValue + (style.CellPadding.x * 4.0F) + (style.WindowPadding.x * 2.0F);
        return ProcessDetailsLayout::computeInfoBlockWidth(ImGui::GetFontSize(), halfWidth, contentNeeded);
    };
    const float leftWidth = blockWidthFor(identityRows.view());
    const float rightWidth = blockWidthFor(runtimeRows.view());

    // Actions and Priority: what can be done to this process? Two cards, each as wide as its content:
    // on this row to the right of Runtime when the pane has room for all four, otherwise wrapped to a
    // row below (#1789). Every card on a row takes the tallest one's height (the Actions and Priority
    // cards' as last drawn), so their edges line up.
    const bool hasControls = ProcessActionsBlock::hasAnyAction(m_ActionCapabilities);
    ProcessDetailsLayout::TopRowLayout controlsLayout{};
    float topRowHeight = cardHeight;
    float controlsHeight = 0.0F;
    if (hasControls)
    {
        const ProcessActionsBlock::Widths controlWidths = ProcessActionsBlock::measure(m_ActionCapabilities);
        controlsLayout = ProcessDetailsLayout::chooseTopRowLayout(
            contentWidth, leftWidth + spacing + rightWidth, spacing, controlWidths.actionsCard(), controlWidths.priorityCard());
        // Only the heights of the cards drawn this frame count: a stale one must not stretch the row.
        const float actionsHeight = controlsLayout.actionsWidth > 0.0F ? m_ActionsCardHeight : 0.0F;
        const float priorityHeight = controlsLayout.priorityWidth > 0.0F ? m_PriorityCardHeight : 0.0F;
        if (controlsLayout.besideInfo)
        {
            topRowHeight = ProcessDetailsLayout::computeRowCardHeight(cardHeight, actionsHeight, priorityHeight);
            controlsHeight = topRowHeight;
        }
        else if (controlsLayout.priorityBesideActions)
        {
            // Wrapped side by side: a row of their own, its height the taller of theirs (0 before either
            // has been drawn, so each fits its content).
            controlsHeight = ProcessDetailsLayout::computeRowCardHeight(0.0F, actionsHeight, priorityHeight);
        }
        // Wrapped one under the other: each as tall as its content (controlsHeight 0).
    }

    // Identity section: Who is this process?
    (void) ProcessOverviewCard::render("BasicInfoLeft",
                                       ICON_FA_ID_CARD,
                                       "Identity",
                                       ImVec2(leftWidth, topRowHeight),
                                       ImGuiChildFlags_None,
                                       [&] { renderInfoTable("BasicInfoLeftTable", identityRows.view()); });

    ImGui::SameLine();

    // Runtime section: What is this process doing?
    (void) ProcessOverviewCard::render("BasicInfoRight",
                                       ICON_FA_CLOCK,
                                       "Runtime",
                                       ImVec2(rightWidth, topRowHeight),
                                       ImGuiChildFlags_None,
                                       [&] { renderInfoTable("BasicInfoRightTable", runtimeRows.view()); });

    // The buttons, confirm dialog and result line are ProcessActionsView's, the priority rows
    // ProcessPriorityView's (#1179); both act through m_ProcessActions, which the panel owns.
    if (hasControls)
    {
        if (controlsLayout.besideInfo)
        {
            ImGui::SameLine();
        }
        const ProcessActionsBlock::Context actions{
            .actionsView = &m_ActionsView,
            .priorityView = &m_PriorityView,
            .actions = m_ProcessActions.get(),
            .capabilities = m_ActionCapabilities,
            .processName = &proc.name,
            .target = selectedTarget(),
            .currentNice = m_HasSnapshot ? std::optional<std::int32_t>{proc.nice} : std::nullopt,
            .currentAffinity = m_HasSnapshot ? &proc.cpuAffinity : nullptr,
        };
        // Kept for the next frame's row height; a card scrolled out of view reports 0 and keeps its last.
        const ProcessActionsBlock::Heights needed = ProcessActionsBlock::render(actions, controlsLayout, controlsHeight);
        if (needed.actions > 0.0F)
        {
            m_ActionsCardHeight = needed.actions;
        }
        if (needed.priority > 0.0F)
        {
            m_PriorityCardHeight = needed.priority;
        }
    }
}

Platform::ProcessTarget ProcessDetailsPanel::selectedTarget() const
{
    return Detail::targetForSelection(m_SelectedPid, m_HasSnapshot ? m_CachedSnapshot.get() : nullptr);
}

} // namespace App
