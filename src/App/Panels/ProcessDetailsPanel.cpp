#include "ProcessDetailsPanel.h"

#include "App/Panel.h"
#include "App/ShellMetrics.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/History.h"
#include "Domain/Numeric.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/Factory.h"
#include "Platform/IProcessActions.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "ProcessDetailsPanel_GpuHelpers.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessDetailsPanel_ResourceHelpers.h"
#include "UI/ChartWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/EmptyState.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
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
#include <optional>
#include <span>
#include <string>
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

/// `bytes` as a percent of system RAM, using memoryPercentPerByte().
[[nodiscard]] double memoryBytesToPercent(std::uint64_t bytes, double percentPerByte)
{
    if (percentPerByte <= 0.0)
    {
        return 0.0;
    }
    return std::clamp(Domain::Numeric::toDouble(bytes) * percentPerByte, 0.0, 100.0);
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

void ProcessDetailsPanel::updateWithSnapshot(const Domain::ProcessSnapshot* snapshot, std::uint64_t snapshotVersion, float deltaTime)
{
    m_LastDeltaSeconds = deltaTime;

    // Fade out action result message
    if (m_ActionResultTimer > 0.0F)
    {
        m_ActionResultTimer -= deltaTime;
        if (m_ActionResultTimer <= 0.0F)
        {
            m_LastActionResult.clear();
        }
    }

    // A snapshot only counts if it is of the selected process itself, not of a different process
    // that has since been given its PID.
    const bool isSelectedProcess = (snapshot != nullptr) && ProcessDetailsLayout::snapshotIsSelectedProcess(
                                                                m_SelectedPid, m_SelectedUniqueKey, snapshot->pid, snapshot->uniqueKey);

    if (isSelectedProcess)
    {
        m_CachedSnapshot = *snapshot;
        m_HasSnapshot = true;
        m_ProcessExited = false;
        // Selected by PID alone: take the identity from the first snapshot, so that a later reuse
        // of the PID is still recognised as a different process.
        if (m_SelectedUniqueKey == 0)
        {
            m_SelectedUniqueKey = snapshot->uniqueKey;
        }

        updateSmoothedUsage(*snapshot, deltaTime);

        // Record history only when the background sampler publishes a new process generation.
        if (snapshotVersion != m_LastHistorySnapshotVersion)
        {
            m_LastHistorySnapshotVersion = snapshotVersion;
            const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            // Store as double to avoid narrowing; convert only at ImPlot boundary.
            m_CpuHistory.push_back(snapshot->cpuPercent);
            m_CpuUserHistory.push_back(snapshot->cpuUserPercent);
            m_CpuSystemHistory.push_back(snapshot->cpuSystemPercent);

            // Use the process RSS percent as a scale factor to express other metrics as percents for consistent charting.
            const double usedPercent = std::clamp(snapshot->memoryPercent, 0.0, 100.0);
            const double percentPerByte = memoryPercentPerByte(*snapshot);
            auto toPercent = [percentPerByte](std::uint64_t bytes) -> double
            {
                return memoryBytesToPercent(bytes, percentPerByte);
            };

            m_MemoryHistory.push_back(usedPercent);
            m_SharedHistory.push_back(toPercent(snapshot->sharedBytes));
            // Bytes, not a percent of RAM: a process's virtual size is usually larger than physical RAM,
            // so as a percent it was clamped to 100 and carried no information (#992).
            m_VirtualHistory.push_back(Domain::Numeric::toDouble(snapshot->virtualBytes));
            m_ThreadHistory.push_back(Domain::Numeric::toDouble(snapshot->threadCount));
            m_HandleHistory.push_back(Domain::Numeric::toDouble(snapshot->handleCount));
            m_PageFaultHistory.push_back(snapshot->pageFaultsPerSec);
            m_IoReadHistory.push_back(snapshot->ioReadBytesPerSec);
            m_IoWriteHistory.push_back(snapshot->ioWriteBytesPerSec);
            m_NetSentHistory.push_back(snapshot->netSentBytesPerSec);
            m_NetRecvHistory.push_back(snapshot->netReceivedBytesPerSec);
            m_PowerHistory.push_back(snapshot->powerWatts);
            m_GpuUtilHistory.push_back(snapshot->gpuUtilPercent);
            m_GpuMemHistory.push_back(Domain::Numeric::toDouble(snapshot->gpuMemoryBytes));
            m_GdiHistory.push_back(snapshot->gdiObjectCount.has_value()
                                       ? Domain::Numeric::toDouble(*snapshot->gdiObjectCount)
                                       // NaN signals "no data" to the plot; ImPlot renders NaN as a gap in the line.
                                       : std::numeric_limits<double>::quiet_NaN());
            m_Timestamps.push_back(nowSeconds);

            // Update peak memory percent (from snapshot's peak value)
            const double peakPercent = toPercent(snapshot->peakMemoryBytes);
            m_PeakMemoryPercent = std::max(m_PeakMemoryPercent, peakPercent);

            trimHistory(nowSeconds);
        }
    }
    else
    {
        // Selection changed or no selection
        if (m_SelectedPid == -1)
        {
            m_HasSnapshot = false;
        }
        else if (ProcessDetailsLayout::selectedProcessHasExited(true, m_HasSnapshot, isSelectedProcess))
        {
            // The cached snapshot is kept (the tab still names the process) but no longer drawn as
            // if it were live; renderContent() shows the exited state instead.
            m_ProcessExited = true;
        }
    }
}

void ProcessDetailsPanel::render(bool* open)
{
    std::string windowLabel;
    if (m_HasSnapshot && (m_SelectedPid != -1) && !m_CachedSnapshot.name.empty())
    {
        windowLabel = std::string(ICON_FA_CIRCLE_INFO) + " " + m_CachedSnapshot.name;
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
    if (m_HasSnapshot && (m_SelectedPid != -1) && !m_CachedSnapshot.name.empty())
    {
        return m_CachedSnapshot.name;
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

    // Skip rendering when tab is inactive (data collection continues in updateWithSnapshot)
    if (!m_IsActiveTab)
    {
        return;
    }

    if (m_ProcessExited)
    {
        // Replaces the whole pane, Actions tab included: nothing here may act on a PID that no
        // longer belongs to this process.
        const std::string detail = std::format(
            "{} (PID {}) is no longer running. Select another process in the Processes tab.", m_CachedSnapshot.name, m_SelectedPid);
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

    if (ImGui::BeginTabBar("DetailsTabs"))
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
                renderBasicInfo(m_CachedSnapshot);
                ImGui::Separator();
                renderResourceUsage(m_CachedSnapshot, fill);
                ImGui::Separator();
                // Only where the platform measures it: Windows does not, and used to chart a
                // fabricated figure (#1028).
                if (m_ProcessCapabilities.hasPowerUsage)
                {
                    renderPowerUsage(m_CachedSnapshot, fill);
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
                const auto& proc = m_CachedSnapshot;
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
                    renderGpuUsage(m_CachedSnapshot, fill);
                }
            }
            ImGui::EndTabItem();
        }

        // 3. Network and I/O - show if process has network or I/O data
        {
            const bool hasNetworkData = (m_CachedSnapshot.netSentBytesPerSec > 0.0 || m_CachedSnapshot.netReceivedBytesPerSec > 0.0 ||
                                         !m_NetSentHistory.empty() || !m_NetRecvHistory.empty());
            const bool hasIoData = (m_CachedSnapshot.ioReadBytesPerSec > 0.0 || m_CachedSnapshot.ioWriteBytesPerSec > 0.0 ||
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

    // Listen for history duration changes
    dispatcher.dispatch<Core::HistoryDurationChangedEvent>(
        [this](Core::HistoryDurationChangedEvent& e)
        {
            m_MaxHistorySeconds = Domain::Numeric::toDouble(e.getSeconds());
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
    m_LastHistorySnapshotVersion = 0;
    m_HasSnapshot = false;
    m_ProcessExited = false;
    m_ShowConfirmDialog = false;
    m_LastActionResult.clear();
    m_SmoothedUsage = {};
    m_PeakMemoryPercent = 0.0;
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
    const auto refreshMs = std::chrono::milliseconds(App::UserConfig::get().settings().refreshIntervalMs);
    const double alpha = computeAlpha(deltaTimeSeconds, refreshMs);

    const double targetCpu = UI::Format::clampPercent(snapshot.cpuPercent);
    const double targetResident = Domain::Numeric::toDouble(snapshot.memoryBytes);
    const double targetVirtual = Domain::Numeric::toDouble(std::max(snapshot.virtualBytes, snapshot.memoryBytes));
    const double targetCpuUser = UI::Format::clampPercent(snapshot.cpuUserPercent);
    const double targetCpuSystem = UI::Format::clampPercent(snapshot.cpuSystemPercent);
    const double targetThreads = Domain::Numeric::toDouble(snapshot.threadCount);
    const double targetHandles = Domain::Numeric::toDouble(snapshot.handleCount);
    const double targetFaults = std::max(0.0, snapshot.pageFaultsPerSec);
    const double targetIoRead = std::max(0.0, snapshot.ioReadBytesPerSec);
    const double targetIoWrite = std::max(0.0, snapshot.ioWriteBytesPerSec);
    const double targetNetSent = std::max(0.0, snapshot.netSentBytesPerSec);
    const double targetNetRecv = std::max(0.0, snapshot.netReceivedBytesPerSec);
    const double targetPower = std::max(0.0, snapshot.powerWatts);
    const double targetGpuUtil = UI::Format::clampPercent(snapshot.gpuUtilPercent);
    const double targetGpuMem = Domain::Numeric::toDouble(snapshot.gpuMemoryBytes);
    // Use 0 when GDI count is unavailable (nullopt) so the exponential smoother keeps a
    // neutral baseline rather than tracking stale data. The history chart uses NaN for
    // nullopt samples instead, so the two representations serve different purposes.
    const double targetGdiObjects = std::max(0.0, Domain::Numeric::toDouble(snapshot.gdiObjectCount.value_or(0)));
    // The Memory bars' percents, on the same RAM scale as the Memory chart's history.
    const double percentPerByte = memoryPercentPerByte(snapshot);
    const double targetMemUsedPercent = std::clamp(snapshot.memoryPercent, 0.0, 100.0);
    const double targetMemSharedPercent = memoryBytesToPercent(snapshot.sharedBytes, percentPerByte);

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
    m_SmoothedUsage.handleCount = std::max(0.0, initializeOrSmooth(m_SmoothedUsage.handleCount, targetHandles, alpha, initialized));
    m_SmoothedUsage.pageFaultsPerSec =
        std::max(0.0, initializeOrSmooth(m_SmoothedUsage.pageFaultsPerSec, targetFaults, alpha, initialized));
    m_SmoothedUsage.ioReadBytesPerSec =
        std::max(0.0, initializeOrSmooth(m_SmoothedUsage.ioReadBytesPerSec, targetIoRead, alpha, initialized));
    m_SmoothedUsage.ioWriteBytesPerSec =
        std::max(0.0, initializeOrSmooth(m_SmoothedUsage.ioWriteBytesPerSec, targetIoWrite, alpha, initialized));
    m_SmoothedUsage.netSentBytesPerSec =
        std::max(0.0, initializeOrSmooth(m_SmoothedUsage.netSentBytesPerSec, targetNetSent, alpha, initialized));
    m_SmoothedUsage.netRecvBytesPerSec =
        std::max(0.0, initializeOrSmooth(m_SmoothedUsage.netRecvBytesPerSec, targetNetRecv, alpha, initialized));
    m_SmoothedUsage.powerWatts = std::max(0.0, initializeOrSmooth(m_SmoothedUsage.powerWatts, targetPower, alpha, initialized));
    m_SmoothedUsage.gpuUtilPercent =
        UI::Format::clampPercent(initializeOrSmooth(m_SmoothedUsage.gpuUtilPercent, targetGpuUtil, alpha, initialized));
    m_SmoothedUsage.gpuMemoryBytes = std::max(0.0, initializeOrSmooth(m_SmoothedUsage.gpuMemoryBytes, targetGpuMem, alpha, initialized));
    m_SmoothedUsage.gdiObjectCount =
        std::max(0.0, initializeOrSmooth(m_SmoothedUsage.gdiObjectCount, targetGdiObjects, alpha, initialized));
    m_SmoothedUsage.memoryUsedPercent = initializeOrSmooth(m_SmoothedUsage.memoryUsedPercent, targetMemUsedPercent, alpha, initialized);
    m_SmoothedUsage.memorySharedPercent =
        initializeOrSmooth(m_SmoothedUsage.memorySharedPercent, targetMemSharedPercent, alpha, initialized);
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
            "Status",
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

    auto rightAlignedText = [](const std::string& text, const ImVec4& color)
    {
        const float colWidth = ImGui::GetColumnWidth();
        const float textWidth = ImGui::CalcTextSize(text.c_str()).x;
        const float padding = ImGui::GetStyle().CellPadding.x * 2.0F;
        const float targetX = ImGui::GetCursorPosX() + std::max(0.0F, colWidth - textWidth - padding);
        ImGui::SetCursorPosX(targetX);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
    };

    auto renderStatusValue = [&]() -> std::pair<std::string, ImVec4>
    {
        ImVec4 statusColor = theme.scheme().textInfo;
        if (proc.displayState == "Running")
        {
            statusColor = theme.scheme().statusRunning;
        }
        else if (proc.displayState == "Sleeping")
        {
            statusColor = theme.scheme().statusSleeping;
        }
        else if (proc.displayState == "Disk Sleep")
        {
            statusColor = theme.scheme().statusDiskSleep;
        }
        else if (proc.displayState == "Zombie")
        {
            statusColor = theme.scheme().statusZombie;
        }
        else if (proc.displayState == "Stopped" || proc.displayState == "Tracing")
        {
            statusColor = theme.scheme().statusStopped;
        }
        else if (proc.displayState == "Idle")
        {
            statusColor = theme.scheme().statusIdle;
        }

        return {proc.displayState, statusColor};
    };

    auto renderInfoTable = [&](const char* tableId, const std::vector<std::pair<std::string, std::pair<std::string, ImVec4>>>& rows)
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
                ImGui::TextUnformatted(row.first.c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                rightAlignedText(row.second.first, row.second.second);
            }

            ImGui::EndTable();
        }
    };

    auto formatCountLocale = [](std::int64_t value) -> std::string
    {
        return UI::Format::formatOrDash(value, [](auto v) { return UI::Format::formatIntLocalized(v); });
    };

#ifdef _WIN32
    constexpr const char* handleLabel = "Handles";
#else
    constexpr const char* handleLabel = "FDs";
#endif

    const auto [statusText, statusColor] = renderStatusValue();
    const std::string userText = proc.user.empty() ? "-" : proc.user;
    const std::string startedText =
        (proc.startTimeEpoch > 0) ? UI::Format::formatEpochDateTimeShort(proc.startTimeEpoch) : std::string("-");

    // Build identity rows (conditionally include Publisher if available)
    std::vector<std::pair<std::string, std::pair<std::string, ImVec4>>> identityRows = {
        {"Name", {proc.name, theme.scheme().textPrimary}},
        {"PID", {std::to_string(proc.pid), theme.scheme().textPrimary}},
        {"Parent PID", {std::to_string(proc.parentPid), theme.scheme().textPrimary}},
        {"User", {userText, theme.scheme().textPrimary}},
        {"Started", {startedText, theme.scheme().textMuted}},
    };
    if (!proc.publisher.empty())
    {
        identityRows.push_back({"Publisher", {proc.publisher, theme.scheme().textMuted}});
    }
    const auto identityRowCount = static_cast<float>(identityRows.size());
    const float leftHeight = (rowHeight * identityRowCount) + basePadding;

    // Build runtime rows (conditionally include Type if available)
    const std::string priorityText = std::format("{} (nice: {})", Domain::Priority::getPriorityLabel(proc.nice), proc.nice);
    std::vector<std::pair<std::string, std::pair<std::string, ImVec4>>> runtimeRows = {
        {"Status", {statusText, statusColor}},
        {"Threads", {proc.threadCount > 0 ? formatCountLocale(proc.threadCount) : std::string("-"), theme.scheme().textPrimary}},
        {handleLabel, {proc.handleCount > 0 ? formatCountLocale(proc.handleCount) : std::string("-"), theme.scheme().textPrimary}},
        {"CPU Time", {UI::Format::formatCpuTimeCompact(proc.cpuTimeSeconds), theme.scheme().textPrimary}},
        {"Priority", {priorityText, theme.scheme().textPrimary}},
    };
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
        runtimeRows.push_back({"Type", {proc.processType, typeColor}});
    }
    const auto runtimeRowCount = static_cast<float>(runtimeRows.size());
    const float rightHeight = (rowHeight * runtimeRowCount) + basePadding;

    // Each block is capped at a readable width instead of taking half the pane, so a label and its
    // value stay together however wide the window is (#925). The blocks pack to the left and the
    // remaining width is left empty.
    auto blockWidthFor = [&](const std::vector<std::pair<std::string, std::pair<std::string, ImVec4>>>& rows) -> float
    {
        float widestValue = 0.0F;
        for (const auto& row : rows)
        {
            widestValue = std::max(widestValue, ImGui::CalcTextSize(row.second.first.c_str()).x);
        }
        const ImGuiStyle& style = ImGui::GetStyle();
        const float contentNeeded = labelColWidth + widestValue + (style.CellPadding.x * 4.0F) + (style.WindowPadding.x * 2.0F);
        return ProcessDetailsLayout::computeInfoBlockWidth(ImGui::GetFontSize(), halfWidth, contentNeeded);
    };
    const float leftWidth = blockWidthFor(identityRows);
    const float rightWidth = blockWidthFor(runtimeRows);

    // Identity section: Who is this process?
    ImGui::BeginGroup();
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_ID_CARD "  Identity");
    ImGui::BeginChild("BasicInfoLeft", ImVec2(leftWidth, leftHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    renderInfoTable("BasicInfoLeftTable", identityRows);
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    // Runtime section: What is this process doing?
    ImGui::BeginGroup();
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CLOCK "  Runtime");
    ImGui::BeginChild("BasicInfoRight", ImVec2(rightWidth, rightHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);

    renderInfoTable("BasicInfoRightTable", runtimeRows);
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

        // Use smoothed values for NowBars for consistent animation
        const NowBar cpuTotalNow{.valueText = UI::Format::percentCompact(m_SmoothedUsage.cpuPercent),
                                 .label = CPU_TOTAL_LABEL,
                                 .tooltipText = {},
                                 .value01 = UI::Format::percent01(m_SmoothedUsage.cpuPercent),
                                 .color = theme.progressColor(m_SmoothedUsage.cpuPercent)};
        const NowBar cpuUserNow{.valueText = UI::Format::percentCompact(m_SmoothedUsage.cpuUserPercent),
                                .label = CPU_USER_LABEL,
                                .tooltipText = {},
                                .value01 = UI::Format::percent01(m_SmoothedUsage.cpuUserPercent),
                                .color = theme.scheme().cpuUser};
        const NowBar cpuSystemNow{.valueText = UI::Format::percentCompact(m_SmoothedUsage.cpuSystemPercent),
                                  .label = CPU_SYSTEM_LABEL,
                                  .tooltipText = {},
                                  .value01 = UI::Format::percent01(m_SmoothedUsage.cpuSystemPercent),
                                  .color = theme.scheme().cpuSystem};

        auto cpuPlot = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::withHeight(
                UI::Widgets::percentHistoryConfig("##ProcOverviewCPU", axisConfig.xMin, axisConfig.xMax), fill.plotHeight()));
            if (chart.active())
            {
                UI::Widgets::drawCollectingHint(alignedCount);
                // alignedCount > 0 here: the section only renders with history (see above).

                // Reuse member scratch buffers across frames instead of local vectors, so the
                // per-frame history redraw doesn't reallocate once buffers reach steady-state size.
                m_CpuStackY0.assign(alignedCount, 0.0);
                m_CpuStackYUser.resize(alignedCount);
                m_CpuStackYSystem.resize(alignedCount);
                auto& y0 = m_CpuStackY0;
                auto& yUserTop = m_CpuStackYUser;
                auto& ySystemTop = m_CpuStackYSystem;

                // PlotShaded fills the area *between* two Y series, so a stacked user/system
                // area chart needs cumulative tops: user alone, then user+system on top of it.
                for (size_t i = 0; i < alignedCount; ++i)
                {
                    yUserTop[i] = cpuUserData[i];
                    ySystemTop[i] = cpuUserData[i] + cpuSystemData[i];
                }

                // Bands and lines reach "now" like every plotLineWithFill series: the last sample
                // held to x = 0 (UI::Widgets::holdLastValueToNow, #1016). Copies, so the tooltip's
                // lookup over cpuTimeData still finds real samples only.
                m_CpuPlotX.assign(cpuTimeData.begin(), cpuTimeData.end());
                m_CpuPlotTotal.assign(cpuData.begin(), cpuData.end());
                m_CpuPlotUser.assign(cpuUserData.begin(), cpuUserData.end());
                m_CpuPlotSystem.assign(cpuSystemData.begin(), cpuSystemData.end());
                // These bands and lines are drawn with ImPlot directly, so they are capped here like
                // every plotLineWithFill series (#1022). Reduced together, so the bands still line
                // up with each other and with the lines. Points are chosen by each drawn value --
                // User (also the user band's top), System and Total -- not by the cumulative system
                // top, which stays flat when System rises as User falls and would drop that spike.
                UI::Widgets::reduceAlignedSeries(m_CpuPlotX,
                                                 {&m_CpuPlotUser, &m_CpuPlotSystem, &m_CpuPlotTotal},
                                                 {&yUserTop, &ySystemTop},
                                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE,
                                                 nowSeconds);
                y0.assign(m_CpuPlotX.size(), 0.0);
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
                ImPlot::PlotShaded(CPU_USER_LABEL,
                                   m_CpuPlotX.data(),
                                   y0.data(),
                                   yUserTop.data(),
                                   drawCount,
                                   {ImPlotProp_FillColor, theme.scheme().cpuUserFill});

                ImPlot::PlotShaded(CPU_SYSTEM_LABEL,
                                   m_CpuPlotX.data(),
                                   yUserTop.data(),
                                   ySystemTop.data(),
                                   drawCount,
                                   {ImPlotProp_FillColor, theme.scheme().cpuSystemFill});

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
                                                        .value = UI::Format::percentCompact(cpuData[*idxVal])},
                                UI::Widgets::TooltipRow{.label = CPU_USER_LABEL,
                                                        .color = theme.scheme().cpuUser,
                                                        .value = UI::Format::percentCompact(cpuUserData[*idxVal])},
                                UI::Widgets::TooltipRow{.label = CPU_SYSTEM_LABEL,
                                                        .color = theme.scheme().cpuSystem,
                                                        .value = UI::Format::percentCompact(cpuSystemData[*idxVal])},
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
            const double usedNow = m_SmoothedUsage.memoryUsedPercent;
            const double sharedNow = m_SmoothedUsage.memorySharedPercent;
            // Virtual size in bytes on its own right-hand axis (#992), eased like a rate axis and shared
            // with its bar.
            const double virtAxisUpper = UI::Widgets::easedRateAxisUpperBound(
                "##ProcOverviewMemory/Y2", UI::Widgets::maxOfSeries(virtData), UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);

            std::vector<NowBar> memoryBars;
            // No tooltipText: the hover tooltip is "label: value" (selectNowBarTooltip), in the
            // chart's own units -- percents of RAM, and bytes for Virtual.
            memoryBars.push_back({.valueText = UI::Format::percentCompact(usedNow),
                                  .label = MEM_USED_LABEL,
                                  .tooltipText = {},
                                  .value01 = UI::Format::percent01(usedNow),
                                  .color = theme.scheme().chartMemory});
            if (showShared)
            {
                memoryBars.push_back({
                    .valueText = UI::Format::percentCompact(sharedNow),
                    .label = MEM_SHARED_LABEL,
                    .tooltipText = {},
                    .value01 = UI::Format::percent01(sharedNow),
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
                const UI::Widgets::HistoryChart chart(
                    UI::Widgets::withHeight(UI::Widgets::withHorizontalLegend(UI::Widgets::percentHistoryConfig(
                                                "##ProcOverviewMemory", axisConfig.xMin, axisConfig.xMax)),
                                            fill.plotHeight()));
                if (chart.active())
                {
                    UI::Widgets::setupSecondaryRateAxis(virtAxisUpper, UI::Widgets::formatAxisBytes);
                    UI::Widgets::drawCollectingHint(alignedCount);
                    // Draw peak working set as a horizontal reference line (never decreases)
                    if (m_PeakMemoryPercent > 0.0)
                    {
                        // Draw horizontal line at peak value across the entire X range
                        const double peakY = m_PeakMemoryPercent;
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
                                                .value = UI::Format::percentCompact(usedData[*idxVal])});
                            }
                            if (*idxVal < sharedData.size())
                            {
                                rows.push_back({.label = MEM_SHARED_LABEL,
                                                .color = theme.scheme().chartCpu,
                                                .value = UI::Format::percentCompact(sharedData[*idxVal])});
                            }
                            if (*idxVal < virtData.size())
                            {
                                rows.push_back({.label = MEM_VIRTUAL_LABEL,
                                                .color = theme.scheme().chartIo,
                                                .value = UI::Format::formatBytes(virtData[*idxVal])});
                            }
                            if (m_PeakMemoryPercent > 0.0)
                            {
                                // The line's colour: this row was textWarning, which matched nothing (#1005).
                                rows.push_back({.label = MEM_PEAK_LABEL,
                                                .color = theme.scheme().chartPeakLine,
                                                .value = UI::Format::percentCompact(m_PeakMemoryPercent)});
                            }
                            UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                        }
                    }
                }
            };

            ImGui::Spacing();
            ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MEMORY "  Memory (%zu samples)", alignedCount);
            renderHistoryWithNowBars(
                "ProcessMemoryOverviewLayout", fill.plotHeight(), memoryPlot, memoryBars, false, PROCESS_NOW_BAR_COLUMNS);
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
    const double countSeriesMax = std::max(UI::Widgets::maxOfSeries(threadData, handleData), UI::Widgets::maxOfSeries(gdiData));
#else
    const double countSeriesMax = UI::Widgets::maxOfSeries(threadData, handleData);
#endif
    const double countAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcThreadsFaults", countSeriesMax, UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);
    const double faultAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcThreadsFaults/Y2", UI::Widgets::maxOfSeries(faultData), UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);

    const NowBar threadsBar{.valueText = UI::Format::formatCountWithLabel(std::llround(m_SmoothedUsage.threadCount), "threads"),
                            .label = THREADS_LABEL,
                            .tooltipText = UI::Widgets::formatTooltipRow(
                                THREADS_LABEL, UI::Format::formatIntLocalized(std::llround(m_SmoothedUsage.threadCount))),
                            .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.threadCount, countAxisUpper),
                            .color = theme.scheme().chartCpu};

#ifdef _WIN32
    constexpr const char* handleLabel = "Handles";
#else
    constexpr const char* handleLabel = "FDs";
#endif

    const NowBar handlesBar{
        .valueText = UI::Format::formatCountWithLabel(std::llround(m_SmoothedUsage.handleCount), handleLabel),
        .label = handleLabel,
        .tooltipText = std::format("{}: {}", handleLabel, UI::Format::formatIntLocalized(std::llround(m_SmoothedUsage.handleCount))),
        .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.handleCount, countAxisUpper),
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
        .valueText =
            hasGdiSamples ? UI::Format::formatCountWithLabel(std::llround(m_SmoothedUsage.gdiObjectCount), "GDI") : std::string("N/A"),
        .label = GDI_LABEL,
        .tooltipText = hasGdiSamples ? UI::Widgets::formatTooltipRow(
                                           GDI_LABEL, UI::Format::formatIntLocalized(std::llround(m_SmoothedUsage.gdiObjectCount)))
                                     : UI::Widgets::formatTooltipRow(GDI_LABEL, "N/A"),
        .value01 = hasGdiSamples ? UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.gdiObjectCount, countAxisUpper) : 0.0,
        .color = theme.accentColor(4)};
#endif

    auto plot = [&]()
    {
        // One legend row: up to four short entries (with GDI on Windows) on a chart that shares the
        // pane's height (see HistoryChartConfig::legendHorizontal).
        const UI::Widgets::HistoryChart chart(
            UI::Widgets::withHeight(UI::Widgets::withHorizontalLegend(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcThreadsFaults", axisConfig.xMin, axisConfig.xMax, formatAxisLocalized, countAxisUpper)),
                                    fill.plotHeight()));
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
                            {.label = THREADS_LABEL,
                             .color = theme.scheme().chartCpu,
                             .value = UI::Format::formatIntLocalized(std::llround(threadData[*idxVal]))},
                            {.label = handleLabel,
                             .color = theme.scheme().chartMemory,
                             .value = UI::Format::formatIntLocalized(std::llround(handleData[*idxVal]))},
                            {.label = FAULTS_LABEL,
                             .color = theme.accentColor(3),
                             .value = UI::Format::formatCountPerSecond(faultData[*idxVal])},
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
    const double ioAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcIoHistory", UI::Widgets::maxOfSeries(readData, writeData), UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    const auto readUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.ioReadBytesPerSec);
    const auto writeUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.ioWriteBytesPerSec);

    const NowBar readBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.ioReadBytesPerSec, readUnit),
                         .label = IO_READ_LABEL,
                         .tooltipText = {},
                         .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.ioReadBytesPerSec, ioAxisUpper),
                         .color = theme.scheme().chartIo};

    const NowBar writeBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.ioWriteBytesPerSec, writeUnit),
                          .label = IO_WRITE_LABEL,
                          .tooltipText = {},
                          .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.ioWriteBytesPerSec, ioAxisUpper),
                          .color = theme.scheme().chartIoWrite};

    // Keep the plot and its hover tooltip together: both consume the same aligned
    // vectors, and the lambda is rendered alongside the matching NowBars below.
    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcIoHistory", axisConfig.xMin, axisConfig.xMax, formatAxisBytesPerSec, ioAxisUpper),
                                    fill.plotHeight()));
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
                                                    .value = UI::Format::formatBytesPerSec(readData[*idxVal])},
                            UI::Widgets::TooltipRow{.label = IO_WRITE_LABEL,
                                                    .color = theme.scheme().chartIoWrite,
                                                    .value = UI::Format::formatBytesPerSec(writeData[*idxVal])},
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
    const double netAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcNetworkHistory", UI::Widgets::maxOfSeries(sentData, recvData), UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    const auto sentUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.netSentBytesPerSec);
    const auto recvUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.netRecvBytesPerSec);

    const NowBar sentBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.netSentBytesPerSec, sentUnit),
                         .label = NET_SENT_LABEL,
                         .tooltipText = {},
                         .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.netSentBytesPerSec, netAxisUpper),
                         .color = theme.scheme().chartNetTx};

    const NowBar recvBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.netRecvBytesPerSec, recvUnit),
                         .label = NET_RECV_LABEL,
                         .tooltipText = {},
                         .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.netRecvBytesPerSec, netAxisUpper),
                         .color = theme.scheme().chartNetRx};

    // The plot lambda owns rendering and hover lookup over the same aligned
    // buffers; renderHistoryWithNowBars composes it with the summary bars.
    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcNetworkHistory", axisConfig.xMin, axisConfig.xMax, formatAxisBytesPerSec, netAxisUpper),
                                    fill.plotHeight()));
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
                                                    .value = UI::Format::formatBytesPerSec(sentData[*idxVal])},
                            UI::Widgets::TooltipRow{.label = NET_RECV_LABEL,
                                                    .color = theme.scheme().chartNetRx,
                                                    .value = UI::Format::formatBytesPerSec(recvData[*idxVal])},
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
        "##ProcPowerHistory", UI::Widgets::maxOfSeries(powerData), UI::Widgets::RATE_AXIS_MIN_SPAN_WATTS);

    const NowBar powerBar{.valueText = UI::Format::formatPowerOrZero(m_SmoothedUsage.powerWatts),
                          .label = POWER_LABEL,
                          .tooltipText = {},
                          .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedUsage.powerWatts, powerAxisUpper),
                          .color = theme.scheme().textInfo};

    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcPowerHistory", axisConfig.xMin, axisConfig.xMax, formatAxisWatts, powerAxisUpper),
                                    fill.plotHeight()));
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
                            const std::array rows{UI::Widgets::TooltipRow{.label = POWER_LABEL,
                                                                          .color = theme.scheme().textInfo,
                                                                          .value = UI::Format::formatPowerOrZero(powerData[*idxVal])}};
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
    ImGui::Separator();
    ImGui::Spacing();

    renderPerGpuBreakdown(proc);

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
// proc.perGpuUsage. No-op if that list is empty, regardless of how many GPUs the system has.
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
            const UI::Widgets::HistoryChart chart(UI::Widgets::withHeight(
                UI::Widgets::percentHistoryConfig("##GPUUtilPlot", axisConfig.xMin, axisConfig.xMax), fill.plotHeight()));
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

        // GPU Memory graph. One upper bound for its axis and its bar, so they agree (#1003).
        const double gpuMemAxisUpper = UI::Widgets::easedRateAxisUpperBound(
            "##GPUMemPlot", UI::Widgets::maxOfSeries(gpuMemVec), UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);
        auto plotGpuMem = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::withHeight(
                UI::Widgets::rateHistoryConfigWithUpper(
                    "##GPUMemPlot", axisConfig.xMin, axisConfig.xMax, UI::Widgets::formatAxisBytes, gpuMemAxisUpper),
                fill.plotHeight()));
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
                                const std::array rows{UI::Widgets::TooltipRow{.label = GPU_MEMORY_LABEL,
                                                                              .color = theme.scheme().gpuMemory,
                                                                              .value = UI::Format::formatBytes(gpuMemVec[*idxVal])}};
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
            .value01 = m_SmoothedUsage.gpuUtilPercent / 100.0,
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

    ImGui::Text("%s (PID %d)", m_CachedSnapshot.name.c_str(), m_SelectedPid);
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
    const bool isError = m_LastActionResult.contains("Error") || m_LastActionResult.contains("Failed");
    const ImVec4 color = isError ? theme.scheme().textError : theme.scheme().textSuccess;
    ImGui::TextColored(color, "%s", m_LastActionResult.c_str());
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderConfirmDialog()
{
    if (m_ShowConfirmDialog)
    {
        ImGui::OpenPopup("Confirm Action");
    }

    if (ImGui::BeginPopupModal("Confirm Action", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // The dialog auto-fits, so it is bounded here: neither the question (which carries the
        // process name) nor the button row may be wider than the main window can show. See
        // ProcessDetailsLayout::computeConfirmContentBudget().
        const ImGuiStyle& confirmStyle = ImGui::GetStyle();
        const float contentBudget = ProcessDetailsLayout::computeConfirmContentBudget(
            ImGui::GetMainViewport()->WorkSize.x, UI::DialogMetrics::MAX_VIEWPORT_FRACTION, confirmStyle.WindowPadding.x);

        const std::string question = std::format("Are you sure you want to {} process '{}' (PID {})?",
                                                 Detail::actionVerb(m_ConfirmAction),
                                                 m_CachedSnapshot.name,
                                                 m_SelectedPid);
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
        const float confirmButtonWidth = ProcessDetailsLayout::computeConfirmButtonWidth(
            UI::DialogMetrics::computeActionButtonWidth(
                std::max(ImGui::CalcTextSize("Yes").x, ImGui::CalcTextSize("No").x), ImGui::GetFontSize(), CONFIRM_BUTTON_MIN_EM),
            contentBudget,
            confirmStyle.ItemSpacing.x);

        if (ImGui::Button("Yes", ImVec2(confirmButtonWidth, 0.0F)))
        {
            dispatchConfirmedAction();
            m_ShowConfirmDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("No", ImVec2(confirmButtonWidth, 0.0F)))
        {
            m_ShowConfirmDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

Platform::ProcessTarget ProcessDetailsPanel::selectedTarget() const
{
    return Detail::targetForSelection(m_SelectedPid, m_HasSnapshot ? &m_CachedSnapshot : nullptr);
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
    constexpr const char* PAUSE_LABEL = ICON_FA_PAUSE " Pause";
    constexpr const char* RESUME_LABEL = ICON_FA_PLAY " Resume";

    // One width for all four, from the widest label and the font, capped to the pane (#949). See
    // ProcessDetailsLayout::computeActionButtonWidth() for why it is no longer a fixed 180px.
    const float emPx = ImGui::GetFontSize();
    const float gutter = ProcessDetailsLayout::ACTION_BUTTON_GUTTER_EM * emPx;
    const float widestLabel = std::max({ImGui::CalcTextSize(TERMINATE_LABEL).x,
                                        ImGui::CalcTextSize(KILL_LABEL).x,
                                        ImGui::CalcTextSize(PAUSE_LABEL).x,
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
                ImGui::SetTooltip("Request graceful shutdown");
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

        // Pause - suspend the process
        ImGui::TableNextColumn();
        if (m_ActionCapabilities.canStop)
        {
            if (ImGui::Button(PAUSE_LABEL, buttonSize))
            {
                m_ConfirmAction = ProcessAction::Stop;
                m_ShowConfirmDialog = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Pause the process");
            }
        }

        // Resume - continue a paused process
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
                ImGui::SetTooltip("Resume a paused process");
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

    // Show current nice value in the header
    const int currentNice = m_HasSnapshot ? m_CachedSnapshot.nice : 0;
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GAUGE_HIGH "  Priority (current nice: %d)", currentNice);
    ImGui::Spacing();

    // Initialize slider from current process nice value if not changed
    if (!m_PriorityChanged && m_HasSnapshot)
    {
        m_PriorityNiceValue = m_CachedSnapshot.nice;
    }

    auto* drawList = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();

    // ========================================
    // Custom gradient priority slider (refactored into helper methods)
    // Layout: High [====gradient====] Low
    //                   Default
    // ========================================

    // Calculate "High" label width for offsetting the slider
    const float emPx = ImGui::GetFontSize();
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
    // The track starts after the "High" label, so the label offset belongs in the sum: without it the
    // button stopped that far short of the track's right edge.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, highLabelOffset + metrics.sliderWidth - applyButtonWidth));

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
                m_PriorityNiceValue = m_CachedSnapshot.nice;
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

    // Cache the badge text color as U32 once per call (avoids repeated theme lookup and conversion)
    const ImU32 badgeTextColorU32 = ImGui::ColorConvertFloat4ToU32(UI::Theme::get().scheme().priorityBadgeTextColor);

    // Draw badge text using the theme-specified badge text color (white on dark themes, near-black on light)
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

    // Cache the badge text color as U32 once per call (avoids repeated theme lookup and conversion)
    const ImU32 thumbFillColorU32 = ImGui::ColorConvertFloat4ToU32(UI::Theme::get().scheme().priorityBadgeTextColor);

    // Thumb outline
    drawList->AddCircleFilled(thumbCenter, thumbRadius + ctx.metrics.thumbOutlineThickness, ImGui::GetColorU32(ImGuiCol_Border));
    // Thumb fill: uses the badge text color (white on dark, near-black on light) for matching contrast
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
