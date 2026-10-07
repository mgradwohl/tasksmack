#include "ProcessDetailsPanel.h"

#include "App/Panel.h"
#include "App/Panels/ProcessStateColor.h"
#include "App/Panels/ProcessTypeColor.h"
#include "App/ShellMetrics.h"
#include "App/SyntheticScenario.h"
#include "App/TabLabel.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "ProcessActionsView.h"
#include "ProcessDetailsHistory.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "ProcessDetailsPanel_GpuHelpers.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessDetailsPanel_ResourceHelpers.h" // NOLINT(misc-include-cleaner) - used by the _WIN32 GDI code, which Linux analysis doesn't see
#include "ProcessPriorityView.h"
#include "UI/ChartWidgets.h"
#include "UI/EmptyState.h"
#include "UI/FillPlotLayout.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/InlineText.h"
#include "UI/RateAxis.h"
#include "UI/TabContent.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using UI::Widgets::computeAlpha;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::formatAxisLocalized;
using UI::Widgets::formatAxisWatts;
using UI::Widgets::frameTimeAxis;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::NowBarList;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::plotSeries;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::SeriesRole;
using UI::Widgets::seriesStyle;

// The NowBar columns each Process Details tab's charts reserve: the most bars any chart on that tab
// has, so charts stacked on one tab are the same width and their time axes line up (#1206), without
// a tab of one- or two-bar charts keeping empty columns for another tab's widest chart.
//
// Overview (CPU, Memory, Power, Resources): Resources has four on Windows (with GDI Objects); with a
// column of its own it was narrower than the CPU and Memory charts above it, and its time axis shorter.
#ifdef _WIN32
constexpr size_t PROCESS_OVERVIEW_NOW_BAR_COLUMNS = 4;
#else
constexpr size_t PROCESS_OVERVIEW_NOW_BAR_COLUMNS = 3;
#endif
// Network and I/O: Read and Write, Sent and Received.
constexpr size_t PROCESS_NETWORK_IO_NOW_BAR_COLUMNS = 2;
// GPU: Utilization, and Memory, one bar each.
constexpr size_t PROCESS_GPU_NOW_BAR_COLUMNS = 1;

// The newest @p count samples of a history, viewed in place (#1018: this was a per-frame copy).
[[nodiscard]] auto tailSpan(std::span<const double> data, std::size_t count) -> std::span<const double>
{
    return UI::Widgets::tailAlignedSpan(data, count).values;
}

// ImPlot series counts are int; keep conversion explicit + checked.

// One label per series, shared by its value-strip entry, tooltip row and NowBar (#1008).
constexpr const char* CPU_TOTAL_LABEL = "Total";
constexpr const char* CPU_USER_LABEL = "User";
constexpr const char* CPU_SYSTEM_LABEL = "System";
// The resident and peak pair carry the Processes table's column names, so the chart and the table
// call one quantity by one name (#1273).
constexpr const char* MEM_USED_LABEL = "Memory";
constexpr const char* MEM_SHARED_LABEL = "Shared";
// A series on a chart's right-hand axis ends in " →", pointing at it (setupSecondaryRateAxis(), #1206); in
// its value-strip entry and tooltip rows the mark follows the value (SECONDARY_AXIS_MARK, #1300).
constexpr const char* MEM_VIRTUAL_LABEL = "Virtual →";
// A peak is named "Peak " plus the series it tracks: this chart's series is "Memory", as the
// System chart's "Peak Used" tracks "Used" (#1342). The Processes table keeps its short "Peak Mem"
// header for width, as it does "Mem %".
constexpr const char* MEM_PEAK_LABEL = "Peak Memory";
constexpr const char* THREADS_LABEL = "Threads";
constexpr const char* FAULTS_LABEL = "Page Faults →"; // Its values carry the "/s" ("12.0/s"), #1202
#ifdef _WIN32
constexpr const char* GDI_LABEL = "GDI Objects";
#endif
constexpr const char* IO_READ_LABEL = "Read";
constexpr const char* IO_WRITE_LABEL = "Write";
constexpr const char* NET_SENT_LABEL = "Sent";
constexpr const char* NET_RECV_LABEL = "Received";
constexpr const char* POWER_LABEL = "Power";
constexpr const char* GPU_UTIL_LABEL = "Utilization";
constexpr const char* GPU_MEMORY_LABEL = "Memory";

/// Percent of system RAM per byte for this process's memory figures, from its own resident size and
/// resident percent (memoryPercent = memoryBytes / totalSystemMemoryBytes * 100). 0 when either is
/// zero, so every percent derived from it is 0 rather than a division by zero.
[[nodiscard]] double memoryPercentPerByte(const Domain::ProcessSnapshot& snapshot)
{
    const double usedPercent = std::clamp(snapshot.memoryPercent, 0.0, 100.0);
    if (usedPercent <= 0.0 || snapshot.memoryBytes == 0)
    {
        return 0.0;
    }
    return usedPercent / Domain::Numeric::toDouble(snapshot.memoryBytes);
}

/// A count history sample as text, or N/A for NaN (an unread value or a gap, #1110 / #1098): std::llround
/// of NaN is unspecified, so it must not reach formatIntLocalized().
[[nodiscard]] std::string formatCountOrNA(double value)
{
    return UI::Widgets::formatSampleOrNA(value, [](double v) { return UI::Format::formatIntLocalized(std::llround(v)); });
}

} // namespace

namespace App
{

using Detail::ProcessSeries;

// Constructor (inside App namespace)
ProcessDetailsPanel::ProcessDetailsPanel() : ProcessDetailsPanel(Synthetic::makeProcessActions(Synthetic::activeScenario()))
{}

ProcessDetailsPanel::ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions)
    : Panel("Process Details"),
      m_ProcessActions(std::move(processActions)),
      m_ActionCapabilities(m_ProcessActions ? m_ProcessActions->actionCapabilities() : Platform::ProcessActionCapabilities{})
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
                        m_SelectedUniqueKey,
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
        updateSmoothedUsage(*m_CachedSnapshot, deltaTime);
    }
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

void ProcessDetailsPanel::render(bool* open)
{
    // Keyed on the name shown (empty for none): the label is rebuilt only when it changes (#1326), and
    // shows all of the name, "#"s included, under the fixed ID "###ProcessDetails" (#1244).
    const std::string_view processName =
        (m_HasSnapshot && (m_SelectedPid != -1)) ? std::string_view{cachedSnapshot().name} : std::string_view{};
    const std::string& windowLabel = m_WindowLabel.get(
        processName, [](std::string_view name) { return TabLabel::makeProcessDetailsWindowLabel(ICON_FA_CIRCLE_INFO, name); });

    if (!ImGui::Begin(windowLabel.c_str(), open))
    {
        ImGui::End();
        return;
    }

    renderContent();

    ImGui::End();
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
        // Replaces the whole pane, Actions tab included: nothing here may act on a PID that no
        // longer belongs to this process.
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

    // Tabs for different info sections
    // Add padding inside tabs for better spacing, scaled like the style it overrides (#971)
    const float tabPaddingScale = UI::Theme::get().styleScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ShellMetrics::TAB_PADDING_X * tabPaddingScale, ShellMetrics::SUB_TAB_PADDING_Y * tabPaddingScale));

    if (ImGui::BeginTabBar("DetailsTabs", ImGuiTabBarFlags_DrawSelectedOverline))
    {
        // 1. Overview
        // Each tab's body scrolls in its own child, so the tab bar itself stays in view (#968).
        if (ImGui::BeginTabItem(ICON_FA_CIRCLE_INFO "  Overview"))
        {
            {
                const UI::Widgets::TabContentScope content("##OverviewContent");
                // The charts on this tab share its height (#959). The Identity/Runtime block above
                // them is inside the scope, so it is counted as non-plot height.
                UI::Widgets::FillPlotLayout fill(m_OverviewFill);
                // Its charts share their plot edges, with or without a right-hand axis (#1206).
                const UI::Widgets::AlignedChartStack alignedCharts("##ProcOverviewCharts");
                renderBasicInfo(cachedSnapshot());
                ImGui::Separator();
                renderResourceUsage(cachedSnapshot(), fill);
                ImGui::Separator();
                // Only where the platform measures it: Windows does not, and used to chart a
                // fabricated figure (#1028).
                if (m_ProcessCapabilities.hasPowerUsage)
                {
                    renderPowerUsage(cachedSnapshot(), fill);
                    ImGui::Separator();
                }
                renderThreadAndFaultHistory(fill);
            }
            ImGui::EndTabItem();
        }

        // 2. GPU (always show, with message if data unavailable)
        if (ImGui::BeginTabItem(ICON_FA_MICROCHIP "  GPU"))
        {
            {
                const UI::Widgets::TabContentScope content("##GpuContent");
                const auto& proc = cachedSnapshot();
                const Detail::GpuTabContent gpuContent =
                    Detail::gpuTabContent(m_CachedRateReadings.gpuSupported, // A failed read is not "not available on this system" (#1210)
                                          Detail::hasGpuUsageToShow(proc.gpuMemoryBytes,
                                                                    proc.gpuUtilPercent,
                                                                    !proc.gpuDevices.empty(),
                                                                    m_History.series(ProcessSeries::GpuUtil),
                                                                    m_History.series(ProcessSeries::GpuMemory)),
                                          Detail::hasAnyReading(m_History.series(ProcessSeries::GpuUtil)) ||
                                              Detail::hasAnyReading(m_History.series(ProcessSeries::GpuMemory)));
                if (gpuContent == Detail::GpuTabContent::Unavailable)
                {
                    // Not "no usage": without per-process metrics none can be seen (#1210).
                    UI::Widgets::renderEmptyState(ICON_FA_MICROCHIP "  Per-process GPU usage is not available",
                                                  "This system's GPU monitoring does not report GPU usage per process.");
                }
                else if (gpuContent == Detail::GpuTabContent::NoReadings)
                {
                    // Every retained read failed: nothing is known yet about this process's GPU use (#1210).
                    UI::Widgets::renderEmptyState(ICON_FA_MICROCHIP "  No GPU readings yet",
                                                  "Reading this process's GPU usage has not succeeded yet.");
                }
                else if (gpuContent == Detail::GpuTabContent::NoUsage)
                {
                    // Only the retained history is looked at, so the text names that window (#1210);
                    // rebuilt only when the window changes.
                    if (m_NoGpuUsageDetail.empty())
                    {
                        m_NoGpuUsageDetail = Detail::noGpuUsageDetail(m_MaxHistorySeconds);
                    }
                    UI::Widgets::renderEmptyState(ICON_FA_MICROCHIP "  No GPU usage", m_NoGpuUsageDetail.c_str());
                }
                else
                {
                    // The two history charts share the tab's height, like the other tabs' charts
                    // (#959). The metrics table and per-GPU breakdown above them count as non-plot.
                    UI::Widgets::FillPlotLayout fill(m_GpuFill);
                    const UI::Widgets::AlignedChartStack alignedCharts("##ProcGpuCharts"); // #1206
                    renderGpuUsage(cachedSnapshot(), fill);
                }
            }
            ImGui::EndTabItem();
        }

        // 3. Network and I/O. Always present, like the GPU tab, so the tab set does not change while a
        // process stays selected; an empty state stands in until there is data (#1210).
        if (ImGui::BeginTabItem(ICON_FA_NETWORK_WIRED "  Network and I/O"))
        {
            {
                const UI::Widgets::TabContentScope content("##NetworkContent");
                // Readings only: every sample adds a point, a gap where there was no reading, so a
                // history that is merely non-empty is not data (#1210).
                if (!Detail::hasNetworkOrIoReadings(m_History.series(ProcessSeries::IoRead),
                                                    m_History.series(ProcessSeries::IoWrite),
                                                    m_History.series(ProcessSeries::NetSent),
                                                    m_History.series(ProcessSeries::NetReceived)))
                {
                    UI::Widgets::renderEmptyState(ICON_FA_NETWORK_WIRED "  No network or disk I/O yet",
                                                  "Disk and network rates for this process appear here once they have been sampled.");
                }
                else
                {
                    UI::Widgets::FillPlotLayout fill(m_NetworkFill);
                    const UI::Widgets::AlignedChartStack alignedCharts("##ProcNetworkCharts"); // #1206
                    // Render I/O stats first (at the top)
                    renderIoStats(fill);
                    ImGui::Separator();
                    renderNetworkStats(fill);
                }
            }
            ImGui::EndTabItem();
        }

        // 4. Actions (last)
        if (ImGui::BeginTabItem(ICON_FA_GEARS "  Actions"))
        {
            {
                const UI::Widgets::TabContentScope content("##ActionsContent");
                renderActions();
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
            setSelectedPid(e.getPid(), e.getUniqueKey());
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
            m_NoGpuUsageDetail.clear(); // It names the window (#1210)
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

void ProcessDetailsPanel::setSelectedPid(std::int32_t pid, std::uint64_t uniqueKey)
{
    // A different key under the same PID is a different process (the PID was reused), so it is a
    // new selection and resets the pane like any other. An unknown key on either side is not a
    // different process: re-selecting what is already shown must not wipe its history.
    if (ProcessDetailsLayout::snapshotIsSelectedProcess(m_SelectedPid, m_SelectedUniqueKey, pid, uniqueKey))
    {
        if (m_SelectedUniqueKey == 0)
        {
            m_SelectedUniqueKey = uniqueKey;
        }
        return;
    }

    m_SelectedPid = pid;
    m_SelectedUniqueKey = uniqueKey;
    m_History.clear();
    m_HistoryGeneration = UI::Widgets::nextChartDataGeneration();
    m_SampleIntake = {};
    m_CachedSnapshot.reset();
    m_HasSnapshot = false;
    m_ProcessExited = false;
    m_ActionsView.onSelectionChanged();
    m_SmoothedUsage = {};
    m_CachedRateReadings = {};
    m_PeakMemoryBytes = 0.0;
    m_PriorityView.onSelectionChanged(); // Drops an edited priority, so it cannot reach the new process

    if (pid != -1)
    {
        spdlog::debug("ProcessDetailsPanel: selected PID {}", pid);
    }
}

void ProcessDetailsPanel::updateSmoothedUsage(const Domain::ProcessSnapshot& snapshot, float deltaTimeSeconds)
{
    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);

    const double targetCpu = UI::Format::clampPercent(snapshot.cpuPercent);
    const double targetResident = Domain::Numeric::toDouble(snapshot.memoryBytes);
    const double targetVirtual = Domain::Numeric::toDouble(std::max(snapshot.virtualBytes, snapshot.memoryBytes));
    const double targetCpuUser = UI::Format::clampPercent(snapshot.cpuUserPercent);
    const double targetCpuSystem = UI::Format::clampPercent(snapshot.cpuSystemPercent);
    const double targetThreads = Domain::Numeric::toDouble(snapshot.threadCount);
    const double targetFaults = std::max(0.0, snapshot.pageFaultsPerSec);
    const double targetPower = std::max(0.0, snapshot.powerWatts);
    const double targetGpuUtil = UI::Format::clampPercent(snapshot.gpuUtilPercent);
    const double targetGpuMem = Domain::Numeric::toDouble(snapshot.gpuMemoryBytes);
    const double targetMemShared = Domain::Numeric::toDouble(snapshot.sharedBytes);

    const bool initialized = m_SmoothedUsage.initialized && (deltaTimeSeconds > 0.0F);

    m_SmoothedUsage.cpuPercent = UI::Format::clampPercent(initializeOrSmooth(m_SmoothedUsage.cpuPercent, targetCpu, alpha, initialized));
    m_SmoothedUsage.residentBytes = std::max(0.0, initializeOrSmooth(m_SmoothedUsage.residentBytes, targetResident, alpha, initialized));
    m_SmoothedUsage.virtualBytes = initializeOrSmooth(m_SmoothedUsage.virtualBytes, targetVirtual, alpha, initialized);
    m_SmoothedUsage.virtualBytes = std::max(m_SmoothedUsage.virtualBytes, m_SmoothedUsage.residentBytes);
    m_SmoothedUsage.cpuUserPercent =
        UI::Format::clampPercent(initializeOrSmooth(m_SmoothedUsage.cpuUserPercent, targetCpuUser, alpha, initialized));
    m_SmoothedUsage.cpuSystemPercent =
        UI::Format::clampPercent(initializeOrSmooth(m_SmoothedUsage.cpuSystemPercent, targetCpuSystem, alpha, initialized));
    m_SmoothedUsage.threadCount = std::max(0.0, initializeOrSmooth(m_SmoothedUsage.threadCount, targetThreads, alpha, initialized));
    m_SmoothedUsage.pageFaultsPerSec =
        std::max(0.0, initializeOrSmooth(m_SmoothedUsage.pageFaultsPerSec, targetFaults, alpha, initialized));
    // Handle/FD count, I/O and network rates the probe could not read (#1110) aren't smoothed toward 0:
    // their NowBars show N/A, as their lines show a gap, and the next reading starts afresh.
    const auto smoothReading = [alpha, initialized](double& value, bool wasAvailable, bool available, double reading)
    {
        const auto next = Detail::smoothOptionalReading(
            {.value = value, .available = wasAvailable}, available ? std::optional<double>(reading) : std::nullopt, alpha, initialized);
        value = next.value;
    };
    smoothReading(m_SmoothedUsage.handleCount,
                  m_SmoothedUsage.handleCountAvailable,
                  snapshot.handleCountAvailable,
                  Domain::Numeric::toDouble(snapshot.handleCount));
    m_SmoothedUsage.handleCountAvailable = snapshot.handleCountAvailable;
    // A rate the probe could not supply at all when the shown sample was taken is not a reading
    // either (#1210); judged with that sample's own generation, as its history point was.
    const bool ioReading = m_CachedRateReadings.io;
    const bool networkReading = m_CachedRateReadings.network;
    smoothReading(m_SmoothedUsage.ioReadBytesPerSec, m_SmoothedUsage.ioAvailable, ioReading, snapshot.ioReadBytesPerSec);
    smoothReading(m_SmoothedUsage.ioWriteBytesPerSec, m_SmoothedUsage.ioAvailable, ioReading, snapshot.ioWriteBytesPerSec);
    m_SmoothedUsage.ioAvailable = ioReading;
    smoothReading(m_SmoothedUsage.netSentBytesPerSec, m_SmoothedUsage.networkAvailable, networkReading, snapshot.netSentBytesPerSec);
    smoothReading(m_SmoothedUsage.netRecvBytesPerSec, m_SmoothedUsage.networkAvailable, networkReading, snapshot.netReceivedBytesPerSec);
    m_SmoothedUsage.networkAvailable = networkReading;
    m_SmoothedUsage.powerWatts = std::max(0.0, initializeOrSmooth(m_SmoothedUsage.powerWatts, targetPower, alpha, initialized));
    // GPU utilization and memory the GPU probe did not supply for the shown sample's generation are
    // not readings either (#1210): not smoothed toward 0, so once support arrives the first real
    // reading starts afresh rather than easing up from placeholder zeros.
    const bool gpuUtilSupplied = m_CachedRateReadings.gpuUtilization;
    const bool gpuMemSupplied = m_CachedRateReadings.gpuPerProcess;
    smoothReading(m_SmoothedUsage.gpuUtilPercent, m_SmoothedUsage.gpuUtilAvailable, gpuUtilSupplied, targetGpuUtil);
    m_SmoothedUsage.gpuUtilPercent = UI::Format::clampPercent(m_SmoothedUsage.gpuUtilPercent);
    m_SmoothedUsage.gpuUtilAvailable = gpuUtilSupplied;
    smoothReading(m_SmoothedUsage.gpuMemoryBytes, m_SmoothedUsage.gpuMemoryAvailable, gpuMemSupplied, targetGpuMem);
    m_SmoothedUsage.gpuMemoryBytes = std::max(0.0, m_SmoothedUsage.gpuMemoryBytes);
    m_SmoothedUsage.gpuMemoryAvailable = gpuMemSupplied;
    // A sample with no GDI reading isn't smoothed toward 0: the NowBar shows N/A for it instead,
    // matching the gap in the line, and the next reading starts afresh (#1148).
    const auto gdi = Detail::smoothOptionalReading(
        {.value = m_SmoothedUsage.gdiObjectCount, .available = m_SmoothedUsage.gdiInitialized},
        snapshot.gdiObjectCount.has_value() ? std::optional<double>(Domain::Numeric::toDouble(*snapshot.gdiObjectCount)) : std::nullopt,
        alpha,
        initialized);
    m_SmoothedUsage.gdiObjectCount = gdi.value;
    m_SmoothedUsage.gdiInitialized = gdi.available;
    m_SmoothedUsage.memorySharedBytes =
        std::max(0.0, initializeOrSmooth(m_SmoothedUsage.memorySharedBytes, targetMemShared, alpha, initialized));
    m_SmoothedUsage.memoryPercentPerByte = memoryPercentPerByte(snapshot);
    m_SmoothedUsage.initialized = true;
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

        float maxTextWidth = 0.0F;
        for (const char* label : labels)
        {
            maxTextWidth = std::max(maxTextWidth, ImGui::CalcTextSize(label).x);
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        return maxTextWidth + (style.CellPadding.x * 2.0F) + 8.0F;
    };

    const float labelColWidth = computeLabelColumnWidth();
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float halfWidth = (contentWidth - spacing) * 0.5F;

    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    const float basePadding = ImGui::GetStyle().WindowPadding.y * 2.0F;

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
    const auto identityRowCount = static_cast<float>(identityRows.count);
    const float leftHeight = (rowHeight * identityRowCount) + basePadding;

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
    const auto runtimeRowCount = static_cast<float>(runtimeRows.count);
    const float rightHeight = (rowHeight * runtimeRowCount) + basePadding;

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
    ImGui::BeginGroup();
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_ID_CARD "  Identity");
    ImGui::BeginChild("BasicInfoLeft", ImVec2(leftWidth, leftHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    renderInfoTable("BasicInfoLeftTable", identityRows.view());
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    // Runtime section: What is this process doing?
    ImGui::BeginGroup();
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CLOCK "  Runtime");
    ImGui::BeginChild("BasicInfoRight", ImVec2(rightWidth, rightHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);

    renderInfoTable("BasicInfoRightTable", runtimeRows.view());
    ImGui::EndChild();
    ImGui::EndGroup();
}

void ProcessDetailsPanel::renderResourceUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill)
{
    // Ensure smoothing is initialized even if render is called before an update tick
    if (!m_SmoothedUsage.initialized)
    {
        updateSmoothedUsage(proc, m_LastDeltaSeconds);
    }

    renderCpuUsageSection(fill);
    renderMemoryUsageSection(fill);
}

// Renders the inline CPU history chart (total/user/system) plus paired "now" bars.
void ProcessDetailsPanel::renderCpuUsageSection(UI::Widgets::FillPlotLayout& fill)
{
    const auto& theme = UI::Theme::get();

    // Inline CPU history with paired now bar
    if (!m_History.empty())
    {
        const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)
        const size_t alignedCount = m_History.size();

        const auto timestamps = tailSpan(m_History.timestamps(), alignedCount);
        const auto cpuData = tailSpan(m_History.series(ProcessSeries::CpuTotal), alignedCount);
        const auto cpuUserData = tailSpan(m_History.series(ProcessSeries::CpuUser), alignedCount);
        const auto cpuSystemData = tailSpan(m_History.series(ProcessSeries::CpuSystem), alignedCount);

        const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
        const auto cpuTimeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

        // The process's CPU is a percent of the whole machine, so a fixed 0-100 axis drew a flat line
        // for any typical process: one busy thread on 16 logical CPUs is 6.25 %. The axis scales to the
        // data instead, from a 5 % floor up to 100, eased like a rate axis, and the bars share its bound
        // so each bar meets its line (#1195, #1003). Values show one decimal, as the table does. Every
        // axis here is sized to the samples in the window, not the one trimming keeps left of it (#1145).
        const double cpuAxisUpper = UI::Widgets::easedPercentAxisUpperBound(
            "##ProcOverviewCPU",
            std::max({UI::Widgets::maxOfSeriesSince(cpuTimeData, axisConfig.xMin, cpuData, cpuUserData, cpuSystemData),
                      m_SmoothedUsage.cpuPercent,
                      m_SmoothedUsage.cpuUserPercent,
                      m_SmoothedUsage.cpuSystemPercent}));

        // Use smoothed values for NowBars for consistent animation
        const NowBar cpuTotalNow{.valueText = UI::Format::percentOneDecimal(m_SmoothedUsage.cpuPercent),
                                 .label = CPU_TOTAL_LABEL,
                                 .tooltipText = {},
                                 .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.cpuPercent, cpuAxisUpper),
                                 .color = theme.scheme().chartCpu}; // The Total line's colour (#1192)
        const NowBar cpuUserNow{.valueText = UI::Format::percentOneDecimal(m_SmoothedUsage.cpuUserPercent),
                                .label = CPU_USER_LABEL,
                                .tooltipText = {},
                                .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.cpuUserPercent, cpuAxisUpper),
                                .color = theme.scheme().cpuUser};
        const NowBar cpuSystemNow{.valueText = UI::Format::percentOneDecimal(m_SmoothedUsage.cpuSystemPercent),
                                  .label = CPU_SYSTEM_LABEL,
                                  .tooltipText = {},
                                  .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.cpuSystemPercent, cpuAxisUpper),
                                  .color = theme.scheme().cpuSystem};

        auto cpuPlot = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(
                    UI::Widgets::rateHistoryConfigWithUpper(
                        "##ProcOverviewCPU", axisConfig.xMin, axisConfig.xMax, UI::Widgets::formatAxisPercent, cpuAxisUpper),
                    fill.plotHeight()),
                m_HistoryGeneration));
            if (chart.active())
            {
                UI::Widgets::drawCollectingHint(alignedCount);
                // alignedCount > 0 here: the section only renders with history (see above).

                // These bands and lines are drawn with ImPlot directly, so they are capped here like
                // every plotLineWithFill series (#1022). Reduced together, so the bands still line
                // up with each other and with the lines. Points are chosen by each drawn value --
                // User (also the user band's top), System and Total -- not by the cumulative system
                // top, which stays flat when System rises as User falls and would drop that spike.
                //
                // The choice of points is kept until the history changes (m_HistoryGeneration,
                // #1139): reducing and copying the whole history every frame was most of this chart's
                // cost at the largest history settings. Each frame only builds the kept points, at
                // most LINE_PLOT_MAX_POINTS_DENSE of them, into reused member buffers.
                const std::span<const UI::Widgets::ReducedPoint> points = m_CpuPlotReduction.points(
                    {.generation = m_HistoryGeneration,
                     .dataId = 0,
                     .count = alignedCount,
                     .maxOut = UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE},
                    [&](std::vector<UI::Widgets::ReducedPoint>& out)
                    {
                        UI::Widgets::reduceAlignedPoints<double>(
                            cpuTimeData, {cpuUserData, cpuSystemData, cpuData}, UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE, nowSeconds, out);
                    });

                // The User and System bands' edges (shared with the Overview, #1180), plus the Total and
                // System lines. A gap point is NaN in every series (see UI::Widgets::reduceAlignedSeries).
                // The User line is the User band's top, so it is drawn from that.
                auto& stack = m_CpuStack;
                UI::Widgets::buildUserSystemStack<double>(points, cpuTimeData, cpuUserData, cpuSystemData, stack);
                UI::Widgets::gatherReducedValues<double>(points, cpuData, m_CpuPlotTotal);
                UI::Widgets::gatherReducedValues<double>(points, cpuSystemData, m_CpuPlotSystem);

                // Bands and lines reach "now" like every plotLineWithFill series: the last sample
                // held to x = 0 (#1016), unless it is too old to pass for current (#1147). Built in
                // their own buffers, so the tooltip's lookup over cpuTimeData still finds real samples only.
                UI::Widgets::holdLastValuesToNow(stack.x,
                                                 {&stack.base, &stack.userTop, &stack.systemTop, &m_CpuPlotTotal, &m_CpuPlotSystem},
                                                 UI::Widgets::maxHoldSecondsForAxis(cpuTimeData));
                const int drawCount = UI::Format::checkedCount(stack.x.size());

                // The bands share their labels with the User and System lines below, so ImPlot
                // treats each band and its line as one item. Each band is filled only where both of
                // its own edges have a reading, so a reading missing from one band alone can't feed
                // NaN to the other (#1149).
                UI::Widgets::plotShadedBand(
                    CPU_USER_LABEL, stack.x.data(), stack.base.data(), stack.userTop.data(), drawCount, theme.scheme().cpuUserFill);
                UI::Widgets::plotShadedBand(CPU_SYSTEM_LABEL,
                                            stack.x.data(),
                                            stack.userTop.data(),
                                            stack.systemTop.data(),
                                            drawCount,
                                            theme.scheme().cpuSystemFill);

                // Total at the primary series' weight; it has no fill of its own, the bands above are
                // the fill. User and System are secondaries: lighter lines, each with its own marker
                // shape (shown on its value-strip swatch too), so they differ by more than colour (#1198).
                ImPlot::PlotLine(CPU_TOTAL_LABEL,
                                 stack.x.data(),
                                 m_CpuPlotTotal.data(),
                                 drawCount,
                                 {ImPlotProp_LineColor,
                                  theme.scheme().chartCpu,
                                  ImPlotProp_LineWeight,
                                  UI::Widgets::lineWeight(UI::Widgets::PRIMARY_SERIES_WEIGHT)});
                UI::Widgets::plotStyledLine(CPU_USER_LABEL,
                                            stack.x.data(),
                                            stack.userTop.data(),
                                            drawCount,
                                            theme.scheme().cpuUser,
                                            seriesStyle(SeriesRole::Secondary, 0));
                UI::Widgets::plotStyledLine(CPU_SYSTEM_LABEL,
                                            stack.x.data(),
                                            m_CpuPlotSystem.data(),
                                            drawCount,
                                            theme.scheme().cpuSystem,
                                            seriesStyle(SeriesRole::Secondary, 1));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(cpuTimeData, mouse.x))
                    {
                        if (*idxVal < alignedCount)
                        {
                            // Total in its line's colour, not progressColor, which matched nothing on the chart.
                            const std::array rows{
                                UI::Widgets::TooltipRow{.label = CPU_TOTAL_LABEL,
                                                        .color = theme.scheme().chartCpu,
                                                        .value = UI::Format::percentOneDecimal(cpuData[*idxVal])},
                                UI::Widgets::TooltipRow{.label = CPU_USER_LABEL,
                                                        .color = theme.scheme().cpuUser,
                                                        .value = UI::Format::percentOneDecimal(cpuUserData[*idxVal])},
                                UI::Widgets::TooltipRow{.label = CPU_SYSTEM_LABEL,
                                                        .color = theme.scheme().cpuSystem,
                                                        .value = UI::Format::percentOneDecimal(cpuSystemData[*idxVal])},
                            };
                            UI::Widgets::renderHistoryTooltip(cpuTimeData[*idxVal], rows);
                        }
                    }
                }
            }
        };

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MICROCHIP "  CPU (%zu samples)", alignedCount);
        renderHistoryWithNowBars("ProcessCPUHistoryOverview",
                                 fill.plotHeight(),
                                 cpuPlot,
                                 {cpuTotalNow, cpuUserNow, cpuSystemNow},
                                 false,
                                 PROCESS_OVERVIEW_NOW_BAR_COLUMNS);
        fill.addPlot();
        ImGui::Spacing();
    }
}

// Renders the inline memory history chart (used/shared/virtual, with a peak-line overlay)
// plus paired "now" bars.
void ProcessDetailsPanel::renderMemoryUsageSection(UI::Widgets::FillPlotLayout& fill)
{
    const auto& theme = UI::Theme::get();

    // Inline history for memory (overview) mirroring system memory chart layout
    if (!m_History.empty())
    {
        const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)
        const size_t alignedCount = m_History.size();

        if (alignedCount > 0)
        {
            const auto timestamps = tailSpan(m_History.timestamps(), alignedCount);
            const auto usedData = tailSpan(m_History.series(ProcessSeries::MemoryUsed), alignedCount);
            // Shared is not reported on Windows; its line, tooltip row and bar are left out there
            // rather than shown as a permanent 0 (#1035).
            const bool showShared = m_ProcessCapabilities.hasSharedMemory;
            const auto sharedData =
                showShared ? tailSpan(m_History.series(ProcessSeries::MemoryShared), alignedCount) : std::span<const double>{};
            const auto virtData = tailSpan(m_History.series(ProcessSeries::Virtual), alignedCount);

            const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
            const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

            // Smoothed like every other NowBar (#1012); these were the raw latest history sample, so
            // they stepped while the bars around them glided.
            const double usedNow = m_SmoothedUsage.residentBytes;
            const double sharedNow = m_SmoothedUsage.memorySharedBytes;
            // Used and Shared in bytes on an axis that scales to them (#1195), eased like a rate axis and
            // shared with their bars. The lifetime peak is left out of the scale: one far above today's
            // usage would flatten the line again; its value is in the strip and the tooltip.
            const double memAxisUpper = UI::Widgets::easedRateAxisUpperBound(
                "##ProcOverviewMemory",
                std::max({UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, usedData, sharedData), usedNow, sharedNow}),
                UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);
            // "412.0 MB (1.3% of RAM)": the bytes the chart plots and the share of RAM the table shows.
            const double percentPerByte = m_SmoothedUsage.memoryPercentPerByte;
            const auto withRamShare = [percentPerByte](double bytes) -> std::string
            {
                if (!std::isfinite(bytes))
                {
                    return "N/A"; // a gap point (#1098)
                }
                return std::format("{} ({} of RAM)",
                                   UI::Format::formatBytes(bytes),
                                   UI::Format::percentOneDecimal(std::clamp(bytes * percentPerByte, 0.0, 100.0)));
            };
            // Virtual size in bytes on its own right-hand axis (#992), eased like a rate axis and shared
            // with its bar, whose smoothed value it covers too.
            const double virtAxisUpper = UI::Widgets::easedRateAxisUpperBound(
                "##ProcOverviewMemory/Y2",
                UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, virtData),
                                               {m_SmoothedUsage.virtualBytes}),
                UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);

            NowBarList memoryBars;
            // Used and Shared carry their share of RAM in tooltipText (shown on hover and in the value
            // strip); Virtual has none, being mostly reserved address space.
            memoryBars.push_back({.valueText = UI::Format::formatBytes(usedNow),
                                  .label = MEM_USED_LABEL,
                                  .tooltipText = UI::InlineText::format("{}: {}", MEM_USED_LABEL, withRamShare(usedNow)),
                                  .value01 = UI::Widgets::normalizeToUnitInterval(usedNow, memAxisUpper),
                                  .color = theme.scheme().chartMemory});
            if (showShared)
            {
                memoryBars.push_back({
                    .valueText = UI::Format::formatBytes(sharedNow),
                    .label = MEM_SHARED_LABEL,
                    .tooltipText = UI::InlineText::format("{}: {}", MEM_SHARED_LABEL, withRamShare(sharedNow)),
                    .value01 = UI::Widgets::normalizeToUnitInterval(sharedNow, memAxisUpper),
                    .color = theme.scheme().chartCpu,
                });
            }
            memoryBars.push_back({.valueText = UI::Format::formatBytes(m_SmoothedUsage.virtualBytes),
                                  .label = MEM_VIRTUAL_LABEL,
                                  .tooltipText = {},
                                  .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.virtualBytes, virtAxisUpper),
                                  .color = theme.scheme().chartIo});

            auto memoryPlot = [&]()
            {
                const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                    UI::Widgets::withHeight(
                        UI::Widgets::rateHistoryConfigWithUpper(
                            "##ProcOverviewMemory", axisConfig.xMin, axisConfig.xMax, UI::Widgets::formatAxisBytes, memAxisUpper),
                        fill.plotHeight()),
                    m_HistoryGeneration));
                if (chart.active())
                {
                    UI::Widgets::setupSecondaryRateAxis(virtAxisUpper, UI::Widgets::formatAxisBytes, theme.scheme().chartIo);
                    UI::Widgets::drawCollectingHint(alignedCount);
                    // Draw peak working set as a horizontal reference line (never decreases)
                    if (m_PeakMemoryBytes > 0.0)
                    {
                        // Draw horizontal line at peak value across the entire X range
                        const double peakY = m_PeakMemoryBytes;
                        std::array<double, 2> peakX = {axisConfig.xMin, axisConfig.xMax};
                        std::array<double, 2> peakYVals = {peakY, peakY};
                        ImPlot::PlotLine(
                            MEM_PEAK_LABEL,
                            peakX.data(),
                            peakYVals.data(),
                            2,
                            {ImPlotProp_LineColor, theme.scheme().chartPeakLine, ImPlotProp_LineWeight, UI::Widgets::lineWeight(1.5F)});
                    }

                    if (!usedData.empty())
                    {
                        plotSeries(MEM_USED_LABEL,
                                   timeData.data(),
                                   usedData.data(),
                                   UI::Format::checkedCount(usedData.size()),
                                   theme.scheme().chartMemory,
                                   theme.scheme().chartMemoryFill,
                                   seriesStyle(SeriesRole::Primary));
                    }

                    if (!sharedData.empty())
                    {
                        plotSeries(MEM_SHARED_LABEL,
                                   timeData.data(),
                                   sharedData.data(),
                                   UI::Format::checkedCount(sharedData.size()),
                                   theme.scheme().chartCpu,
                                   theme.scheme().chartCpuFill,
                                   seriesStyle(SeriesRole::Secondary, 0));
                    }

                    if (!virtData.empty())
                    {
                        // Line only: a fill on its own scale would cover the Used and Shared areas.
                        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                        plotSeries(MEM_VIRTUAL_LABEL,
                                   timeData.data(),
                                   virtData.data(),
                                   UI::Format::checkedCount(virtData.size()),
                                   theme.scheme().chartIo,
                                   theme.scheme().chartIoFill,
                                   seriesStyle(SeriesRole::Secondary, 1));
                        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                    }

                    if (ImPlot::IsPlotHovered())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                        {
                            std::vector<UI::Widgets::TooltipRow> rows;
                            if (*idxVal < usedData.size())
                            {
                                rows.push_back({.label = MEM_USED_LABEL,
                                                .color = theme.scheme().chartMemory,
                                                .value = withRamShare(usedData[*idxVal])});
                            }
                            if (*idxVal < sharedData.size())
                            {
                                rows.push_back({.label = MEM_SHARED_LABEL,
                                                .color = theme.scheme().chartCpu,
                                                .value = withRamShare(sharedData[*idxVal])});
                            }
                            if (*idxVal < virtData.size())
                            {
                                rows.push_back({.label = MEM_VIRTUAL_LABEL,
                                                .color = theme.scheme().chartIo,
                                                .value = UI::Widgets::formatSampleOrNA(
                                                    virtData[*idxVal], [](double v) { return UI::Format::formatBytes(v); })});
                            }
                            if (m_PeakMemoryBytes > 0.0)
                            {
                                // The line's colour: this row was textWarning, which matched nothing (#1005).
                                rows.push_back({.label = MEM_PEAK_LABEL,
                                                .color = theme.scheme().chartPeakLine,
                                                .value = UI::Format::formatBytes(m_PeakMemoryBytes)});
                            }
                            UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                        }
                    }
                }
            };

            ImGui::Spacing();
            ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MEMORY "  Memory (%zu samples)", alignedCount);
            // Peak Memory is a line with a tooltip row but no bar; list it in the value strip too (#1193).
            const std::array peakEntry{UI::Widgets::ValueStripEntry{
                .label = MEM_PEAK_LABEL,
                .value = UI::Format::formatBytes(m_PeakMemoryBytes),
                .color = theme.scheme().chartPeakLine,
            }};
            const std::span<const UI::Widgets::ValueStripEntry> stripExtras = (m_PeakMemoryBytes > 0.0)
                                                                                ? std::span<const UI::Widgets::ValueStripEntry>(peakEntry)
                                                                                : std::span<const UI::Widgets::ValueStripEntry>{};
            renderHistoryWithNowBars("ProcessMemoryOverviewLayout",
                                     fill.plotHeight(),
                                     memoryPlot,
                                     memoryBars,
                                     false,
                                     PROCESS_OVERVIEW_NOW_BAR_COLUMNS,
                                     false,
                                     UI::Widgets::NowBarValues::Strip,
                                     stripExtras);
            fill.addPlot();
            ImGui::Spacing();
        }
    }
}

// Renders the thread/handle/page-fault history plot plus matching "now" bars.
// Aligns history buffers by their shared tail, smooths the latest sample for the
// bars, and returns early when no aligned data is available.
void ProcessDetailsPanel::renderThreadAndFaultHistory(UI::Widgets::FillPlotLayout& fill)
{
    if (m_History.empty())
    {
        return;
    }

    // Every series has one value per timestamp (Detail::ProcessDetailsHistory), so every plotted value
    // and tooltip lookup refers to the same point in time.
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)
    const size_t alignedCount = m_History.size();

    const auto& theme = UI::Theme::get();

    const auto timestamps = tailSpan(m_History.timestamps(), alignedCount);
    const auto threadData = tailSpan(m_History.series(ProcessSeries::Threads), alignedCount);
    const auto handleData = tailSpan(m_History.series(ProcessSeries::Handles), alignedCount);
    const auto faultData = tailSpan(m_History.series(ProcessSeries::PageFaults), alignedCount);

    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

#ifdef _WIN32
    // The GDI history can be shorter than the others, and ends at the same newest sample, so it
    // starts gdiTimeOffset timestamps in (#1001).
    const size_t gdiAlignedCount = std::min(alignedCount, m_History.series(ProcessSeries::GdiObjects).size());
    const auto gdiData = tailSpan(m_History.series(ProcessSeries::GdiObjects), gdiAlignedCount);
    const size_t gdiTimeOffset = Detail::seriesTimeOffset(alignedCount, gdiData.size());
    const bool hasGdiSamples = Detail::hasAnySample(gdiData);
#endif

    // Threads, handles (and GDI objects) are counts on the left axis; page faults are a rate, on
    // their own right-hand axis, so a fault spike no longer flattens the count lines (#1024). Each
    // bound covers every series drawn on its axis, and each bar is scaled to its series' axis, so a
    // bar and its line show a value at the same height (#1003). Each also covers its bars' smoothed
    // values, which can still be easing down from a peak that has just left the window (#1145).
    const double handlesNow = UI::Widgets::currentIfAvailable(m_SmoothedUsage.handleCountAvailable, m_SmoothedUsage.handleCount);
#ifdef _WIN32
    const double countSeriesMax =
        UI::Widgets::withCurrentValues(std::max(UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, threadData, handleData),
                                                UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, gdiData)),
                                       {m_SmoothedUsage.threadCount,
                                        handlesNow,
                                        UI::Widgets::currentIfAvailable(m_SmoothedUsage.gdiInitialized, m_SmoothedUsage.gdiObjectCount)});
#else
    const double countSeriesMax = UI::Widgets::withCurrentValues(
        UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, threadData, handleData), {m_SmoothedUsage.threadCount, handlesNow});
#endif
    const double countAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcThreadsFaults", countSeriesMax, UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);
    const double faultAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcThreadsFaults/Y2",
        UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, faultData),
                                       {m_SmoothedUsage.pageFaultsPerSec}),
        UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);

    const NowBar threadsBar{.valueText = UI::Format::formatIntLocalized(std::llround(m_SmoothedUsage.threadCount)),
                            .label = THREADS_LABEL,
                            .tooltipText = {}, // The fallback, "Threads: <value>", says it all (#1019)
                            .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.threadCount, countAxisUpper),
                            .color = theme.scheme().chartCpu};

#ifdef _WIN32
    constexpr const char* handleLabel = "Handles";
#else
    constexpr const char* handleLabel = "FDs";
#endif

    // An unreadable count (#1110) shows N/A, as its line shows a gap.
    const std::string handlesText = m_SmoothedUsage.handleCountAvailable
                                      ? UI::Format::formatIntLocalized(std::llround(m_SmoothedUsage.handleCount))
                                      : std::string("N/A");
    const NowBar handlesBar{.valueText = handlesText,
                            .label = handleLabel,
                            .tooltipText = {}, // The fallback, "<label>: <value>", says it all (#1019)
                            .value01 = m_SmoothedUsage.handleCountAvailable
                                         ? UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.handleCount, countAxisUpper)
                                         : 0.0,
                            .color = theme.scheme().chartMemory};

    const NowBar faultsBar{.valueText = UI::Format::formatCountPerSecond(m_SmoothedUsage.pageFaultsPerSec),
                           .label = FAULTS_LABEL,
                           .tooltipText = {},
                           .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.pageFaultsPerSec, faultAxisUpper),
                           // The line's colour: this bar was chartIo while its line is accentColor(3) (#1004).
                           .color = theme.accentColor(3)};

#ifdef _WIN32
    // GDI objects NowBar (Windows-only). A missing reading is NaN: it is skipped by the axis bound
    // and shown as N/A, and a series with no reading at all -- a process TaskSmack cannot open -- is
    // not drawn (#1000).
    const NowBar gdiBar{
        .valueText = m_SmoothedUsage.gdiInitialized ? UI::Format::formatIntLocalized(std::llround(m_SmoothedUsage.gdiObjectCount))
                                                    : std::string("N/A"),
        .label = GDI_LABEL,
        .tooltipText = {}, // The fallback, "GDI Objects: <value>", says it all (#1019)
        .value01 =
            m_SmoothedUsage.gdiInitialized ? UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.gdiObjectCount, countAxisUpper) : 0.0,
        .color = theme.accentColor(4)};
#endif

    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcThreadsFaults", axisConfig.xMin, axisConfig.xMax, formatAxisLocalized, countAxisUpper),
                                    fill.plotHeight()),
            m_HistoryGeneration));
        if (chart.active())
        {
            UI::Widgets::setupSecondaryRateAxis(faultAxisUpper, formatAxisLocalized, theme.accentColor(3));
            UI::Widgets::drawCollectingHint(alignedCount);
            const int plotCount = UI::Format::checkedCount(alignedCount);
            plotSeries(THREADS_LABEL,
                       timeData.data(),
                       threadData.data(),
                       plotCount,
                       theme.scheme().chartCpu,
                       theme.scheme().chartCpuFill,
                       seriesStyle(SeriesRole::Primary));
            plotSeries(handleLabel,
                       timeData.data(),
                       handleData.data(),
                       plotCount,
                       theme.scheme().chartMemory,
                       theme.scheme().chartMemoryFill,
                       seriesStyle(SeriesRole::Secondary, 0));
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            plotSeries(FAULTS_LABEL,
                       timeData.data(),
                       faultData.data(),
                       plotCount,
                       theme.accentColor(3),
                       std::nullopt,
                       seriesStyle(SeriesRole::Secondary, 1));
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);

#ifdef _WIN32
            if (hasGdiSamples && gdiTimeOffset < timeData.size())
            {
                const int gdiPlotCount = UI::Format::checkedCount(std::min(gdiData.size(), timeData.size() - gdiTimeOffset));
                plotSeries(GDI_LABEL,
                           std::span(timeData).subspan(gdiTimeOffset).data(),
                           gdiData.data(),
                           gdiPlotCount,
                           theme.accentColor(4),
                           std::nullopt,
                           seriesStyle(SeriesRole::Secondary, 2));
            }
#endif

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    if (*idxVal < alignedCount)
                    {
                        std::vector<UI::Widgets::TooltipRow> rows{
                            {.label = THREADS_LABEL, .color = theme.scheme().chartCpu, .value = formatCountOrNA(threadData[*idxVal])},
                            {.label = handleLabel, .color = theme.scheme().chartMemory, .value = formatCountOrNA(handleData[*idxVal])},
                            {.label = FAULTS_LABEL,
                             .color = theme.accentColor(3),
                             .value = UI::Widgets::formatSampleOrNA(faultData[*idxVal],
                                                                    [](double v) { return UI::Format::formatCountPerSecond(v); })},
                        };
#ifdef _WIN32
                        if (hasGdiSamples)
                        {
                            const auto gdiValue = Detail::seriesValueAt(gdiData, gdiTimeOffset, *idxVal);
                            rows.push_back(
                                {.label = GDI_LABEL,
                                 .color = theme.accentColor(4),
                                 .value = gdiValue ? UI::Format::formatIntLocalized(std::llround(*gdiValue)) : std::string("N/A")});
                        }
#endif
                        UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                    }
                }
            }
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GEARS "  Resources (%zu samples)", alignedCount);
#ifdef _WIN32
    // 4 NowBars on Windows: Threads, Handles, Page Faults, GDI Objects (PROCESS_OVERVIEW_NOW_BAR_COLUMNS)
    renderHistoryWithNowBars("ProcessResourceHistory",
                             fill.plotHeight(),
                             plot,
                             {threadsBar, handlesBar, faultsBar, gdiBar},
                             false,
                             PROCESS_OVERVIEW_NOW_BAR_COLUMNS);
    fill.addPlot();
#else
    renderHistoryWithNowBars(
        "ProcessResourceHistory", fill.plotHeight(), plot, {threadsBar, handlesBar, faultsBar}, false, PROCESS_OVERVIEW_NOW_BAR_COLUMNS);
    fill.addPlot();
#endif
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderIoStats(UI::Widgets::FillPlotLayout& fill)
{
    const size_t alignedCount = m_History.size();
    if (alignedCount == 0)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

    const auto timestamps = tailSpan(m_History.timestamps(), alignedCount);
    const auto readData = tailSpan(m_History.series(ProcessSeries::IoRead), alignedCount);
    const auto writeData = tailSpan(m_History.series(ProcessSeries::IoWrite), alignedCount);

    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

    // Unreadable I/O counters (#1110) show N/A, as their lines show a gap.
    const bool ioAvailable = m_SmoothedUsage.ioAvailable;

    // Compare the smoothed current rates with the visible history when scaling the NowBars, so either
    // a visible or newly observed peak remains representable, and a bar still easing down from a peak
    // that has just left the window is not clamped to full height (#1145).
    const double ioAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcIoHistory",
        UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, readData, writeData),
                                       {UI::Widgets::currentIfAvailable(ioAvailable, m_SmoothedUsage.ioReadBytesPerSec),
                                        UI::Widgets::currentIfAvailable(ioAvailable, m_SmoothedUsage.ioWriteBytesPerSec)}),
        UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    const auto readUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.ioReadBytesPerSec);
    const auto writeUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.ioWriteBytesPerSec);
    const NowBar readBar{
        .valueText = ioAvailable ? UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.ioReadBytesPerSec, readUnit) : std::string("N/A"),
        .label = IO_READ_LABEL,
        .tooltipText = {},
        .value01 = ioAvailable ? UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.ioReadBytesPerSec, ioAxisUpper) : 0.0,
        .color = theme.scheme().chartIo};

    const NowBar writeBar{.valueText = ioAvailable ? UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.ioWriteBytesPerSec, writeUnit)
                                                   : std::string("N/A"),
                          .label = IO_WRITE_LABEL,
                          .tooltipText = {},
                          .value01 =
                              ioAvailable ? UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.ioWriteBytesPerSec, ioAxisUpper) : 0.0,
                          .color = theme.scheme().chartIoWrite};

    // Keep the plot and its hover tooltip together: both consume the same aligned
    // vectors, and the lambda is rendered alongside the matching NowBars below.
    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcIoHistory", axisConfig.xMin, axisConfig.xMax, formatAxisBytesPerSec, ioAxisUpper),
                                    fill.plotHeight()),
            m_HistoryGeneration));
        if (chart.active())
        {
            UI::Widgets::drawCollectingHint(alignedCount);
            const int plotCount = UI::Format::checkedCount(alignedCount);
            plotSeries(IO_READ_LABEL,
                       timeData.data(),
                       readData.data(),
                       plotCount,
                       theme.scheme().chartIo,
                       theme.scheme().chartIoFill,
                       seriesStyle(SeriesRole::Primary));

            plotSeries(IO_WRITE_LABEL,
                       timeData.data(),
                       writeData.data(),
                       plotCount,
                       theme.scheme().chartIoWrite,
                       theme.scheme().chartIoWriteFill,
                       seriesStyle(SeriesRole::Secondary, 0));

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    if (*idxVal < alignedCount)
                    {
                        const std::array rows{
                            UI::Widgets::TooltipRow{.label = IO_READ_LABEL,
                                                    .color = theme.scheme().chartIo,
                                                    .value = UI::Format::formatBytesPerSecOrNA(readData[*idxVal])},
                            UI::Widgets::TooltipRow{.label = IO_WRITE_LABEL,
                                                    .color = theme.scheme().chartIoWrite,
                                                    .value = UI::Format::formatBytesPerSecOrNA(writeData[*idxVal])},
                        };
                        UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                    }
                }
            }
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_HARD_DRIVE "  I/O Statistics (%zu samples)", alignedCount);
    renderHistoryWithNowBars("ProcessIoHistory", fill.plotHeight(), plot, {readBar, writeBar}, false, PROCESS_NETWORK_IO_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderNetworkStats(UI::Widgets::FillPlotLayout& fill)
{
    const size_t alignedCount = m_History.size();
    if (alignedCount == 0)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

    const auto timestamps = tailSpan(m_History.timestamps(), alignedCount);
    const auto sentData = tailSpan(m_History.series(ProcessSeries::NetSent), alignedCount);
    const auto recvData = tailSpan(m_History.series(ProcessSeries::NetReceived), alignedCount);

    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

    // Network counters that couldn't be attributed to the process (#1110) show N/A, as their lines
    // show a gap.
    const bool netAvailable = m_SmoothedUsage.networkAvailable;

    // Scale the NowBars against both the visible peak and the smoothed current values, so a new
    // traffic burst cannot exceed the normalized range, nor a bar still easing down from a peak that
    // has just left the window (#1145).
    const double netAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcNetworkHistory",
        UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, sentData, recvData),
                                       {UI::Widgets::currentIfAvailable(netAvailable, m_SmoothedUsage.netSentBytesPerSec),
                                        UI::Widgets::currentIfAvailable(netAvailable, m_SmoothedUsage.netRecvBytesPerSec)}),
        UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    const auto sentUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.netSentBytesPerSec);
    const auto recvUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.netRecvBytesPerSec);
    const NowBar sentBar{.valueText = netAvailable ? UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.netSentBytesPerSec, sentUnit)
                                                   : std::string("N/A"),
                         .label = NET_SENT_LABEL,
                         .tooltipText = {},
                         .value01 =
                             netAvailable ? UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.netSentBytesPerSec, netAxisUpper) : 0.0,
                         .color = theme.scheme().chartNetTx};

    const NowBar recvBar{.valueText = netAvailable ? UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.netRecvBytesPerSec, recvUnit)
                                                   : std::string("N/A"),
                         .label = NET_RECV_LABEL,
                         .tooltipText = {},
                         .value01 =
                             netAvailable ? UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.netRecvBytesPerSec, netAxisUpper) : 0.0,
                         .color = theme.scheme().chartNetRx};

    // The plot lambda owns rendering and hover lookup over the same aligned
    // buffers; renderHistoryWithNowBars composes it with the summary bars.
    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcNetworkHistory", axisConfig.xMin, axisConfig.xMax, formatAxisBytesPerSec, netAxisUpper),
                                    fill.plotHeight()),
            m_HistoryGeneration));
        if (chart.active())
        {
            UI::Widgets::drawCollectingHint(alignedCount);
            const int plotCount = UI::Format::checkedCount(alignedCount);
            plotSeries(NET_SENT_LABEL,
                       timeData.data(),
                       sentData.data(),
                       plotCount,
                       theme.scheme().chartNetTx,
                       theme.scheme().chartNetTxFill,
                       seriesStyle(SeriesRole::Primary));

            plotSeries(NET_RECV_LABEL,
                       timeData.data(),
                       recvData.data(),
                       plotCount,
                       theme.scheme().chartNetRx,
                       theme.scheme().chartNetRxFill,
                       seriesStyle(SeriesRole::Secondary, 0));

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    if (*idxVal < alignedCount)
                    {
                        // Averages since the process was first seen, as the chart's heading says.
                        const std::array rows{
                            UI::Widgets::TooltipRow{.label = NET_SENT_LABEL,
                                                    .color = theme.scheme().chartNetTx,
                                                    .value = UI::Format::formatBytesPerSecOrNA(sentData[*idxVal])},
                            UI::Widgets::TooltipRow{.label = NET_RECV_LABEL,
                                                    .color = theme.scheme().chartNetRx,
                                                    .value = UI::Format::formatBytesPerSecOrNA(recvData[*idxVal])},
                        };
                        UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                    }
                }
            }
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_NETWORK_WIRED "  Network (%zu samples)", alignedCount);
    // The heading's tooltip is shown after the chart: its value strip is placed beside the heading,
    // the item drawn just before it, so nothing else is submitted between the two.
    const bool headingHovered = ImGui::IsItemHovered();
    renderHistoryWithNowBars(
        "ProcessNetworkHistory", fill.plotHeight(), plot, {sentBar, recvBar}, false, PROCESS_NETWORK_IO_NOW_BAR_COLUMNS);
    if (headingHovered)
    {
        ImGui::SetTooltip("Network bytes/sec between readings of the process's open connections. A refresh that reuses a cached reading "
                          "shows the last rate.");
    }
    fill.addPlot();
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderPowerUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill)
{
    const bool hasCurrent = proc.powerWatts > 0.0;
    if (m_History.empty() && !hasCurrent)
    {
        return;
    }

    const size_t alignedCount = m_History.size();

    const auto& theme = UI::Theme::get();
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

    const auto powerData = tailSpan(m_History.series(ProcessSeries::Power), alignedCount);
    const auto timestamps = tailSpan(m_History.timestamps(), alignedCount);
    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

    // Use smoothed value for NowBar; the axis covers it too, so the bar is not clamped while it eases
    // down from a peak that has just left the window (#1145).
    const double powerAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcPowerHistory",
        UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, powerData), {m_SmoothedUsage.powerWatts}),
        UI::Widgets::RATE_AXIS_MIN_SPAN_WATTS);

    const NowBar powerBar{.valueText = UI::Format::formatPowerOrZero(m_SmoothedUsage.powerWatts),
                          .label = POWER_LABEL,
                          .tooltipText = {},
                          .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.powerWatts, powerAxisUpper),
                          .color = theme.scheme().textInfo};

    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcPowerHistory", axisConfig.xMin, axisConfig.xMax, formatAxisWatts, powerAxisUpper),
                                    fill.plotHeight()),
            m_HistoryGeneration));
        if (chart.active())
        {
            UI::Widgets::drawCollectingHint(powerData.size());
            if (!powerData.empty())
            {
                plotLineWithFill(
                    POWER_LABEL, timeData.data(), powerData.data(), UI::Format::checkedCount(powerData.size()), theme.scheme().textInfo);

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        if (*idxVal < powerData.size())
                        {
                            const std::array rows{UI::Widgets::TooltipRow{
                                .label = POWER_LABEL,
                                .color = theme.scheme().textInfo,
                                .value = UI::Widgets::formatSampleOrNA(powerData[*idxVal],
                                                                       [](double v) { return UI::Format::formatPowerOrZero(v); })}};
                            UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                        }
                    }
                }
            }
            else
            {
                ImPlot::PlotDummy("Power");
            }
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_BOLT "  Power Usage (%zu samples)", alignedCount);
    renderHistoryWithNowBars("ProcessPowerHistory", fill.plotHeight(), plot, {powerBar}, false, PROCESS_OVERVIEW_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderGpuUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill)
{
    auto& theme = UI::Theme::get();

    // Throttle debug logging to avoid per-frame spam
    // Only log when GPU data changes or on first render of a new process
    // Using member variables ensures per-panel state tracking (not static)
    if (proc.pid != m_LastGpuLogPid || proc.gpuMemoryBytes != m_LastGpuLogMemoryBytes)
    {
        spdlog::debug("renderGpuUsage: PID {} util={:.1f}%, mem={}, devices='{}'",
                      proc.pid,
                      proc.gpuUtilPercent,
                      proc.gpuMemoryBytes,
                      proc.gpuDevices);

        m_LastGpuLogPid = proc.pid;
        m_LastGpuLogMemoryBytes = proc.gpuMemoryBytes;
    }

    // Show GPU info
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MICROCHIP "  GPU Usage");
    ImGui::Spacing();

    renderGpuCurrentMetricsTable(proc);

    ImGui::Spacing();
    // With one GPU the breakdown repeats the table above (#1207).
    if (Detail::shouldShowPerGpuBreakdown(proc.perGpuUsage.size()))
    {
        ImGui::Separator();
        ImGui::Spacing();

        renderPerGpuBreakdown(proc);
    }

    ImGui::Separator();
    ImGui::Spacing();

    renderGpuHistoryGraphs(fill);
}

// Renders the current-value GPU metrics table (utilization, memory, devices, engines,
// encoder/decoder) for the selected process.
void ProcessDetailsPanel::renderGpuCurrentMetricsTable(const Domain::ProcessSnapshot& proc) const
{
    const auto& theme = UI::Theme::get();

    // Every label this table can show. The label column is measured from them (#966), so a label
    // added below must be added here too -- which is why the rows use these constants rather than
    // repeating the strings.
    constexpr const char* LABEL_UTILIZATION = "GPU Utilization:";
    constexpr const char* LABEL_MEMORY = "GPU Memory:";
    constexpr const char* LABEL_DEDICATED = "  Dedicated:";
    constexpr const char* LABEL_SHARED = "  Shared:";
    constexpr const char* LABEL_DEVICES = "GPU Device(s):";
    constexpr const char* LABEL_ENGINES = "Active Engines:";
    constexpr const char* LABEL_ENCODER = "Video Encoder:";
    constexpr const char* LABEL_DECODER = "Video Decoder:";
    constexpr auto LABELS = std::to_array<const char*>(
        {LABEL_UTILIZATION, LABEL_MEMORY, LABEL_DEDICATED, LABEL_SHARED, LABEL_DEVICES, LABEL_ENGINES, LABEL_ENCODER, LABEL_DECODER});

    // Current GPU metrics
    if (ImGui::BeginTable("GPUCurrentMetrics", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, UI::Widgets::measureLabelColumnWidth(LABELS));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        // GPU Utilization
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(LABEL_UTILIZATION);
        ImGui::TableNextColumn();
        const ImVec4 gpuUtilColor = theme.scheme().gpuUtilization;
        ImGui::TextColored(
            gpuUtilColor, "%s", Detail::gpuUtilizationText(m_SmoothedUsage.gpuUtilAvailable, m_SmoothedUsage.gpuUtilPercent).c_str());

        // GPU Memory
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(LABEL_MEMORY);
        ImGui::TableNextColumn();
        const ImVec4 gpuMemColor = theme.scheme().gpuMemory;
        const std::string memStr =
            m_SmoothedUsage.gpuMemoryAvailable ? UI::Format::formatBytes(m_SmoothedUsage.gpuMemoryBytes) : std::string("N/A");
        ImGui::TextColored(gpuMemColor, "%s", memStr.c_str());

        // GPU Memory counts what each GPU's "used" figure on the GPU tab counts (#1164). Both kinds are
        // listed beneath it whenever that total doesn't already show them: shared memory is mapped, or
        // the dedicated bytes aren't what was counted (a shared-segment GPU with no shared use yet).
        if (proc.gpuSharedMemoryBytes > 0 || proc.gpuDedicatedMemoryBytes != proc.gpuMemoryBytes)
        {
            for (const auto& [label, bytes] :
                 {std::pair{LABEL_DEDICATED, proc.gpuDedicatedMemoryBytes}, std::pair{LABEL_SHARED, proc.gpuSharedMemoryBytes}})
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(label);
                ImGui::TableNextColumn();
                const std::string bytesStr = UI::Format::formatBytes(Domain::Numeric::toDouble(bytes));
                ImGui::TextColored(gpuMemColor, "%s", bytesStr.c_str());
            }
        }

        // GPU Device(s)
        if (!proc.gpuDevices.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(LABEL_DEVICES);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(proc.gpuDevices.c_str());
        }

        // GPU Engines
        if (!proc.gpuEngines.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(LABEL_ENGINES);
            ImGui::TableNextColumn();
            std::string enginesStr;
            for (size_t i = 0; i < proc.gpuEngines.size(); ++i)
            {
                if (i > 0)
                {
                    enginesStr += ", ";
                }
                enginesStr += proc.gpuEngines[i];
            }
            ImGui::TextUnformatted(enginesStr.c_str());
        }

        // Encoder/Decoder utilization
        if (proc.gpuEncoderUtil > 0.0)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(LABEL_ENCODER);
            ImGui::TableNextColumn();
            const ImVec4 encColor = theme.scheme().gpuEncoder;
            ImGui::TextColored(encColor, "%s", UI::Format::percentOneDecimal(proc.gpuEncoderUtil).c_str());
        }

        if (proc.gpuDecoderUtil > 0.0)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(LABEL_DECODER);
            ImGui::TableNextColumn();
            const ImVec4 decColor = theme.scheme().gpuDecoder;
            ImGui::TextColored(decColor, "%s", UI::Format::percentOneDecimal(proc.gpuDecoderUtil).c_str());
        }

        ImGui::EndTable();
    }
}

// Renders a collapsible per-GPU breakdown (utilization, memory, engines) for each entry in
// proc.perGpuUsage. No-op if that list is empty, regardless of how many GPUs the system has; the caller
// skips it for a single GPU (#1207).
void ProcessDetailsPanel::renderPerGpuBreakdown(const Domain::ProcessSnapshot& proc) const
{
    const auto& theme = UI::Theme::get();

    // Per-GPU breakdown if available
    if (!proc.perGpuUsage.empty())
    {
        ImGui::Text("Per-GPU Breakdown:");
        ImGui::Spacing();

        const ImVec4 gpuUtilColor = theme.scheme().gpuUtilization;
        const ImVec4 gpuMemColor = theme.scheme().gpuMemory;

        // As in renderGpuCurrentMetricsTable(): the label column is measured from these (#966).
        constexpr const char* LABEL_UTILIZATION = "Utilization:";
        constexpr const char* LABEL_MEMORY = "Memory:";
        constexpr const char* LABEL_DEDICATED = "  Dedicated:";
        constexpr const char* LABEL_SHARED = "  Shared:";
        constexpr const char* LABEL_ENGINES = "Engines:";
        constexpr auto LABELS = std::to_array<const char*>({LABEL_UTILIZATION, LABEL_MEMORY, LABEL_DEDICATED, LABEL_SHARED, LABEL_ENGINES});
        const float labelColumnWidth = UI::Widgets::measureLabelColumnWidth(LABELS);

        for (const auto& gpuUsage : proc.perGpuUsage)
        {
            // Same words as the system GPU tab (GpuSection.cpp), so one adapter is not described
            // two ways depending on which tab is open (#963).
            const std::string gpuLabel =
                std::format("{} {} [{}]", ICON_FA_MICROCHIP, gpuUsage.gpuName, gpuUsage.isIntegrated ? "Shared Memory" : "Discrete");

            if (ImGui::CollapsingHeader(gpuLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Indent();

                if (ImGui::BeginTable("PerGPUMetrics", 2, ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelColumnWidth);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(LABEL_UTILIZATION);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(
                        gpuUtilColor, "%s", Detail::gpuUtilizationText(m_CachedRateReadings.gpuUtilization, gpuUsage.utilPercent).c_str());

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(LABEL_MEMORY);
                    ImGui::TableNextColumn();
                    const std::string memoryStr = UI::Format::formatBytes(static_cast<double>(gpuUsage.memoryBytes));
                    ImGui::TextColored(gpuMemColor, "%s", memoryStr.c_str());

                    // As in renderGpuCurrentMetricsTable(): both kinds whenever the total doesn't show them (#1164).
                    if (gpuUsage.sharedMemoryBytes > 0 || gpuUsage.dedicatedMemoryBytes != gpuUsage.memoryBytes)
                    {
                        for (const auto& [label, bytes] : {std::pair{LABEL_DEDICATED, gpuUsage.dedicatedMemoryBytes},
                                                           std::pair{LABEL_SHARED, gpuUsage.sharedMemoryBytes}})
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(label);
                            ImGui::TableNextColumn();
                            const std::string bytesStr = UI::Format::formatBytes(Domain::Numeric::toDouble(bytes));
                            ImGui::TextColored(gpuMemColor, "%s", bytesStr.c_str());
                        }
                    }

                    if (!gpuUsage.engines.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(LABEL_ENGINES);
                        ImGui::TableNextColumn();
                        std::string engStr;
                        for (size_t i = 0; i < gpuUsage.engines.size(); ++i)
                        {
                            if (i > 0)
                            {
                                engStr += ", ";
                            }
                            engStr += gpuUsage.engines[i];
                        }
                        ImGui::TextUnformatted(engStr.c_str());
                    }

                    ImGui::EndTable();
                }

                ImGui::Unindent();
                ImGui::Spacing();
            }
        }
    }
}

// Renders the GPU utilization and memory history charts, or a "collecting data" placeholder
// until enough history has accumulated.
void ProcessDetailsPanel::renderGpuHistoryGraphs(UI::Widgets::FillPlotLayout& fill)
{
    auto& theme = UI::Theme::get();

    // GPU history graphs: drawn from the start, with the collecting hint until samples arrive, like
    // every other chart (#1013); this was a line of text until there was history.
    {
        // Every series has one value per timestamp (Detail::ProcessDetailsHistory), so none can be read
        // past its end (#1149).
        const size_t alignedCount = m_History.size();
        const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

        // Extract only what we need for the graphs
        const auto timestamps = tailSpan(m_History.timestamps(), alignedCount);
        const auto gpuUtilVec = tailSpan(m_History.series(ProcessSeries::GpuUtil), alignedCount);
        const auto gpuMemVec = tailSpan(m_History.series(ProcessSeries::GpuMemory), alignedCount);

        const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
        const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

        const int plotCount = UI::Format::checkedCount(alignedCount);

        // GPU Utilization graph (percent metric: locked 0-100 axis with percent formatter)
        auto plotGpuUtil = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(UI::Widgets::percentHistoryConfig("##GPUUtilPlot", axisConfig.xMin, axisConfig.xMax),
                                        fill.plotHeight()),
                m_HistoryGeneration));
            if (chart.active())
            {
                UI::Widgets::drawCollectingHint(alignedCount);
                if (plotCount > 0)
                {
                    plotLineWithFill(GPU_UTIL_LABEL,
                                     timeData.data(),
                                     gpuUtilVec.data(),
                                     plotCount,
                                     theme.scheme().gpuUtilization,
                                     theme.scheme().gpuUtilizationFill,
                                     2.0F,
                                     true,
                                     UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

                    // Tooltip
                    if (ImPlot::IsPlotHovered())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                        {
                            if (*idxVal < alignedCount)
                            {
                                const std::array rows{UI::Widgets::TooltipRow{
                                    .label = GPU_UTIL_LABEL,
                                    .color = theme.scheme().gpuUtilization,
                                    .value = UI::Format::percentOneDecimal(static_cast<double>(gpuUtilVec[*idxVal])),
                                }};
                                UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                            }
                        }
                    }
                }
                else
                {
                    ImPlot::PlotDummy(GPU_UTIL_LABEL);
                }
            }
        };

        // GPU Memory graph. One upper bound for its axis and its bar, so they agree (#1003), from the
        // samples in the window, not the one trimming keeps left of it (#1145), and the bar's smoothed
        // value, which can still be easing down from a peak that has just left it.
        const double gpuMemAxisUpper = UI::Widgets::easedRateAxisUpperBound(
            "##GPUMemPlot",
            UI::Widgets::withCurrentValues(
                UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, gpuMemVec),
                {UI::Widgets::currentIfAvailable(m_SmoothedUsage.gpuMemoryAvailable, m_SmoothedUsage.gpuMemoryBytes)}),
            UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);
        auto plotGpuMem = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(
                    UI::Widgets::rateHistoryConfigWithUpper(
                        "##GPUMemPlot", axisConfig.xMin, axisConfig.xMax, UI::Widgets::formatAxisBytes, gpuMemAxisUpper),
                    fill.plotHeight()),
                m_HistoryGeneration));
            if (chart.active())
            {
                UI::Widgets::drawCollectingHint(alignedCount);
                if (plotCount > 0)
                {
                    plotLineWithFill(GPU_MEMORY_LABEL,
                                     timeData.data(),
                                     gpuMemVec.data(),
                                     plotCount,
                                     theme.scheme().gpuMemory,
                                     theme.scheme().gpuMemoryFill,
                                     2.0F,
                                     true,
                                     UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

                    // Tooltip
                    if (ImPlot::IsPlotHovered())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                        {
                            if (*idxVal < alignedCount)
                            {
                                const std::array rows{
                                    UI::Widgets::TooltipRow{.label = GPU_MEMORY_LABEL,
                                                            .color = theme.scheme().gpuMemory,
                                                            .value = UI::Widgets::formatSampleOrNA(
                                                                gpuMemVec[*idxVal], [](double v) { return UI::Format::formatBytes(v); })}};
                                UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                            }
                        }
                    }
                }
                else
                {
                    ImPlot::PlotDummy("GPU Memory");
                }
            }
        };

        // Now bars for current values
        const NowBar gpuUtilBar{
            .valueText = Detail::gpuUtilizationText(m_SmoothedUsage.gpuUtilAvailable, m_SmoothedUsage.gpuUtilPercent),
            .label = GPU_UTIL_LABEL,
            .tooltipText = {},
            .value01 = m_SmoothedUsage.gpuUtilAvailable ? UI::Format::percent01(m_SmoothedUsage.gpuUtilPercent) : 0.0,
            .color = theme.scheme().gpuUtilization,
        };

        const NowBar gpuMemBar{
            .valueText = m_SmoothedUsage.gpuMemoryAvailable ? UI::Format::formatBytes(m_SmoothedUsage.gpuMemoryBytes) : std::string("N/A"),
            .label = GPU_MEMORY_LABEL,
            .tooltipText = {},
            .value01 = m_SmoothedUsage.gpuMemoryAvailable
                         ? UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.gpuMemoryBytes, gpuMemAxisUpper)
                         : 0.0,
            .color = theme.scheme().gpuMemory,
        };

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CHART_LINE "  GPU Utilization History (%zu samples)", alignedCount);
        renderHistoryWithNowBars("ProcessGPUUtilHistory", fill.plotHeight(), plotGpuUtil, {gpuUtilBar}, false, PROCESS_GPU_NOW_BAR_COLUMNS);
        fill.addPlot();
        ImGui::Spacing();

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CHART_LINE "  GPU Memory History (%zu samples)", alignedCount);
        renderHistoryWithNowBars("ProcessGPUMemHistory", fill.plotHeight(), plotGpuMem, {gpuMemBar}, false, PROCESS_GPU_NOW_BAR_COLUMNS);
        fill.addPlot();
        ImGui::Spacing();
    }
}

void ProcessDetailsPanel::renderActions()
{
    // The buttons, confirm dialog and result line are ProcessActionsView's, and the priority control
    // under them ProcessPriorityView's (#1179). Both act through m_ProcessActions, which the panel owns.
    const Platform::ProcessTarget target = selectedTarget();
    m_ActionsView.render(m_ProcessActions.get(), m_ActionCapabilities, cachedSnapshot().name, target);
    const std::optional<std::int32_t> currentNice = m_HasSnapshot ? std::optional<std::int32_t>{cachedSnapshot().nice} : std::nullopt;
    m_PriorityView.render(m_ProcessActions.get(), m_ActionCapabilities, currentNice, target);
}

Platform::ProcessTarget ProcessDetailsPanel::selectedTarget() const
{
    return Detail::targetForSelection(m_SelectedPid, m_HasSnapshot ? m_CachedSnapshot.get() : nullptr);
}

} // namespace App
