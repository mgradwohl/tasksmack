#include "ProcessDetailsPanel.h"

#include "App/Panel.h"
#include "App/ShellMetrics.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/History.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Platform/Factory.h"
#include "Platform/IProcessActions.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "ProcessDetailsPanel_GpuHelpers.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessDetailsPanel_ResourceHelpers.h" // NOLINT(misc-include-cleaner) - used by the _WIN32 GDI code, which Linux analysis doesn't see
#include "UI/ChartWidgets.h"
#include "UI/DialogMetrics.h"
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
#include <limits>
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
using UI::Widgets::renderHistoryWithNowBars;

constexpr size_t PROCESS_NOW_BAR_COLUMNS = 3;

// Floor on the Confirm Action dialog's Yes/No buttons, in ems: 120px at the reference em.
constexpr float CONFIRM_BUTTON_MIN_EM = 11.25F;

// The newest @p count samples of a history, viewed in place (#1018: this was a per-frame copy).
[[nodiscard]] auto tailSpan(const std::vector<double>& data, std::size_t count) -> std::span<const double>
{
    return UI::Widgets::tailAlignedSpan(data, count).values;
}

// Drops the oldest @p count samples of a history (all of them if it has fewer).
void dropOldest(std::vector<double>& data, std::size_t count)
{
    data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(std::min(count, data.size())));
}

// ImPlot series counts are int; keep conversion explicit + checked.

// One label per series, shared by its legend entry, tooltip row and NowBar (#1008).
constexpr const char* CPU_TOTAL_LABEL = "Total";
constexpr const char* CPU_USER_LABEL = "User";
constexpr const char* CPU_SYSTEM_LABEL = "System";
constexpr const char* MEM_USED_LABEL = "Used";
constexpr const char* MEM_SHARED_LABEL = "Shared";
constexpr const char* MEM_VIRTUAL_LABEL = "Virtual";
constexpr const char* MEM_PEAK_LABEL = "Peak Used";
constexpr const char* THREADS_LABEL = "Threads";
constexpr const char* FAULTS_LABEL = "Page Faults/s";
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

// Import priority slider constants and helpers selectively
using Detail::getNiceColor;
using Detail::getNiceFromPosition;
using Detail::getNicePosition;
using Detail::NICE_MAX;
using Detail::NICE_MIN;
using Detail::NICE_RANGE;
using Detail::PRIORITY_APPLY_BUTTON_MIN_EM;
using Detail::PRIORITY_GRADIENT_SEGMENTS;
using Detail::PRIORITY_LABEL_PADDING_EM;

// Constructor (inside App namespace)
ProcessDetailsPanel::ProcessDetailsPanel() : ProcessDetailsPanel(Platform::makeProcessActions())
{}

ProcessDetailsPanel::ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions)
    : Panel("Process Details"),
      m_ProcessActions(std::move(processActions)),
      m_ActionCapabilities(m_ProcessActions ? m_ProcessActions->actionCapabilities() : Platform::ProcessActionCapabilities{})
{}

/// Captures all computed layout values in one place for helper methods
struct ProcessDetailsPanel::PrioritySliderContext
{
    ImDrawList* drawList = nullptr;
    ImVec2 cursorStart;         // Screen position where badge area starts
    ImVec2 sliderMin;           // Top-left of slider bar (screen coords)
    ImVec2 sliderMax;           // Bottom-right of slider bar (screen coords)
    float sliderLocalX = 0.0F;  // Slider X position in window-local coords (for cursor positioning)
    float normalizedPos = 0.0F; // 0.0 = nice -20, 1.0 = nice 19
    int32_t niceValue = 0;      // Current nice value
    const ImGuiStyle* style = nullptr;
    ImVec4 priorityHighColor;              // Theme color for high-priority end
    ImVec4 priorityNormalColor;            // Theme color for normal priority
    ImVec4 priorityLowColor;               // Theme color for low-priority end
    Detail::PrioritySliderMetrics metrics; // Font-derived pixel geometry for this frame
};

void ProcessDetailsPanel::updateWithSamples(std::span<const Domain::ProcessSample> samples, float deltaTime)
{
    m_LastDeltaSeconds = deltaTime;

    // Fade out action result message
    if (m_ActionResultTimer > 0.0F)
    {
        m_ActionResultTimer -= deltaTime;
        if (m_ActionResultTimer <= 0.0F)
        {
            m_LastActionResult = {};
        }
    }

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
                            recordHistoryPoint(*sample.snapshot, sample.sampleTimeSeconds, gapBefore);
                            recorded = true;
                        });
    if (recorded)
    {
        trimHistory(m_Timestamps.back());
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

void ProcessDetailsPanel::recordHistoryPoint(const Domain::ProcessSnapshot& snapshot, double sampleTimeSeconds, bool gapBefore)
{
    using Domain::Numeric::toDouble;

    // Every history, m_Timestamps included, gets one value per point, so they
    // stay aligned.
    const std::array<std::vector<double>*, 17> histories{&m_CpuHistory,
                                                         &m_CpuUserHistory,
                                                         &m_CpuSystemHistory,
                                                         &m_MemoryHistory,
                                                         &m_SharedHistory,
                                                         &m_VirtualHistory,
                                                         &m_ThreadHistory,
                                                         &m_HandleHistory,
                                                         &m_PageFaultHistory,
                                                         &m_IoReadHistory,
                                                         &m_IoWriteHistory,
                                                         &m_NetSentHistory,
                                                         &m_NetRecvHistory,
                                                         &m_PowerHistory,
                                                         &m_GpuUtilHistory,
                                                         &m_GpuMemHistory,
                                                         &m_GdiHistory};

    if (gapBefore && !m_Timestamps.empty() && sampleTimeSeconds > m_Timestamps.back())
    {
        // Generations were published here that are no longer available: NaN in
        // every series, so each chart shows a gap rather than a line drawn across
        // the missing samples.
        m_Timestamps.push_back((m_Timestamps.back() + sampleTimeSeconds) * 0.5);
        for (auto* history : histories)
        {
            history->push_back(std::numeric_limits<double>::quiet_NaN());
        }
    }

    // Stored as double to avoid narrowing; converted only at the ImPlot boundary.
    // In the order of `histories` above. A value the probe could not read is NaN,
    // drawn as a gap (#1110).
    const std::array<double, 17> values{
        snapshot.cpuPercent,
        snapshot.cpuUserPercent,
        snapshot.cpuSystemPercent,
        // Bytes, not a percent of RAM: a typical process is under 1 % of RAM,
        // which drew a flat line on a 0-100 % axis (#1195). The share of RAM is
        // shown in the tooltip and bar text instead.
        toDouble(snapshot.memoryBytes),
        toDouble(snapshot.sharedBytes),
        // Bytes, not a percent of RAM: a process's virtual size is usually larger
        // than physical RAM, so as a percent it was clamped to 100 and carried no
        // information (#992).
        toDouble(snapshot.virtualBytes),
        toDouble(snapshot.threadCount),
        Detail::readingOrGap(snapshot.handleCountAvailable, toDouble(snapshot.handleCount)),
        snapshot.pageFaultsPerSec,
        Detail::readingOrGap(snapshot.ioAvailable, snapshot.ioReadBytesPerSec),
        Detail::readingOrGap(snapshot.ioAvailable, snapshot.ioWriteBytesPerSec),
        Detail::readingOrGap(snapshot.networkAvailable, snapshot.netSentBytesPerSec),
        Detail::readingOrGap(snapshot.networkAvailable, snapshot.netReceivedBytesPerSec),
        snapshot.powerWatts,
        snapshot.gpuUtilPercent,
        toDouble(snapshot.gpuMemoryBytes),
        // NaN signals "no data" to the plot; ImPlot renders NaN as a gap in the
        // line.
        snapshot.gdiObjectCount.has_value() ? toDouble(*snapshot.gdiObjectCount) : std::numeric_limits<double>::quiet_NaN(),
    };
    m_Timestamps.push_back(sampleTimeSeconds);
    for (std::size_t i = 0; i < histories.size(); ++i)
    {
        histories[i]->push_back(values[i]);
    }

    // Peak working set in bytes, like the Used line it caps (never decreases)
    m_PeakMemoryBytes = std::max(m_PeakMemoryBytes, toDouble(snapshot.peakMemoryBytes));
}

const Domain::ProcessSnapshot& ProcessDetailsPanel::cachedSnapshot() const
{
    static const Domain::ProcessSnapshot empty{};
    return m_CachedSnapshot ? *m_CachedSnapshot : empty;
}

void ProcessDetailsPanel::render(bool* open)
{
    std::string windowLabel;
    if (m_HasSnapshot && (m_SelectedPid != -1) && !cachedSnapshot().name.empty())
    {
        windowLabel = std::string(ICON_FA_CIRCLE_INFO) + " " + cachedSnapshot().name;
        windowLabel += "###ProcessDetails";
    }
    else
    {
        windowLabel = ICON_FA_CIRCLE_INFO " Process Details###ProcessDetails";
    }

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
                if (!Detail::hasGpuUsageToShow(
                        proc.gpuMemoryBytes, proc.gpuUtilPercent, !proc.gpuDevices.empty(), m_GpuUtilHistory, m_GpuMemHistory))
                {
                    ImGui::TextUnformatted("No GPU usage detected for this process");
                }
                else
                {
                    // The two history charts share the tab's height, like the other tabs' charts
                    // (#959). The metrics table and per-GPU breakdown above them count as non-plot.
                    UI::Widgets::FillPlotLayout fill(m_GpuFill);
                    renderGpuUsage(cachedSnapshot(), fill);
                }
            }
            ImGui::EndTabItem();
        }

        // 3. Network and I/O - show if process has network or I/O data
        {
            const bool hasNetworkData = (cachedSnapshot().netSentBytesPerSec > 0.0 || cachedSnapshot().netReceivedBytesPerSec > 0.0 ||
                                         !m_NetSentHistory.empty() || !m_NetRecvHistory.empty());
            const bool hasIoData = (cachedSnapshot().ioReadBytesPerSec > 0.0 || cachedSnapshot().ioWriteBytesPerSec > 0.0 ||
                                    !m_IoReadHistory.empty() || !m_IoWriteHistory.empty());
            if (hasNetworkData || hasIoData)
            {
                if (ImGui::BeginTabItem(ICON_FA_NETWORK_WIRED "  Network and I/O"))
                {
                    {
                        const UI::Widgets::TabContentScope content("##NetworkContent");
                        UI::Widgets::FillPlotLayout fill(m_NetworkFill);
                        // Render I/O stats first (at the top)
                        renderIoStats(fill);
                        ImGui::Separator();
                        renderNetworkStats(fill);
                    }
                    ImGui::EndTabItem();
                }
            }
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
            if (!m_Timestamps.empty())
            {
                trimHistory(m_Timestamps.back());
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
    m_CpuHistory.clear();
    m_CpuUserHistory.clear();
    m_CpuSystemHistory.clear();
    m_MemoryHistory.clear();
    m_SharedHistory.clear();
    m_VirtualHistory.clear();
    m_ThreadHistory.clear();
    m_HandleHistory.clear();
    m_PageFaultHistory.clear();
    m_IoReadHistory.clear();
    m_IoWriteHistory.clear();
    m_NetSentHistory.clear();
    m_NetRecvHistory.clear();
    m_PowerHistory.clear();
    m_GpuUtilHistory.clear();
    m_GpuMemHistory.clear();
    m_GdiHistory.clear();
    m_Timestamps.clear();
    m_HistoryGeneration = UI::Widgets::nextChartDataGeneration();
    m_SampleIntake = {};
    m_CachedSnapshot.reset();
    m_HasSnapshot = false;
    m_ProcessExited = false;
    m_ShowConfirmDialog = false;
    m_LastActionResult = {};
    m_SmoothedUsage = {};
    m_PeakMemoryBytes = 0.0;
    m_PriorityChanged = false;
    m_PriorityNiceValue = 0;
    m_PriorityError.clear(); // Clear priority error when switching processes

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
    smoothReading(m_SmoothedUsage.ioReadBytesPerSec, m_SmoothedUsage.ioAvailable, snapshot.ioAvailable, snapshot.ioReadBytesPerSec);
    smoothReading(m_SmoothedUsage.ioWriteBytesPerSec, m_SmoothedUsage.ioAvailable, snapshot.ioAvailable, snapshot.ioWriteBytesPerSec);
    m_SmoothedUsage.ioAvailable = snapshot.ioAvailable;
    smoothReading(
        m_SmoothedUsage.netSentBytesPerSec, m_SmoothedUsage.networkAvailable, snapshot.networkAvailable, snapshot.netSentBytesPerSec);
    smoothReading(
        m_SmoothedUsage.netRecvBytesPerSec, m_SmoothedUsage.networkAvailable, snapshot.networkAvailable, snapshot.netReceivedBytesPerSec);
    m_SmoothedUsage.networkAvailable = snapshot.networkAvailable;
    m_SmoothedUsage.powerWatts = std::max(0.0, initializeOrSmooth(m_SmoothedUsage.powerWatts, targetPower, alpha, initialized));
    m_SmoothedUsage.gpuUtilPercent =
        UI::Format::clampPercent(initializeOrSmooth(m_SmoothedUsage.gpuUtilPercent, targetGpuUtil, alpha, initialized));
    m_SmoothedUsage.gpuMemoryBytes = std::max(0.0, initializeOrSmooth(m_SmoothedUsage.gpuMemoryBytes, targetGpuMem, alpha, initialized));
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

    const auto statusColorFor = [&theme](std::string_view state) -> ImVec4
    {
        if (state == "Running")
        {
            return theme.scheme().statusRunning;
        }
        if (state == "Sleeping")
        {
            return theme.scheme().statusSleeping;
        }
        if (state == "Disk Sleep")
        {
            return theme.scheme().statusDiskSleep;
        }
        if (state == "Zombie")
        {
            return theme.scheme().statusZombie;
        }
        if (state == "Stopped" || state == "Tracing")
        {
            return theme.scheme().statusStopped;
        }
        if (state == "Idle")
        {
            return theme.scheme().statusIdle;
        }
        return theme.scheme().textInfo;
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
    BasicInfoText& text = m_BasicInfoText;
    if (text.key != &proc)
    {
        text.key = &proc;
        // Holding the snapshot keeps its address from being reused by a later one, which would
        // otherwise look like the same key.
        text.keepAlive = (m_CachedSnapshot.get() == &proc) ? m_CachedSnapshot : nullptr;

        const auto formatCountLocale = [](std::int64_t value) -> std::string
        {
            return UI::Format::formatOrDash(value, [](auto v) { return UI::Format::formatIntLocalized(v); });
        };
        text.pid = std::to_string(proc.pid);
        text.parentPid = std::to_string(proc.parentPid);
        text.started = (proc.startTimeEpoch > 0) ? UI::Format::formatEpochDateTimeShort(proc.startTimeEpoch) : std::string("-");
        text.threads = proc.threadCount > 0 ? formatCountLocale(proc.threadCount) : std::string("-");
        text.handles = "N/A"; // unreadable, e.g. another user's process without root (#1110)
        if (proc.handleCountAvailable)
        {
            text.handles = proc.handleCount > 0 ? formatCountLocale(proc.handleCount) : std::string("-");
        }
        text.cpuTime = UI::Format::formatCpuTimeCompact(proc.cpuTimeSeconds);
        text.priority = Detail::priorityDisplayText(proc.nice, Detail::PRIORITY_USES_WINDOWS_CLASSES); // No nice on Windows (#1204)
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
    runtimeRows.add({.label = "State", .value = proc.displayState, .color = statusColorFor(proc.displayState)});
    runtimeRows.add({.label = "Threads", .value = text.threads, .color = theme.scheme().textPrimary});
    runtimeRows.add({.label = handleLabel, .value = text.handles, .color = theme.scheme().textPrimary});
    runtimeRows.add({.label = "CPU Time", .value = text.cpuTime, .color = theme.scheme().textPrimary});
    runtimeRows.add({.label = "Priority", .value = text.priority, .color = theme.scheme().textPrimary});
    if (!proc.processType.empty())
    {
        // Color-code the process type using status colors for visual clarity
        ImVec4 typeColor;
        if (proc.processType == "App")
        {
            typeColor = theme.scheme().statusRunning;
        }
        else if (proc.processType == "Windows Process")
        {
            typeColor = theme.scheme().textInfo;
        }
        else
        {
            typeColor = theme.scheme().textMuted;
        }
        runtimeRows.add({.label = "Type", .value = proc.processType, .color = typeColor});
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
    if (!m_Timestamps.empty() && !m_CpuHistory.empty())
    {
        const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)
        const size_t alignedCount =
            std::min({m_Timestamps.size(), m_CpuHistory.size(), m_CpuUserHistory.size(), m_CpuSystemHistory.size()});

        const auto timestamps = tailSpan(m_Timestamps, alignedCount);
        const auto cpuData = tailSpan(m_CpuHistory, alignedCount);
        const auto cpuUserData = tailSpan(m_CpuUserHistory, alignedCount);
        const auto cpuSystemData = tailSpan(m_CpuSystemHistory, alignedCount);

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

                auto& y0 = m_CpuStackY0;
                auto& yUserTop = m_CpuStackYUser;
                auto& ySystemTop = m_CpuStackYSystem;
                const std::size_t pointCount = points.size();
                m_CpuPlotX.resize(pointCount);
                m_CpuPlotTotal.resize(pointCount);
                m_CpuPlotUser.resize(pointCount);
                m_CpuPlotSystem.resize(pointCount);
                y0.assign(pointCount, 0.0);
                yUserTop.resize(pointCount);
                ySystemTop.resize(pointCount);
                for (std::size_t k = 0; k < pointCount; ++k)
                {
                    // A gap point is NaN in every series (see UI::Widgets::reduceAlignedSeries).
                    const auto i = static_cast<std::size_t>(points[k].index);
                    m_CpuPlotX[k] = cpuTimeData[i];
                    if (points[k].gap)
                    {
                        constexpr double gap = std::numeric_limits<double>::quiet_NaN();
                        m_CpuPlotTotal[k] = m_CpuPlotUser[k] = m_CpuPlotSystem[k] = yUserTop[k] = ySystemTop[k] = gap;
                        continue;
                    }
                    m_CpuPlotTotal[k] = cpuData[i];
                    m_CpuPlotUser[k] = cpuUserData[i];
                    m_CpuPlotSystem[k] = cpuSystemData[i];
                    // PlotShaded fills the area *between* two Y series, so a stacked user/system
                    // area chart needs cumulative tops: user alone, then user+system on top of it.
                    yUserTop[k] = cpuUserData[i];
                    ySystemTop[k] = cpuUserData[i] + cpuSystemData[i];
                }

                // Bands and lines reach "now" like every plotLineWithFill series: the last sample
                // held to x = 0 (UI::Widgets::holdLastValueToNow, #1016). Built in their own buffers,
                // so the tooltip's lookup over cpuTimeData still finds real samples only.
                if (!m_CpuPlotX.empty() && m_CpuPlotX.back() < 0.0)
                {
                    m_CpuPlotX.push_back(0.0);
                    for (auto* series : {&y0, &yUserTop, &ySystemTop, &m_CpuPlotTotal, &m_CpuPlotUser, &m_CpuPlotSystem})
                    {
                        series->push_back(series->back());
                    }
                }
                const int drawCount = UI::Format::checkedCount(m_CpuPlotX.size());

                // The bands share their labels with the User and System lines below, so ImPlot
                // treats each band and its line as one legend item: hiding "User" hides both.
                // With separate hidden labels the band stayed on screen after its line was hidden.
                // ImPlot's shaded renderer doesn't break at NaN, so the bands are filled run by run over
                // the finite points: a gap (a missing sample, or a UI stall that overran the sample ring,
                // #1098) is drawn as a gap rather than as fill triangles through NaN. A gap point is NaN
                // in every band, so the system top's runs serve both.
                UI::Widgets::forEachFiniteRun(ySystemTop.data(),
                                              drawCount,
                                              [&](int runStart, int runLength)
                                              {
                                                  const auto at = static_cast<std::size_t>(runStart);
                                                  ImPlot::PlotShaded(CPU_USER_LABEL,
                                                                     &m_CpuPlotX[at],
                                                                     &y0[at],
                                                                     &yUserTop[at],
                                                                     runLength,
                                                                     {ImPlotProp_FillColor, theme.scheme().cpuUserFill});
                                                  ImPlot::PlotShaded(CPU_SYSTEM_LABEL,
                                                                     &m_CpuPlotX[at],
                                                                     &yUserTop[at],
                                                                     &ySystemTop[at],
                                                                     runLength,
                                                                     {ImPlotProp_FillColor, theme.scheme().cpuSystemFill});
                                              });

                ImPlot::PlotLine(CPU_TOTAL_LABEL,
                                 m_CpuPlotX.data(),
                                 m_CpuPlotTotal.data(),
                                 drawCount,
                                 {ImPlotProp_LineColor, theme.scheme().chartCpu, ImPlotProp_LineWeight, UI::Widgets::lineWeight(2.0F)});

                ImPlot::PlotLine(CPU_USER_LABEL,
                                 m_CpuPlotX.data(),
                                 m_CpuPlotUser.data(),
                                 drawCount,
                                 {ImPlotProp_LineColor, theme.scheme().cpuUser, ImPlotProp_LineWeight, UI::Widgets::lineWeight(2.0F)});

                ImPlot::PlotLine(CPU_SYSTEM_LABEL,
                                 m_CpuPlotX.data(),
                                 m_CpuPlotSystem.data(),
                                 drawCount,
                                 {ImPlotProp_LineColor, theme.scheme().cpuSystem, ImPlotProp_LineWeight, UI::Widgets::lineWeight(2.0F)});

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
                                 PROCESS_NOW_BAR_COLUMNS);
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
    if (!m_Timestamps.empty())
    {
        const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)
        const size_t alignedCount =
            std::min({m_Timestamps.size(), m_MemoryHistory.size(), m_SharedHistory.size(), m_VirtualHistory.size()});

        if (alignedCount > 0)
        {
            const auto timestamps = tailSpan(m_Timestamps, alignedCount);
            const auto usedData = tailSpan(m_MemoryHistory, alignedCount);
            // Shared is not reported on Windows; its line, tooltip row and bar are left out there
            // rather than shown as a permanent 0 (#1035).
            const bool showShared = m_ProcessCapabilities.hasSharedMemory;
            const auto sharedData = showShared ? tailSpan(m_SharedHistory, alignedCount) : std::span<const double>{};
            const auto virtData = tailSpan(m_VirtualHistory, alignedCount);

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
            // with its bar.
            const double virtAxisUpper =
                UI::Widgets::easedRateAxisUpperBound("##ProcOverviewMemory/Y2",
                                                     UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, virtData),
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
                // Four legend entries (Used, Shared, Virtual, Peak Used): one row (see legendHorizontal).
                const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                    UI::Widgets::withHeight(
                        UI::Widgets::withHorizontalLegend(UI::Widgets::rateHistoryConfigWithUpper(
                            "##ProcOverviewMemory", axisConfig.xMin, axisConfig.xMax, UI::Widgets::formatAxisBytes, memAxisUpper)),
                        fill.plotHeight()),
                    m_HistoryGeneration));
                if (chart.active())
                {
                    UI::Widgets::setupSecondaryRateAxis(virtAxisUpper, UI::Widgets::formatAxisBytes);
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
                        plotLineWithFill(MEM_USED_LABEL,
                                         timeData.data(),
                                         usedData.data(),
                                         UI::Format::checkedCount(usedData.size()),
                                         theme.scheme().chartMemory,
                                         theme.scheme().chartMemoryFill);
                    }

                    if (!sharedData.empty())
                    {
                        plotLineWithFill(MEM_SHARED_LABEL,
                                         timeData.data(),
                                         sharedData.data(),
                                         UI::Format::checkedCount(sharedData.size()),
                                         theme.scheme().chartCpu,
                                         theme.scheme().chartCpuFill);
                    }

                    if (!virtData.empty())
                    {
                        // Line only: a fill on its own scale would cover the Used and Shared areas.
                        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                        plotLineWithFill(MEM_VIRTUAL_LABEL,
                                         timeData.data(),
                                         virtData.data(),
                                         UI::Format::checkedCount(virtData.size()),
                                         theme.scheme().chartIo,
                                         theme.scheme().chartIoFill,
                                         2.0F,
                                         false);
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
            // Peak Used is a line with a tooltip row but no bar; list it in the value strip too (#1193).
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
                                     PROCESS_NOW_BAR_COLUMNS,
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
    if (m_Timestamps.empty() || (m_ThreadHistory.empty() && m_HandleHistory.empty() && m_PageFaultHistory.empty()))
    {
        return;
    }

    // Align the independently sampled histories to their newest shared window so
    // every plotted value and tooltip lookup refers to the same point in time.
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)
    const size_t alignedCount = std::min({m_Timestamps.size(), m_ThreadHistory.size(), m_HandleHistory.size(), m_PageFaultHistory.size()});
    if (alignedCount == 0)
    {
        return;
    }

    const auto& theme = UI::Theme::get();

    const auto timestamps = tailSpan(m_Timestamps, alignedCount);
    const auto threadData = tailSpan(m_ThreadHistory, alignedCount);
    const auto handleData = tailSpan(m_HandleHistory, alignedCount);
    const auto faultData = tailSpan(m_PageFaultHistory, alignedCount);

    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

#ifdef _WIN32
    // The GDI history can be shorter than the others, and ends at the same newest sample, so it
    // starts gdiTimeOffset timestamps in (#1001).
    const size_t gdiAlignedCount = std::min(alignedCount, m_GdiHistory.size());
    const auto gdiData = tailSpan(m_GdiHistory, gdiAlignedCount);
    const size_t gdiTimeOffset = Detail::seriesTimeOffset(alignedCount, gdiData.size());
    const bool hasGdiSamples = Detail::hasAnySample(gdiData);
#endif

    // Threads, handles (and GDI objects) are counts on the left axis; page faults are a rate, on
    // their own right-hand axis, so a fault spike no longer flattens the count lines (#1024). Each
    // bound covers every series drawn on its axis, and each bar is scaled to its series' axis, so a
    // bar and its line show a value at the same height (#1003).
#ifdef _WIN32
    const double countSeriesMax = std::max(UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, threadData, handleData),
                                           UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, gdiData));
#else
    const double countSeriesMax = UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, threadData, handleData);
#endif
    const double countAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcThreadsFaults", countSeriesMax, UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);
    const double faultAxisUpper = UI::Widgets::easedRateAxisUpperBound("##ProcThreadsFaults/Y2",
                                                                       UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, faultData),
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
        // One legend row: up to four short entries (with GDI on Windows) on a chart that shares the
        // pane's height (see HistoryChartConfig::legendHorizontal).
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(UI::Widgets::withHorizontalLegend(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcThreadsFaults", axisConfig.xMin, axisConfig.xMax, formatAxisLocalized, countAxisUpper)),
                                    fill.plotHeight()),
            m_HistoryGeneration));
        if (chart.active())
        {
            UI::Widgets::setupSecondaryRateAxis(faultAxisUpper, formatAxisLocalized);
            UI::Widgets::drawCollectingHint(alignedCount);
            const int plotCount = UI::Format::checkedCount(alignedCount);
            plotLineWithFill(THREADS_LABEL,
                             timeData.data(),
                             threadData.data(),
                             plotCount,
                             theme.scheme().chartCpu,
                             theme.scheme().chartCpuFill,
                             2.0F,
                             true,
                             UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
            plotLineWithFill(handleLabel,
                             timeData.data(),
                             handleData.data(),
                             plotCount,
                             theme.scheme().chartMemory,
                             theme.scheme().chartMemoryFill,
                             2.0F,
                             true,
                             UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            plotLineWithFill(FAULTS_LABEL,
                             timeData.data(),
                             faultData.data(),
                             plotCount,
                             theme.accentColor(3),
                             std::nullopt,
                             2.0F,
                             true,
                             UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);

#ifdef _WIN32
            if (hasGdiSamples && gdiTimeOffset < timeData.size())
            {
                const int gdiPlotCount = UI::Format::checkedCount(std::min(gdiData.size(), timeData.size() - gdiTimeOffset));
                plotLineWithFill(GDI_LABEL,
                                 std::span(timeData).subspan(gdiTimeOffset).data(),
                                 gdiData.data(),
                                 gdiPlotCount,
                                 theme.accentColor(4),
                                 std::nullopt,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
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
    // 4 NowBars on Windows: Threads, Handles, Page Faults, GDI Objects
    constexpr size_t RESOURCE_NOW_BAR_COLUMNS = 4;
    renderHistoryWithNowBars(
        "ProcessResourceHistory", fill.plotHeight(), plot, {threadsBar, handlesBar, faultsBar, gdiBar}, false, RESOURCE_NOW_BAR_COLUMNS);
    fill.addPlot();
#else
    renderHistoryWithNowBars(
        "ProcessResourceHistory", fill.plotHeight(), plot, {threadsBar, handlesBar, faultsBar}, false, PROCESS_NOW_BAR_COLUMNS);
    fill.addPlot();
#endif
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderIoStats(UI::Widgets::FillPlotLayout& fill)
{
    const size_t alignedCount = std::min({m_Timestamps.size(), m_IoReadHistory.size(), m_IoWriteHistory.size()});
    if (alignedCount == 0)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

    const auto timestamps = tailSpan(m_Timestamps, alignedCount);
    const auto readData = tailSpan(m_IoReadHistory, alignedCount);
    const auto writeData = tailSpan(m_IoWriteHistory, alignedCount);

    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

    // Compare the smoothed current rates with history when scaling the NowBars,
    // so either a historical or newly observed peak remains representable.
    const double ioAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcIoHistory",
                                             UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, readData, writeData),
                                             UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    const auto readUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.ioReadBytesPerSec);
    const auto writeUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.ioWriteBytesPerSec);

    // Unreadable I/O counters (#1110) show N/A, as their lines show a gap.
    const bool ioAvailable = m_SmoothedUsage.ioAvailable;
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
            plotLineWithFill(IO_READ_LABEL,
                             timeData.data(),
                             readData.data(),
                             plotCount,
                             theme.scheme().chartIo,
                             theme.scheme().chartIoFill,
                             2.0F,
                             true,
                             UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

            plotLineWithFill(IO_WRITE_LABEL,
                             timeData.data(),
                             writeData.data(),
                             plotCount,
                             theme.scheme().chartIoWrite,
                             theme.scheme().chartIoWriteFill,
                             2.0F,
                             true,
                             UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

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
    renderHistoryWithNowBars("ProcessIoHistory", fill.plotHeight(), plot, {readBar, writeBar}, false, PROCESS_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderNetworkStats(UI::Widgets::FillPlotLayout& fill)
{
    const size_t alignedCount = std::min({m_Timestamps.size(), m_NetSentHistory.size(), m_NetRecvHistory.size()});
    if (alignedCount == 0)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

    const auto timestamps = tailSpan(m_Timestamps, alignedCount);
    const auto sentData = tailSpan(m_NetSentHistory, alignedCount);
    const auto recvData = tailSpan(m_NetRecvHistory, alignedCount);

    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

    // Scale the NowBars against both the historical peak and smoothed current
    // value so a new traffic burst cannot exceed the normalized range.
    const double netAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcNetworkHistory",
                                             UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, sentData, recvData),
                                             UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    const auto sentUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.netSentBytesPerSec);
    const auto recvUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.netRecvBytesPerSec);

    // Network counters that couldn't be attributed to the process (#1110) show N/A, as their lines
    // show a gap.
    const bool netAvailable = m_SmoothedUsage.networkAvailable;
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
            plotLineWithFill(NET_SENT_LABEL,
                             timeData.data(),
                             sentData.data(),
                             plotCount,
                             theme.scheme().chartNetTx,
                             theme.scheme().chartNetTxFill,
                             2.0F,
                             true,
                             UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

            plotLineWithFill(NET_RECV_LABEL,
                             timeData.data(),
                             recvData.data(),
                             plotCount,
                             theme.scheme().chartNetRx,
                             theme.scheme().chartNetRxFill,
                             2.0F,
                             true,
                             UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

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
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Network bytes/sec between readings of the process's open connections. A refresh that reuses a cached reading "
                          "shows the last rate.");
    }
    renderHistoryWithNowBars("ProcessNetworkHistory", fill.plotHeight(), plot, {sentBar, recvBar}, false, PROCESS_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderPowerUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill)
{
    const bool hasCurrent = proc.powerWatts > 0.0;
    if (m_Timestamps.empty() && m_PowerHistory.empty() && !hasCurrent)
    {
        return;
    }

    const size_t alignedCount = std::min(m_Timestamps.size(), m_PowerHistory.size());
    if (alignedCount == 0 && !hasCurrent)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

    const auto powerData = tailSpan(m_PowerHistory, alignedCount);
    const auto timestamps = tailSpan(m_Timestamps, alignedCount);
    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);

    // Use smoothed value for NowBar
    const double powerAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcPowerHistory", UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, powerData), UI::Widgets::RATE_AXIS_MIN_SPAN_WATTS);

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
    renderHistoryWithNowBars("ProcessPowerHistory", fill.plotHeight(), plot, {powerBar}, false, PROCESS_NOW_BAR_COLUMNS);
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
    constexpr const char* LABEL_DEVICES = "GPU Device(s):";
    constexpr const char* LABEL_ENGINES = "Active Engines:";
    constexpr const char* LABEL_ENCODER = "Video Encoder:";
    constexpr const char* LABEL_DECODER = "Video Decoder:";
    constexpr auto LABELS =
        std::to_array<const char*>({LABEL_UTILIZATION, LABEL_MEMORY, LABEL_DEVICES, LABEL_ENGINES, LABEL_ENCODER, LABEL_DECODER});

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
        ImGui::TextColored(gpuUtilColor, "%.1f%%", m_SmoothedUsage.gpuUtilPercent);

        // GPU Memory
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(LABEL_MEMORY);
        ImGui::TableNextColumn();
        const ImVec4 gpuMemColor = theme.scheme().gpuMemory;
        const std::string memStr = UI::Format::formatBytes(m_SmoothedUsage.gpuMemoryBytes);
        ImGui::TextColored(gpuMemColor, "%s", memStr.c_str());

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
            ImGui::TextColored(encColor, "%.1f%%", proc.gpuEncoderUtil);
        }

        if (proc.gpuDecoderUtil > 0.0)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(LABEL_DECODER);
            ImGui::TableNextColumn();
            const ImVec4 decColor = theme.scheme().gpuDecoder;
            ImGui::TextColored(decColor, "%.1f%%", proc.gpuDecoderUtil);
        }

        ImGui::EndTable();
    }
}

// Renders a collapsible per-GPU breakdown (utilization, memory, engines) for each entry in
// proc.perGpuUsage. No-op if that list is empty, regardless of how many GPUs the system has; the caller
// skips it for a single GPU (#1207).
void ProcessDetailsPanel::renderPerGpuBreakdown(const Domain::ProcessSnapshot& proc)
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
        constexpr const char* LABEL_ENGINES = "Engines:";
        constexpr auto LABELS = std::to_array<const char*>({LABEL_UTILIZATION, LABEL_MEMORY, LABEL_ENGINES});
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
                    ImGui::TextColored(gpuUtilColor, "%.1f%%", gpuUsage.utilPercent);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(LABEL_MEMORY);
                    ImGui::TableNextColumn();
                    const std::string memoryStr = UI::Format::formatBytes(static_cast<double>(gpuUsage.memoryBytes));
                    ImGui::TextColored(gpuMemColor, "%s", memoryStr.c_str());

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
        const size_t alignedCount = std::min(m_GpuUtilHistory.size(), m_Timestamps.size());
        const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

        // Extract only what we need for the graphs
        const auto timestamps = tailSpan(m_Timestamps, alignedCount);
        const auto gpuUtilVec = tailSpan(m_GpuUtilHistory, alignedCount);
        const auto gpuMemVec = tailSpan(m_GpuMemHistory, alignedCount);

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
                                const std::array rows{
                                    UI::Widgets::TooltipRow{.label = GPU_UTIL_LABEL,
                                                            .color = theme.scheme().gpuUtilization,
                                                            .value = UI::Format::percentCompact(static_cast<double>(gpuUtilVec[*idxVal]))}};
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
        // samples in the window, not the one trimming keeps left of it (#1145).
        const double gpuMemAxisUpper = UI::Widgets::easedRateAxisUpperBound(
            "##GPUMemPlot", UI::Widgets::maxOfSeriesSince(timeData, axisConfig.xMin, gpuMemVec), UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);
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
            .valueText = UI::Format::percentCompact(m_SmoothedUsage.gpuUtilPercent),
            .label = GPU_UTIL_LABEL,
            .tooltipText = {},
            .value01 = UI::Format::percent01(m_SmoothedUsage.gpuUtilPercent),
            .color = theme.scheme().gpuUtilization,
        };

        const NowBar gpuMemBar{
            .valueText = UI::Format::formatBytes(m_SmoothedUsage.gpuMemoryBytes),
            .label = GPU_MEMORY_LABEL,
            .tooltipText = {},
            .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.gpuMemoryBytes, gpuMemAxisUpper),
            .color = theme.scheme().gpuMemory,
        };

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CHART_LINE "  GPU Utilization History (%zu samples)", alignedCount);
        renderHistoryWithNowBars("ProcessGPUUtilHistory", fill.plotHeight(), plotGpuUtil, {gpuUtilBar}, false, PROCESS_NOW_BAR_COLUMNS);
        fill.addPlot();
        ImGui::Spacing();

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CHART_LINE "  GPU Memory History (%zu samples)", alignedCount);
        renderHistoryWithNowBars("ProcessGPUMemHistory", fill.plotHeight(), plotGpuMem, {gpuMemBar}, false, PROCESS_NOW_BAR_COLUMNS);
        fill.addPlot();
        ImGui::Spacing();
    }
}

void ProcessDetailsPanel::trimHistory(double nowSeconds)
{
    const double cutoff = nowSeconds - m_MaxHistorySeconds;
    // Keep the newest sample before the cutoff, so the charts' lines run off the window's left edge
    // instead of leaving an empty strip there after every trim (#1016).
    // Only while a newer sample remains (the newest is always the current sample, so here one does
    // unless the window is shorter than the time since it -- then everything goes).
    // The anchor is dropped too when nothing newer remains or it is across a gap
    // (Domain::HistoryUtils::keepTrimAnchor).
    const size_t removeCount = Domain::HistoryUtils::trimCountBefore(m_Timestamps, cutoff);

    // One erase per buffer rather than a pop_front per sample: vector erase from the front shifts
    // the rest, so it must not run once per dropped sample.
    const auto trimHistoryFront = [removeCount](std::vector<double>& history)
    {
        dropOldest(history, removeCount);
    };
    trimHistoryFront(m_Timestamps);

    trimHistoryFront(m_CpuHistory);
    trimHistoryFront(m_CpuUserHistory);
    trimHistoryFront(m_CpuSystemHistory);
    trimHistoryFront(m_MemoryHistory);
    trimHistoryFront(m_SharedHistory);
    trimHistoryFront(m_VirtualHistory);
    trimHistoryFront(m_ThreadHistory);
    trimHistoryFront(m_HandleHistory);
    trimHistoryFront(m_PageFaultHistory);
    trimHistoryFront(m_IoReadHistory);
    trimHistoryFront(m_IoWriteHistory);
    trimHistoryFront(m_NetSentHistory);
    trimHistoryFront(m_NetRecvHistory);
    trimHistoryFront(m_PowerHistory);
    trimHistoryFront(m_GpuUtilHistory);
    trimHistoryFront(m_GpuMemHistory);
    trimHistoryFront(m_GdiHistory);

    // Keep all history buffers aligned to the smallest non-empty length.
    size_t minSize = std::numeric_limits<size_t>::max();
    const auto updateMin = [&minSize](size_t size)
    {
        if (size > 0)
        {
            minSize = std::min(minSize, size);
        }
    };

    updateMin(m_Timestamps.size());
    updateMin(m_CpuHistory.size());
    updateMin(m_CpuUserHistory.size());
    updateMin(m_CpuSystemHistory.size());
    updateMin(m_MemoryHistory.size());
    updateMin(m_SharedHistory.size());
    updateMin(m_VirtualHistory.size());
    updateMin(m_ThreadHistory.size());
    updateMin(m_HandleHistory.size());
    updateMin(m_PageFaultHistory.size());
    updateMin(m_IoReadHistory.size());
    updateMin(m_IoWriteHistory.size());
    updateMin(m_NetSentHistory.size());
    updateMin(m_NetRecvHistory.size());
    updateMin(m_PowerHistory.size());
    updateMin(m_GpuUtilHistory.size());
    updateMin(m_GpuMemHistory.size());
    updateMin(m_GdiHistory.size());

    if (minSize != std::numeric_limits<size_t>::max())
    {
        const auto trimToMin = [minSize](std::vector<double>& history)
        {
            if (history.size() > minSize)
            {
                dropOldest(history, history.size() - minSize);
            }
        };

        trimToMin(m_Timestamps);
        trimToMin(m_CpuHistory);
        trimToMin(m_CpuUserHistory);
        trimToMin(m_CpuSystemHistory);
        trimToMin(m_MemoryHistory);
        trimToMin(m_SharedHistory);
        trimToMin(m_VirtualHistory);
        trimToMin(m_ThreadHistory);
        trimToMin(m_HandleHistory);
        trimToMin(m_PageFaultHistory);
        trimToMin(m_IoReadHistory);
        trimToMin(m_IoWriteHistory);
        trimToMin(m_NetSentHistory);
        trimToMin(m_NetRecvHistory);
        trimToMin(m_PowerHistory);
        trimToMin(m_GpuUtilHistory);
        trimToMin(m_GpuMemHistory);
        trimToMin(m_GdiHistory);
    }
}

void ProcessDetailsPanel::renderActions()
{
    const auto& theme = UI::Theme::get();

    ImGui::Text("%s (PID %d)", cachedSnapshot().name.c_str(), m_SelectedPid);
    ImGui::Spacing();

    // Section: Process Control
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GEARS "  Process Control");
    ImGui::Spacing();

    renderActionResultFeedback();
    renderConfirmDialog();
    renderActionButtons();
    renderPrioritySection();
}

void ProcessDetailsPanel::renderActionResultFeedback()
{
    if (m_LastActionResult.empty())
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    // The colour comes from the result's flag, not from searching its text for "Error" (#1203).
    const ImVec4 color = m_LastActionResult.ok ? theme.scheme().textSuccess : theme.scheme().textError;
    ImGui::TextColored(color, "%s", m_LastActionResult.text.c_str());
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderConfirmDialog()
{
    // The title names the action and the process, "Kill firefox (PID 1234)?"; "###" keeps the
    // popup's ID fixed while the visible title changes with them (#1203).
    constexpr const char* CONFIRM_POPUP_ID = "###ConfirmAction";
    if (m_ShowConfirmDialog)
    {
        ImGui::OpenPopup(CONFIRM_POPUP_ID);
    }
    if (!ImGui::IsPopupOpen(CONFIRM_POPUP_ID))
    {
        return; // Nothing to draw; skip building the title every frame
    }

    const std::string popupTitle = Detail::confirmTitle(m_ConfirmAction, cachedSnapshot().name, m_SelectedPid) + CONFIRM_POPUP_ID;
    if (ImGui::BeginPopupModal(popupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // The dialog auto-fits, so it is bounded here: neither the question (which carries the
        // process name) nor the button row may be wider than the main window can show. See
        // ProcessDetailsLayout::computeConfirmContentBudget().
        const ImGuiStyle& confirmStyle = ImGui::GetStyle();
        const float contentBudget = ProcessDetailsLayout::computeConfirmContentBudget(
            ImGui::GetMainViewport()->WorkSize.x, UI::DialogMetrics::MAX_VIEWPORT_FRACTION, confirmStyle.WindowPadding.x);

        // States what the action does; the title can be cut short by a long name, so the body
        // names the process too (#1203).
        const std::string question = Detail::confirmBody(m_ConfirmAction, cachedSnapshot().name, m_SelectedPid);
        // Wrapped at the budget, or at the text's own width when that is narrower -- a wrap
        // position wider than the text would make the auto-fitting dialog as wide as the budget.
        const float questionWidth = ImGui::CalcTextSize(question.c_str()).x;
        const float wrapWidth = (contentBudget > 0.0F) ? std::min(questionWidth, contentBudget) : questionWidth;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::TextUnformatted(question.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // One width for both, from the font: 11.25 em is the former fixed 120px at the reference
        // em, so the dialog is unchanged there and the buttons stay a comfortable target for a
        // destructive confirmation at any font size or display density (#971).
        //
        // Held to half the dialog's budget: at Even Huger on a 175% display each button wants
        // 420px, and the pair would be wider than a minimum-width window.
        //
        // The confirm button is named for the action ([Kill][Cancel], not [Yes][No]) so a
        // destructive confirmation says what it does on the button itself (#1203).
        const char* confirmLabel = Detail::actionLabel(m_ConfirmAction);
        const float confirmButtonWidth = ProcessDetailsLayout::computeConfirmButtonWidth(
            UI::DialogMetrics::computeActionButtonWidth(std::max(ImGui::CalcTextSize(confirmLabel).x, ImGui::CalcTextSize("Cancel").x),
                                                        ImGui::GetFontSize(),
                                                        CONFIRM_BUTTON_MIN_EM),
            contentBudget,
            confirmStyle.ItemSpacing.x);

        if (ImGui::Button(confirmLabel, ImVec2(confirmButtonWidth, 0.0F)))
        {
            dispatchConfirmedAction();
            m_ShowConfirmDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(confirmButtonWidth, 0.0F)))
        {
            m_ShowConfirmDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

Platform::ProcessTarget ProcessDetailsPanel::selectedTarget() const
{
    return Detail::targetForSelection(m_SelectedPid, m_HasSnapshot ? m_CachedSnapshot.get() : nullptr);
}

void ProcessDetailsPanel::dispatchConfirmedAction()
{
    // m_ProcessActions can be null: the injection constructor doesn't reject a null
    // unique_ptr (m_ActionCapabilities already handles that case), so guard here too rather
    // than dereferencing unconditionally.
    const Platform::ProcessActionResult result = m_ProcessActions
                                                   ? Detail::dispatchProcessAction(*m_ProcessActions, m_ConfirmAction, selectedTarget())
                                                   : Platform::ProcessActionResult::error("Process actions unavailable");
    m_LastActionResult = Detail::formatActionResultMessage(m_ConfirmAction, m_SelectedPid, result);
    m_ActionResultTimer = 5.0F;
}

void ProcessDetailsPanel::renderActionButtons()
{
    // Action buttons - use consistent sizing and 2x2 grid layout
    constexpr const char* TERMINATE_LABEL = ICON_FA_XMARK " Terminate";
    constexpr const char* KILL_LABEL = ICON_FA_SKULL " Kill";
    // "Suspend", not "Pause": the same word as the confirm dialog and the result line (#1203).
    constexpr const char* SUSPEND_LABEL = ICON_FA_PAUSE " Suspend";
    constexpr const char* RESUME_LABEL = ICON_FA_PLAY " Resume";

    // One width for all four, from the widest label and the font, capped to the pane (#949). See
    // ProcessDetailsLayout::computeActionButtonWidth() for why it is no longer a fixed 180px.
    const float emPx = ImGui::GetFontSize();
    const float gutter = ProcessDetailsLayout::ACTION_BUTTON_GUTTER_EM * emPx;
    const float widestLabel = std::max({ImGui::CalcTextSize(TERMINATE_LABEL).x,
                                        ImGui::CalcTextSize(KILL_LABEL).x,
                                        ImGui::CalcTextSize(SUSPEND_LABEL).x,
                                        ImGui::CalcTextSize(RESUME_LABEL).x});
    // Per-column overhead is the gutter plus one CellPadding.x, not two. This table has no inner
    // border, so ImGui does not pad inside each cell: it puts CellPadding.x on each side of the gap
    // *between* columns. Two columns have one gap, so the table is 2 * (width + gutter) plus
    // 2 * CellPadding.x in total -- one CellPadding.x per column.
    const float buttonWidth = ProcessDetailsLayout::computeActionButtonWidth(
        widestLabel, emPx, ImGui::GetContentRegionAvail().x, gutter + ImGui::GetStyle().CellPadding.x);
    constexpr float BUTTON_HEIGHT = 0.0F; // Use default height
    const ImVec2 buttonSize(buttonWidth, BUTTON_HEIGHT);

    // Use a table for consistent alignment
    if (ImGui::BeginTable("ActionButtons", 2, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Col1", ImGuiTableColumnFlags_WidthFixed, buttonWidth + gutter);
        ImGui::TableSetupColumn("Col2", ImGuiTableColumnFlags_WidthFixed, buttonWidth + gutter);

        // Row 1: Terminate and Kill
        ImGui::TableNextRow();

        // Terminate - graceful shutdown
        ImGui::TableNextColumn();
        if (m_ActionCapabilities.canTerminate)
        {
            if (ImGui::Button(TERMINATE_LABEL, buttonSize))
            {
                m_ConfirmAction = ProcessAction::Terminate;
                m_ShowConfirmDialog = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Ask the process to exit: it can save its work first, or refuse");
            }
        }

        // Kill - force terminate
        ImGui::TableNextColumn();
        if (m_ActionCapabilities.canKill)
        {
            if (ImGui::Button(KILL_LABEL, buttonSize))
            {
                m_ConfirmAction = ProcessAction::Kill;
                m_ShowConfirmDialog = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Force terminate (cannot be caught or ignored)");
            }
        }

        // Row 2: Stop and Resume
        ImGui::TableNextRow();

        // Suspend - stop the process running until it is resumed
        ImGui::TableNextColumn();
        if (m_ActionCapabilities.canStop)
        {
            if (ImGui::Button(SUSPEND_LABEL, buttonSize))
            {
                m_ConfirmAction = ProcessAction::Stop;
                m_ShowConfirmDialog = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Suspend the process until it is resumed");
            }
        }

        // Resume - continue a suspended process
        ImGui::TableNextColumn();
        if (m_ActionCapabilities.canContinue)
        {
            if (ImGui::Button(RESUME_LABEL, buttonSize))
            {
                m_ConfirmAction = ProcessAction::Resume;
                m_ShowConfirmDialog = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Resume a suspended process");
            }
        }

        ImGui::EndTable();
    }
}

// Renders the process-priority section: current nice value, the gradient slider (drawn via
// the drawPriority*/handlePrioritySliderInput helpers below), and the Apply button. No-op if
// the process actions probe doesn't support setting priority.
void ProcessDetailsPanel::renderPrioritySection()
{
    if (!m_ActionCapabilities.canSetPriority)
    {
        return;
    }

    const auto& theme = UI::Theme::get();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Spacing();

    const int currentNice = m_HasSnapshot ? cachedSnapshot().nice : 0;

    // Initialize the control from the current process nice value if not changed
    if (!m_PriorityChanged && m_HasSnapshot)
    {
        m_PriorityNiceValue = cachedSnapshot().nice;
    }

    const float emPx = ImGui::GetFontSize();
    // Where the priority control ends, for right-aligning the Apply button under it.
    float controlRightEdge = 0.0F;

#ifdef _WIN32
    // Windows has priority classes, not nice values (#1204): name the current class and offer the five
    // settable ones in a combo. Each writes its representative nice value through setPriority(), which
    // maps it back to that class; Realtime can only be shown.
    const std::string currentClassName{Detail::windowsPriorityClassName(Detail::windowsPriorityClassFromNice(currentNice))};
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GAUGE_HIGH "  Priority (current: %s)", currentClassName.c_str());
    ImGui::Spacing();

    const Detail::WindowsPriorityClass selectedClass = Detail::windowsPriorityClassFromNice(m_PriorityNiceValue);
    const std::string selectedClassName{Detail::windowsPriorityClassName(selectedClass)};
    const float comboWidth = std::min(Detail::PRIORITY_CLASS_COMBO_WIDTH_EM * emPx, std::max(ImGui::GetContentRegionAvail().x, 1.0F));
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##priority_class", selectedClassName.c_str()))
    {
        for (const Detail::WindowsPriorityClass priorityClass : Detail::SETTABLE_WINDOWS_PRIORITY_CLASSES)
        {
            const std::string optionName{Detail::windowsPriorityClassName(priorityClass)};
            const bool isSelected = priorityClass == selectedClass;
            if (ImGui::Selectable(optionName.c_str(), isSelected) && !isSelected)
            {
                m_PriorityNiceValue = Detail::windowsPriorityClassNice(priorityClass);
                m_PriorityChanged = true;
                m_PriorityError.clear();
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Windows priority class: higher classes get CPU time first.\n"
                          "Realtime cannot be set here.\n\n"
                          "Note: Changing another user's or an elevated process typically requires administrator privileges");
    }
    controlRightEdge = comboWidth;
    if (selectedClass == Detail::WindowsPriorityClass::Realtime)
    {
        ImGui::TextColored(theme.scheme().textWarning,
                           ICON_FA_TRIANGLE_EXCLAMATION "  Realtime was set outside TaskSmack; it can be lowered here, not set");
    }
#else
    // Show current nice value in the header
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GAUGE_HIGH "  Priority (current nice: %d)", currentNice);
    ImGui::Spacing();

    auto* drawList = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();

    // ========================================
    // Custom gradient priority slider (refactored into helper methods)
    // Layout: High [====gradient====] Low
    //                   Default
    // ========================================

    // Calculate "High" label width for offsetting the slider
    const float labelPadding = PRIORITY_LABEL_PADDING_EM * emPx;
    const ImVec2 highLabelSize = ImGui::CalcTextSize("High");
    const float highLabelOffset = highLabelSize.x + labelPadding;

    // The track gets whatever the panel has left once both labels and their padding are placed, so a
    // large font on a narrow panel shortens the track rather than pushing "Low" out of view.
    const float availableTrackWidth = ImGui::GetContentRegionAvail().x - highLabelOffset - labelPadding - ImGui::CalcTextSize("Low").x;

    // Build context for helper methods
    PrioritySliderContext ctx;
    ctx.drawList = drawList;
    ctx.niceValue = m_PriorityNiceValue;
    ctx.normalizedPos = getNicePosition(m_PriorityNiceValue);
    ctx.style = &style;
    ctx.priorityHighColor = theme.scheme().priorityHighColor;
    ctx.priorityNormalColor = theme.scheme().priorityNormalColor;
    ctx.priorityLowColor = theme.scheme().priorityLowColor;
    // A panel too narrow to leave any room is not "unconstrained": pass the smallest positive width
    // so the track shrinks to that instead of taking its full authored width and being clipped.
    ctx.metrics = Detail::computePrioritySliderMetrics(emPx, std::max(availableTrackWidth, 1.0F));
    const Detail::PrioritySliderMetrics& metrics = ctx.metrics;

    // Reserve space for badge above slider (offset by High label width)
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    ctx.cursorStart = ImVec2(rowStart.x + highLabelOffset, rowStart.y);
    ImGui::Dummy(ImVec2(highLabelOffset + metrics.sliderWidth, metrics.badgeHeight + metrics.badgeArrowSize));

    // Draw the value badge/callout above the slider position
    drawPriorityBadge(drawList, ctx);

    // Draw "High" label (left of slider, vertically centered with slider)
    // Note: 'theme' is already declared in the outer scope
    const float sliderRowY = ImGui::GetCursorPosY();
    const float labelCenterY = sliderRowY + ((metrics.sliderHeight - highLabelSize.y) * 0.5F);
    ImGui::SetCursorPosY(labelCenterY);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textError);
    ImGui::TextUnformatted("High");
    ImGui::PopStyleColor();

    // Position the slider after "High" label on same line
    ImGui::SameLine();
    ImGui::SetCursorPosY(sliderRowY);

    // Draw the gradient slider bar
    ctx.sliderMin = ImGui::GetCursorScreenPos();
    ctx.sliderMax = ImVec2(ctx.sliderMin.x + metrics.sliderWidth, ctx.sliderMin.y + metrics.sliderHeight);
    // Store window-local X coordinate for scale label positioning
    ctx.sliderLocalX = ctx.sliderMin.x - ImGui::GetWindowPos().x;

    // Draw gradient background (red -> green -> blue)
    drawPriorityGradient(drawList, ctx);

    // Draw slider border
    drawList->AddRect(ctx.sliderMin, ctx.sliderMax, ImGui::GetColorU32(ImGuiCol_Border), metrics.sliderCornerRadius);

    // Draw slider thumb/handle
    drawPriorityThumb(drawList, ctx);

    // Make the slider interactive with an invisible button
    ImGui::InvisibleButton("##priority_slider", ImVec2(metrics.sliderWidth, metrics.sliderHeight));
    handlePrioritySliderInput(ctx);

    // Draw "Low" label and "Default" label
    drawPriorityScaleLabels(ctx);

    // Tooltip on hover with keyboard shortcut hints
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Nice value: -20 (highest priority) to 19 (lowest priority)\n"
                          "Lower values = higher priority (more CPU time)\n"
                          "Normal priority = 0\n\n"
                          "Keyboard shortcuts:\n"
                          "  Left/Right: Adjust by 1\n"
                          "  PgUp/PgDown: Adjust by 5\n"
                          "  Home/End: Min/Max priority\n"
                          "  0: Reset to default\n\n"
                          "Note: Setting values below 0 typically requires root/admin privileges");
    }
    // The track starts after the "High" label, so the label offset belongs in the sum: without it the
    // Apply button stopped that far short of the track's right edge.
    controlRightEdge = highLabelOffset + metrics.sliderWidth;
#endif

    ImGui::Spacing();

    // ========================================
    // Action button (right-aligned)
    // ========================================
    const bool canApply = m_PriorityChanged && m_HasSnapshot;

    // Right-align the Apply button
    // Capped to the panel for the same reason the track is: the content area does not scroll
    // horizontally, so a button wider than the space available would be clipped.
    const float applyButtonWidth =
        std::min(UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize("Apply").x, emPx, PRIORITY_APPLY_BUTTON_MIN_EM),
                 std::max(ImGui::GetContentRegionAvail().x, 1.0F));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, controlRightEdge - applyButtonWidth));

    // Apply button with success (green) styling
    {
        if (!canApply)
        {
            ImGui::BeginDisabled();
        }
        // As for the Settings dialog's Apply: the label is drawn in whichever of the theme's text
        // colour and window background reads better on the fill showing in the current state (#969).
        if (UI::Widgets::filledButton("Apply",
                                      ImVec2(applyButtonWidth, 0.0F),
                                      {
                                          .resting = theme.scheme().successButton,
                                          .hovered = theme.scheme().successButtonHovered,
                                          .pressed = theme.scheme().successButtonActive,
                                      },
                                      theme.scheme().textPrimary,
                                      theme.scheme().windowBg))
        {
            const auto result = m_ProcessActions->setPriority(selectedTarget(), m_PriorityNiceValue);
            if (result.success)
            {
                m_PriorityError.clear(); // Clear any previous error
                m_PriorityChanged = false;
            }
            else
            {
                m_PriorityError = result.errorMessage; // Persistent error message
                // Revert slider to the actual process priority since the change failed
                m_PriorityNiceValue = cachedSnapshot().nice;
                m_PriorityChanged = false;
            }
        }
        if (!canApply)
        {
            ImGui::EndDisabled();
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Apply the selected priority to the process");
    }

    // Display persistent error message if priority change failed
    if (!m_PriorityError.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(theme.scheme().textError, ICON_FA_CIRCLE_EXCLAMATION "  %s", m_PriorityError.c_str());
    }
}

// =============================================================================
// Priority Slider Helper Methods
// =============================================================================

void ProcessDetailsPanel::drawPriorityBadge(ImDrawList* drawList, const PrioritySliderContext& ctx)
{
    const float badgeX = ctx.cursorStart.x + (ctx.normalizedPos * ctx.metrics.sliderWidth);
    const float badgeY = ctx.cursorStart.y;

    // Badge text
    const std::string valueText = std::to_string(ctx.niceValue);
    const ImVec2 textSize = ImGui::CalcTextSize(valueText.c_str());
    const float badgeWidth = textSize.x + (ctx.style->FramePadding.x * 2.0F);
    const float badgeHalfWidth = badgeWidth * 0.5F;

    // Keep the badge over the slider rather than hanging off either end
    const float clampedBadgeX = Detail::computeBadgeCenterX(badgeX, ctx.cursorStart.x, ctx.metrics.sliderWidth, badgeHalfWidth);

    // Badge rectangle
    const ImVec2 badgeMin(clampedBadgeX - badgeHalfWidth, badgeY);
    const ImVec2 badgeMax(clampedBadgeX + badgeHalfWidth, badgeY + ctx.metrics.badgeHeight);

    // Badge color based on nice value
    const ImU32 badgeColorU32 = getNiceColor(ctx.niceValue, ctx.priorityHighColor, ctx.priorityNormalColor, ctx.priorityLowColor);

    // Draw badge rectangle with rounded corners
    drawList->AddRectFilled(badgeMin, badgeMax, badgeColorU32, ctx.metrics.badgeCornerRadius);

    // Draw arrow pointing down from badge
    const ImVec2 arrowTip(badgeX, badgeMax.y + ctx.metrics.badgeArrowSize);
    const ImVec2 arrowLeft(badgeX - ctx.metrics.badgeArrowSize, badgeMax.y);
    const ImVec2 arrowRight(badgeX + ctx.metrics.badgeArrowSize, badgeMax.y);
    drawList->AddTriangleFilled(arrowLeft, arrowRight, arrowTip, badgeColorU32);

    // The theme's badge text colour when it reaches 4.5:1 on this badge's fill, else its window
    // background when that does, else black or white (badgeTextFor): a fixed colour was unreadable on
    // the nice-0 badge in most dark themes (#1130).
    const UI::ColorScheme& scheme = UI::Theme::get().scheme();
    const ImU32 badgeTextColorU32 = ImGui::ColorConvertFloat4ToU32(
        Detail::badgeTextFor(Detail::unpackColor(badgeColorU32), scheme.priorityBadgeTextColor, scheme.windowBg));

    // Draw badge text
    const ImVec2 textPos(clampedBadgeX - (textSize.x * 0.5F), badgeY + ((ctx.metrics.badgeHeight - textSize.y) * 0.5F));
    drawList->AddText(textPos, badgeTextColorU32, valueText.c_str());
}

void ProcessDetailsPanel::drawPriorityGradient(ImDrawList* drawList, const PrioritySliderContext& ctx)
{
    constexpr auto SEGMENTS = static_cast<int>(PRIORITY_GRADIENT_SEGMENTS);
    const float segmentWidth = ctx.metrics.sliderWidth / PRIORITY_GRADIENT_SEGMENTS;

    for (int i = 0; i < SEGMENTS; ++i)
    {
        const float t1 = static_cast<float>(i) / PRIORITY_GRADIENT_SEGMENTS;
        const float t2 = static_cast<float>(i + 1) / PRIORITY_GRADIENT_SEGMENTS;
        const int nice1 = NICE_MIN + static_cast<int>(t1 * static_cast<float>(NICE_RANGE));
        const int nice2 = NICE_MIN + static_cast<int>(t2 * static_cast<float>(NICE_RANGE));
        const ImU32 col1 = getNiceColor(nice1, ctx.priorityHighColor, ctx.priorityNormalColor, ctx.priorityLowColor);
        const ImU32 col2 = getNiceColor(nice2, ctx.priorityHighColor, ctx.priorityNormalColor, ctx.priorityLowColor);

        const ImVec2 segMin(ctx.sliderMin.x + (static_cast<float>(i) * segmentWidth), ctx.sliderMin.y);
        const ImVec2 segMax(ctx.sliderMin.x + (static_cast<float>(i + 1) * segmentWidth), ctx.sliderMax.y);

        drawList->AddRectFilledMultiColor(segMin, segMax, col1, col2, col2, col1);
    }
}

void ProcessDetailsPanel::drawPriorityThumb(ImDrawList* drawList, const PrioritySliderContext& ctx)
{
    const float thumbX = ctx.sliderMin.x + (ctx.normalizedPos * ctx.metrics.sliderWidth);
    const float thumbRadius = ctx.metrics.thumbRadius;
    const ImVec2 thumbCenter(thumbX, ctx.sliderMin.y + (ctx.metrics.sliderHeight * 0.5F));

    // The thumb sits on the track at the current nice value, which is the badge's fill, so it takes the
    // badge text's colour: readable there by construction rather than a fixed colour that vanished into
    // the light green middle of the track on dark themes (#1130).
    const UI::ColorScheme& scheme = UI::Theme::get().scheme();
    const ImU32 trackColorU32 = getNiceColor(ctx.niceValue, ctx.priorityHighColor, ctx.priorityNormalColor, ctx.priorityLowColor);
    const ImU32 thumbFillColorU32 = ImGui::ColorConvertFloat4ToU32(
        Detail::badgeTextFor(Detail::unpackColor(trackColorU32), scheme.priorityBadgeTextColor, scheme.windowBg));

    // Thumb outline
    drawList->AddCircleFilled(thumbCenter, thumbRadius + ctx.metrics.thumbOutlineThickness, ImGui::GetColorU32(ImGuiCol_Border));
    // Thumb fill
    drawList->AddCircleFilled(thumbCenter, thumbRadius, thumbFillColorU32);
}

void ProcessDetailsPanel::handlePrioritySliderInput(const PrioritySliderContext& ctx)
{
    // Mouse input: drag to set value
    if (ImGui::IsItemActive())
    {
        const float mouseX = ImGui::GetIO().MousePos.x;
        const float relX = std::clamp((mouseX - ctx.sliderMin.x) / ctx.metrics.sliderWidth, 0.0F, 1.0F);
        const int32_t newNice = getNiceFromPosition(relX);
        if (newNice != m_PriorityNiceValue)
        {
            m_PriorityNiceValue = newNice;
            m_PriorityChanged = true;
            // Clear any previous error when user interacts with slider
            // This provides fresher feedback rather than showing stale errors
            m_PriorityError.clear();
        }
    }

    // Keyboard input: adjust value when focused
    // Keys: Left/Right (±1), PgUp/PgDown (±5), Home/End (min/max), 0 (default)
    if (ImGui::IsItemFocused())
    {
        int32_t newNice = m_PriorityNiceValue;

        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
        {
            newNice = std::max(NICE_MIN, m_PriorityNiceValue - 1);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
        {
            newNice = std::min(NICE_MAX, m_PriorityNiceValue + 1);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
        {
            // Page Up = higher priority = lower nice value
            newNice = std::max(NICE_MIN, m_PriorityNiceValue - 5);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
        {
            // Page Down = lower priority = higher nice value
            newNice = std::min(NICE_MAX, m_PriorityNiceValue + 5);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Home))
        {
            newNice = NICE_MIN; // Highest priority (-20)
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_End))
        {
            newNice = NICE_MAX; // Lowest priority (19)
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0))
        {
            newNice = 0; // Default priority
        }

        if (newNice != m_PriorityNiceValue)
        {
            m_PriorityNiceValue = newNice;
            m_PriorityChanged = true;
            m_PriorityError.clear();
        }
    }
}

void ProcessDetailsPanel::drawPriorityScaleLabels(const PrioritySliderContext& ctx)
{
    const auto& theme = UI::Theme::get();

    // "Low" label (right of slider, colored blue)
    // Position it after the slider with padding (sliderLocalX + width = right edge)
    const float lowLabelX = ctx.sliderLocalX + ctx.metrics.sliderWidth + ctx.metrics.labelPadding;
    ImGui::SameLine();
    ImGui::SetCursorPosX(lowLabelX);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textInfo);
    ImGui::TextUnformatted("Low");
    ImGui::PopStyleColor();

    // "Default" label centered below the 0 position on the slider
    // Use getNicePosition(0) for consistency with other position calculations
    const float defaultX = ctx.sliderLocalX + (getNicePosition(0) * ctx.metrics.sliderWidth);
    const ImVec2 defaultSize = ImGui::CalcTextSize("Default");
    ImGui::SetCursorPosX(defaultX - (defaultSize.x * 0.5F));
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textMuted);
    ImGui::TextUnformatted("Default");
    ImGui::PopStyleColor();
}

} // namespace App
