#include "ProcessDetailsPanel.h"

#include "App/Panel.h"
#include "App/Panels/ProcessStateColor.h"
#include "App/Panels/ProcessTypeColor.h"
#include "App/SelectOverride.h"
#include "App/ShellMetrics.h"
#include "App/SyntheticScenario.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessSecurity.h"
#include "ProcessActionsBlock.h"
#include "ProcessActionsView.h"
#include "ProcessConnectionsView.h"
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
#include "ProcessSecurityView.h"
#include "ProcessSmoothedUsage.h"
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
ProcessDetailsPanel::ProcessDetailsPanel()
    : ProcessDetailsPanel(Synthetic::makeProcessActions(Synthetic::activeScenario()),
                          Synthetic::makeProcessEnvironmentReader(Synthetic::activeScenario()),
                          Synthetic::makeProcessConnectionsReader(Synthetic::activeScenario()),
                          Synthetic::makeProcessModulesReader(Synthetic::activeScenario()),
                          Synthetic::makeProcessSecurityReader(Synthetic::activeScenario()),
                          Synthetic::makeProcessOpenFilesReader(Synthetic::activeScenario()))
{}

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

    // The Security section's on-demand read (#1526), on the same terms.
    const bool canReadSecurity = m_HasSecurity && m_HasSnapshot && !m_ProcessExited;
    static_cast<void>(m_SecurityView.update(canReadSecurity ? m_SecurityReader.get() : nullptr, selectedTarget(), deltaTime));
    // The Open files section's on-demand read (#183), on the same terms.
    const bool canReadOpenFiles = m_HasOpenFiles && m_HasSnapshot && !m_ProcessExited;
    static_cast<void>(m_OpenFilesView.update(canReadOpenFiles ? m_OpenFilesReader.get() : nullptr, selectedTarget(), deltaTime));
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
    };
}

const std::string& ProcessDetailsPanel::tabLabel() const
{
    if (m_HasSnapshot && (m_SelectedPid != -1) && !cachedSnapshot().name.empty())
    {
        return cachedSnapshot().name;
    }
    // Use static string to avoid heap allocation every frame for the default label
    static const std::string defaultLabel{"Select a process"};
    return defaultLabel;
}

void ProcessDetailsPanel::renderContent()
{
    // F9 (#170), taken now so it can only act on this frame: by the time the tabs below are drawn,
    // every reason not to (no selection, not on show, exited, not yet sampled) has returned early.
    const bool killRequested = m_KillShortcut.take();

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
                // Collapsed by default, and hidden where the platform cannot read environments (#179)
                m_EnvironmentView.render(m_HasEnvironment);
                // Likewise collapsed by default, and hidden where the platform cannot list sockets (#799)
                m_ConnectionsView.render(m_HasConnections);
                // Likewise collapsed by default; hidden only in synthetic runs (#802)
                m_ModulesView.render(m_HasModules);
                // Likewise collapsed by default; hidden on Windows until its token reader lands (#1526)
                m_SecurityView.render(m_HasSecurity);
                // Likewise collapsed by default; hidden only in synthetic runs (#183)
                m_OpenFilesView.render(m_HasOpenFiles);
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

        // 3. Network and I/O. Always present, like the GPU tab, so the tab set does not change while a
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

    // The Actions block's confirm dialog, submitted here every frame rather than from the block: the
    // block's child is skipped while the Overview is scrolled so that it is out of view, which left
    // an F9 Kill confirm pending with no dialog, refusing further F9 presses (#1493).
    m_ActionsView.renderConfirmation(m_ProcessActions.get(), selectedTarget());
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
    ImGui::Spacing();

    const auto computeLabelColumnWidth = []() -> float
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
    };
    // At most six rows a table (Publisher and Type are optional), held in place.
    struct InfoRows
    {
        std::array<InfoRow, 6> rows{};
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
    identityRows.add({.label = "Started", .value = text.started, .color = theme.scheme().textMuted});
    if (!proc.publisher.empty())
    {
        identityRows.add({.label = "Publisher", .value = proc.publisher, .color = theme.scheme().textMuted});
    }

    // Build runtime rows (conditionally include Type if available)
    InfoRows runtimeRows;
    // Same name as the table's State column (#1203)
    runtimeRows.add({.label = "State", .value = proc.displayState, .color = processStateColor(proc.displayState, theme.scheme())});
    runtimeRows.add({.label = "Threads", .value = text.threads, .color = theme.scheme().textPrimary});
    runtimeRows.add({.label = handleLabel, .value = text.handles, .color = theme.scheme().textPrimary});
    runtimeRows.add({.label = "CPU Time", .value = text.cpuTime, .color = theme.scheme().textPrimary});
    runtimeRows.add({.label = "Priority", .value = text.priority, .color = theme.scheme().textPrimary});
    if (!proc.processType.empty())
    {
        // The same colour as the table's Type column (#1180)
        runtimeRows.add({.label = "Type", .value = proc.processType, .color = processTypeColor(proc.processType, theme.scheme())});
    }
    // Identity and Runtime are cards with their header inside (#1537), both as tall as the taller of
    // the two, so their edges line up -- and Actions' too, beside them.
    const auto tallerRowCount = static_cast<float>(std::max(identityRows.count, runtimeRows.count));
    const float cardHeight = ProcessDetailsLayout::computeInfoCardHeight(tallerRowCount, rowHeight, ImGui::GetStyle().WindowPadding.y);

    // Each block is capped at a readable width instead of taking half the pane, so a label and its
    // value stay together however wide the window is (#925). The blocks pack to the left and the
    // remaining width is left empty.
    auto blockWidthFor = [&](std::span<const InfoRow> rows) -> float
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

    // Identity section: Who is this process?
    (void) ProcessOverviewCard::render("BasicInfoLeft",
                                       ICON_FA_ID_CARD,
                                       "Identity",
                                       ImVec2(leftWidth, cardHeight),
                                       ImGuiChildFlags_None,
                                       [&] { renderInfoTable("BasicInfoLeftTable", identityRows.view()); });

    ImGui::SameLine();

    // Runtime section: What is this process doing?
    (void) ProcessOverviewCard::render("BasicInfoRight",
                                       ICON_FA_CLOCK,
                                       "Runtime",
                                       ImVec2(rightWidth, cardHeight),
                                       ImGuiChildFlags_None,
                                       [&] { renderInfoTable("BasicInfoRightTable", runtimeRows.view()); });

    // Actions section: What can be done to this process? The buttons, confirm dialog and result line
    // are ProcessActionsView's, the priority rows ProcessPriorityView's (#1179); both act through
    // m_ProcessActions, which the panel owns. A third block on this row, as wide as its content, when
    // the pane leaves room for it, no taller than the other two so the charts keep their height;
    // otherwise wrapped below them (#1493).
    if (ProcessActionsBlock::hasAnyAction(m_ActionCapabilities))
    {
        const ProcessActionsBlock::Widths actionWidths = ProcessActionsBlock::measure(m_ActionCapabilities);
        const ProcessDetailsLayout::ActionsBlockLayout actionsLayout = ProcessDetailsLayout::computeActionsBlockLayout(
            contentWidth, leftWidth + spacing + rightWidth, spacing, actionWidths.content(), m_ActionsBlockHeight, cardHeight);
        if (actionsLayout.besideInfo)
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
        };
        // Kept for the next frame's placement: a block taller than the row wraps below it.
        if (const float needed = ProcessActionsBlock::render(actions, actionsLayout, cardHeight); needed > 0.0F)
        {
            m_ActionsBlockHeight = needed;
        }
    }
}

Platform::ProcessTarget ProcessDetailsPanel::selectedTarget() const
{
    return Detail::targetForSelection(m_SelectedPid, m_HasSnapshot ? m_CachedSnapshot.get() : nullptr);
}

} // namespace App
