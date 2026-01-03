#include "ProcessDetailsPanel.h"

#include "App/Panel.h"
#include "App/UserConfig.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/Factory.h"
#include "Platform/IProcessActions.h"
#include "UI/Format.h"
#include "UI/HistoryWidgets.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Numeric.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{

using UI::Widgets::buildTimeAxisDoubles;
using UI::Widgets::computeAlpha;
using UI::Widgets::formatAgeSeconds;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::formatAxisLocalized;
using UI::Widgets::formatAxisWatts;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::setupLegendDefault;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

constexpr size_t PROCESS_NOW_BAR_COLUMNS = 3;

template<typename T> [[nodiscard]] auto tailVector(const std::deque<T>& data, std::size_t count) -> std::vector<T>
{
    count = std::min(count, data.size());

    std::vector<T> out;
    out.reserve(count);

    const std::size_t start = data.size() - count;
    for (std::size_t i = start; i < data.size(); ++i)
    {
        out.push_back(data[i]);
    }

    return out;
}

[[nodiscard]] auto seriesMax(const std::vector<double>& values, double current) -> double
{
    if (values.empty())
    {
        return current;
    }

    const double historyMax = *std::ranges::max_element(values);
    return std::max(current, historyMax);
}

// ImPlot series counts are int; keep conversion explicit + checked.

} // namespace

namespace App
{

ProcessDetailsPanel::ProcessDetailsPanel()
    : Panel("Process Details"),
      m_MaxHistorySeconds(UI::Numeric::toDouble(App::UserConfig::get().settings().maxHistorySeconds)),
      m_ProcessActions(Platform::makeProcessActions()),
      m_ActionCapabilities(m_ProcessActions->actionCapabilities())
{
}

void ProcessDetailsPanel::updateWithSnapshot(const Domain::ProcessSnapshot* snapshot, float deltaTime)
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

    if (snapshot != nullptr && snapshot->pid == m_SelectedPid)
    {
        m_CachedSnapshot = *snapshot;
        m_HasSnapshot = true;

        updateSmoothedUsage(*snapshot, deltaTime);

        // Sample history at fixed interval
        m_HistoryTimer += deltaTime;
        if (m_HistoryTimer >= HISTORY_SAMPLE_INTERVAL)
        {
            m_HistoryTimer = 0.0F;
            const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            // Store as double to avoid narrowing; convert only at ImPlot boundary.
            m_CpuHistory.push_back(snapshot->cpuPercent);
            m_CpuUserHistory.push_back(snapshot->cpuUserPercent);
            m_CpuSystemHistory.push_back(snapshot->cpuSystemPercent);

            // Use the process RSS percent as a scale factor to express other metrics as percents for consistent charting.
            const double usedPercent = std::clamp(snapshot->memoryPercent, 0.0, 100.0);
            double scale = 0.0;
            if (usedPercent > 0.0 && snapshot->memoryBytes > 0)
            {
                // memoryPercent = (memoryBytes / totalSystemMemoryBytes) * 100
                // => X% of system = X * (memoryPercent / memoryBytes)
                scale = usedPercent / UI::Numeric::toDouble(snapshot->memoryBytes);
            }

            auto toPercent = [scale](std::uint64_t bytes) -> double
            {
                if (scale <= 0.0)
                {
                    return 0.0;
                }
                return std::clamp(UI::Numeric::toDouble(bytes) * scale, 0.0, 100.0);
            };

            m_MemoryHistory.push_back(usedPercent);
            m_SharedHistory.push_back(toPercent(snapshot->sharedBytes));
            m_VirtualHistory.push_back(toPercent(snapshot->virtualBytes));
            m_ThreadHistory.push_back(UI::Numeric::toDouble(snapshot->threadCount));
            m_PageFaultHistory.push_back(snapshot->pageFaultsPerSec);
            m_IoReadHistory.push_back(snapshot->ioReadBytesPerSec);
            m_IoWriteHistory.push_back(snapshot->ioWriteBytesPerSec);
            m_NetSentHistory.push_back(snapshot->netSentBytesPerSec);
            m_NetRecvHistory.push_back(snapshot->netReceivedBytesPerSec);
            m_PowerHistory.push_back(snapshot->powerWatts);
            m_GpuUtilHistory.push_back(snapshot->gpuUtilPercent);
            m_GpuMemHistory.push_back(UI::Numeric::toDouble(snapshot->gpuMemoryBytes));
            m_Timestamps.push_back(nowSeconds);

            // Update peak memory percent (from snapshot's peak value)
            const double peakPercent = toPercent(snapshot->peakMemoryBytes);
            m_PeakMemoryPercent = std::max(m_PeakMemoryPercent, peakPercent);

            trimHistory(nowSeconds);
        }
    }
    else if (snapshot == nullptr || snapshot->pid != m_SelectedPid)
    {
        // Selection changed or no selection
        if (m_SelectedPid == -1)
        {
            m_HasSnapshot = false;
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

std::string ProcessDetailsPanel::tabLabel() const
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
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textMuted, "Select a process from the Processes panel to view details");
        return;
    }

    if (!m_HasSnapshot)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textWarning, "Process %d not found (may have exited)", m_SelectedPid);
        return;
    }

    // Tabs for different info sections
    // Add padding inside tabs for better spacing
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0F, 8.0F));

    if (ImGui::BeginTabBar("DetailsTabs"))
    {
        if (ImGui::BeginTabItem(ICON_FA_CIRCLE_INFO "  Overview"))
        {
            renderBasicInfo(m_CachedSnapshot);
            ImGui::Separator();
            renderResourceUsage(m_CachedSnapshot);
            ImGui::Separator();
            renderPowerUsage(m_CachedSnapshot);
            ImGui::Separator();
            renderThreadAndFaultHistory(m_CachedSnapshot);
            ImGui::Separator();
            renderIoStats(m_CachedSnapshot);
            ImGui::Separator();
            renderNetworkStats(m_CachedSnapshot);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(ICON_FA_GEARS "  Actions"))
        {
            renderActions();
            ImGui::EndTabItem();
        }

        // GPU tab - show if process has GPU usage
        if (m_CachedSnapshot.gpuUtilPercent > 0.0 || m_CachedSnapshot.gpuMemoryBytes > 0 || !m_CachedSnapshot.gpuDevices.empty())
        {
            if (ImGui::BeginTabItem(ICON_FA_MICROCHIP "  GPU"))
            {
                renderGpuUsage(m_CachedSnapshot);
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    ImGui::PopStyleVar(); // FramePadding
}

void ProcessDetailsPanel::setSelectedPid(std::int32_t pid)
{
    if (pid != m_SelectedPid)
    {
        m_SelectedPid = pid;
        m_CpuHistory.clear();
        m_CpuUserHistory.clear();
        m_CpuSystemHistory.clear();
        m_MemoryHistory.clear();
        m_SharedHistory.clear();
        m_VirtualHistory.clear();
        m_ThreadHistory.clear();
        m_PageFaultHistory.clear();
        m_IoReadHistory.clear();
        m_IoWriteHistory.clear();
        m_NetSentHistory.clear();
        m_NetRecvHistory.clear();
        m_PowerHistory.clear();
        m_GpuUtilHistory.clear();
        m_GpuMemHistory.clear();
        m_Timestamps.clear();
        m_HistoryTimer = 0.0F;
        m_HasSnapshot = false;
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
}

void ProcessDetailsPanel::updateSmoothedUsage(const Domain::ProcessSnapshot& snapshot, float deltaTimeSeconds)
{
    const auto refreshMs = std::chrono::milliseconds(App::UserConfig::get().settings().refreshIntervalMs);
    const double alpha = computeAlpha(deltaTimeSeconds, refreshMs);

    const double targetCpu = UI::Numeric::clampPercent(snapshot.cpuPercent);
    const double targetResident = UI::Numeric::toDouble(snapshot.memoryBytes);
    const double targetVirtual = UI::Numeric::toDouble(std::max(snapshot.virtualBytes, snapshot.memoryBytes));
    const double targetCpuUser = UI::Numeric::clampPercent(snapshot.cpuUserPercent);
    const double targetCpuSystem = UI::Numeric::clampPercent(snapshot.cpuSystemPercent);
    const double targetThreads = UI::Numeric::toDouble(snapshot.threadCount);
    const double targetFaults = std::max(0.0, snapshot.pageFaultsPerSec);
    const double targetIoRead = std::max(0.0, snapshot.ioReadBytesPerSec);
    const double targetIoWrite = std::max(0.0, snapshot.ioWriteBytesPerSec);
    const double targetNetSent = std::max(0.0, snapshot.netSentBytesPerSec);
    const double targetNetRecv = std::max(0.0, snapshot.netReceivedBytesPerSec);
    const double targetPower = std::max(0.0, snapshot.powerWatts);
    const double targetGpuUtil = UI::Numeric::clampPercent(snapshot.gpuUtilPercent);
    const double targetGpuMem = UI::Numeric::toDouble(snapshot.gpuMemoryBytes);

    if (!m_SmoothedUsage.initialized || deltaTimeSeconds <= 0.0F)
    {
        m_SmoothedUsage.cpuPercent = targetCpu;
        m_SmoothedUsage.residentBytes = targetResident;
        m_SmoothedUsage.virtualBytes = targetVirtual;
        m_SmoothedUsage.cpuUserPercent = targetCpuUser;
        m_SmoothedUsage.cpuSystemPercent = targetCpuSystem;
        m_SmoothedUsage.threadCount = targetThreads;
        m_SmoothedUsage.pageFaultsPerSec = targetFaults;
        m_SmoothedUsage.ioReadBytesPerSec = targetIoRead;
        m_SmoothedUsage.ioWriteBytesPerSec = targetIoWrite;
        m_SmoothedUsage.netSentBytesPerSec = targetNetSent;
        m_SmoothedUsage.netRecvBytesPerSec = targetNetRecv;
        m_SmoothedUsage.powerWatts = targetPower;
        m_SmoothedUsage.gpuUtilPercent = targetGpuUtil;
        m_SmoothedUsage.gpuMemoryBytes = targetGpuMem;
        m_SmoothedUsage.initialized = true;
        return;
    }

    m_SmoothedUsage.cpuPercent = UI::Numeric::clampPercent(smoothTowards(m_SmoothedUsage.cpuPercent, targetCpu, alpha));
    m_SmoothedUsage.residentBytes = std::max(0.0, smoothTowards(m_SmoothedUsage.residentBytes, targetResident, alpha));
    m_SmoothedUsage.virtualBytes = smoothTowards(m_SmoothedUsage.virtualBytes, targetVirtual, alpha);
    m_SmoothedUsage.virtualBytes = std::max(m_SmoothedUsage.virtualBytes, m_SmoothedUsage.residentBytes);
    m_SmoothedUsage.cpuUserPercent = UI::Numeric::clampPercent(smoothTowards(m_SmoothedUsage.cpuUserPercent, targetCpuUser, alpha));
    m_SmoothedUsage.cpuSystemPercent = UI::Numeric::clampPercent(smoothTowards(m_SmoothedUsage.cpuSystemPercent, targetCpuSystem, alpha));
    m_SmoothedUsage.threadCount = std::max(0.0, smoothTowards(m_SmoothedUsage.threadCount, targetThreads, alpha));
    m_SmoothedUsage.pageFaultsPerSec = std::max(0.0, smoothTowards(m_SmoothedUsage.pageFaultsPerSec, targetFaults, alpha));
    m_SmoothedUsage.ioReadBytesPerSec = std::max(0.0, smoothTowards(m_SmoothedUsage.ioReadBytesPerSec, targetIoRead, alpha));
    m_SmoothedUsage.ioWriteBytesPerSec = std::max(0.0, smoothTowards(m_SmoothedUsage.ioWriteBytesPerSec, targetIoWrite, alpha));
    m_SmoothedUsage.netSentBytesPerSec = std::max(0.0, smoothTowards(m_SmoothedUsage.netSentBytesPerSec, targetNetSent, alpha));
    m_SmoothedUsage.netRecvBytesPerSec = std::max(0.0, smoothTowards(m_SmoothedUsage.netRecvBytesPerSec, targetNetRecv, alpha));
    m_SmoothedUsage.powerWatts = std::max(0.0, smoothTowards(m_SmoothedUsage.powerWatts, targetPower, alpha));
    m_SmoothedUsage.gpuUtilPercent = UI::Numeric::clampPercent(smoothTowards(m_SmoothedUsage.gpuUtilPercent, targetGpuUtil, alpha));
    m_SmoothedUsage.gpuMemoryBytes = std::max(0.0, smoothTowards(m_SmoothedUsage.gpuMemoryBytes, targetGpuMem, alpha));
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
        constexpr std::array<const char*, 10> labels = {
            "PID",
            "Parent",
            "Name",
            "Status",
            "User",
            "Threads",
            "Nice",
            "CPU Time",
            "Page Faults",
            "Affinity",
        };

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
    const float leftHeight = (rowHeight * 5.0F) + basePadding;  // Identity rows
    const float rightHeight = (rowHeight * 5.0F) + basePadding; // Runtime rows

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

    auto formatPageFaults = [&]() -> std::string
    {
        return UI::Format::formatOrDash(proc.pageFaults, [](auto value) { return UI::Format::formatIntLocalized(value); });
    };

    auto formatCountLocale = [](std::int64_t value) -> std::string
    {
        return UI::Format::formatOrDash(value, [](auto v) { return UI::Format::formatIntLocalized(v); });
    };

    const auto [statusText, statusColor] = renderStatusValue();
    const std::string userText = proc.user.empty() ? "-" : proc.user;
    const std::string affinityText = UI::Format::formatCpuAffinityMask(proc.cpuAffinityMask);

    ImGui::BeginGroup();
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_ID_CARD "  Identity");
    ImGui::BeginChild("BasicInfoLeft", ImVec2(halfWidth, leftHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    renderInfoTable("BasicInfoLeftTable",
                    {
                        {"Name", {proc.name, theme.scheme().textPrimary}},
                        {"PID", {std::to_string(proc.pid), theme.scheme().textPrimary}},
                        {"Parent", {std::to_string(proc.parentPid), theme.scheme().textPrimary}},
                        {"Status", {statusText, statusColor}},
                        {"User", {userText, theme.scheme().textPrimary}},
                    });
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CLOCK "  Runtime");
    ImGui::BeginChild("BasicInfoRight", ImVec2(halfWidth, rightHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    renderInfoTable(
        "BasicInfoRightTable",
        {
            {"Threads", {proc.threadCount > 0 ? formatCountLocale(proc.threadCount) : std::string("-"), theme.scheme().textPrimary}},
            {"Nice", {std::to_string(proc.nice), theme.scheme().textPrimary}},
            {"CPU Time", {UI::Format::formatCpuTimeCompact(proc.cpuTimeSeconds), theme.scheme().textPrimary}},
            {"Page Faults", {formatPageFaults(), theme.scheme().textPrimary}},
            {"Affinity", {affinityText, theme.scheme().textPrimary}},
        });
    ImGui::EndChild();
    ImGui::EndGroup();
}

void ProcessDetailsPanel::renderResourceUsage(const Domain::ProcessSnapshot& proc)
{
    const auto& theme = UI::Theme::get();

    // Ensure smoothing is initialized even if render is called before an update tick
    if (!m_SmoothedUsage.initialized)
    {
        updateSmoothedUsage(proc, m_LastDeltaSeconds);
    }

    // Inline CPU history with paired now bar
    if (!m_Timestamps.empty() && !m_CpuHistory.empty())
    {
        const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const size_t alignedCount =
            std::min({m_Timestamps.size(), m_CpuHistory.size(), m_CpuUserHistory.size(), m_CpuSystemHistory.size()});

        const std::vector<double> timestamps = tailVector(m_Timestamps, alignedCount);
        std::vector<double> cpuData = tailVector(m_CpuHistory, alignedCount);
        std::vector<double> cpuUserData = tailVector(m_CpuUserHistory, alignedCount);
        std::vector<double> cpuSystemData = tailVector(m_CpuSystemHistory, alignedCount);

        const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
        std::vector<double> cpuTimeData = buildTimeAxisDoubles(timestamps, alignedCount, nowSeconds);

        // Use smoothed values for NowBars for consistent animation
        const NowBar cpuTotalNow{.valueText = UI::Format::percentCompact(m_SmoothedUsage.cpuPercent),
                                 .label = "CPU Total",
                                 .value01 = UI::Numeric::percent01(m_SmoothedUsage.cpuPercent),
                                 .color = theme.progressColor(m_SmoothedUsage.cpuPercent)};
        const NowBar cpuUserNow{.valueText = UI::Format::percentCompact(m_SmoothedUsage.cpuUserPercent),
                                .label = "User",
                                .value01 = UI::Numeric::percent01(m_SmoothedUsage.cpuUserPercent),
                                .color = theme.scheme().cpuUser};
        const NowBar cpuSystemNow{.valueText = UI::Format::percentCompact(m_SmoothedUsage.cpuSystemPercent),
                                  .label = "System",
                                  .value01 = UI::Numeric::percent01(m_SmoothedUsage.cpuSystemPercent),
                                  .color = theme.scheme().cpuSystem};

        auto cpuPlot = [&]()
        {
            const UI::Widgets::PlotFontGuard fontGuard;
            if (ImPlot::BeginPlot("##ProcOverviewCPU", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
            {
                setupLegendDefault();
                ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                ImPlot::SetupAxisFormat(ImAxis_Y1, UI::Widgets::formatAxisPercent);
                ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

                if (alignedCount > 0)
                {
                    const int plotCount = UI::Numeric::checkedCount(alignedCount);
                    std::vector<double> y0(alignedCount, 0.0);
                    std::vector<double> yUserTop(alignedCount);
                    std::vector<double> ySystemTop(alignedCount);

                    for (size_t i = 0; i < alignedCount; ++i)
                    {
                        yUserTop[i] = cpuUserData[i];
                        ySystemTop[i] = cpuUserData[i] + cpuSystemData[i];
                    }

                    ImPlot::SetNextFillStyle(theme.scheme().cpuUserFill);
                    ImPlot::PlotShaded("##CpuUser", cpuTimeData.data(), y0.data(), yUserTop.data(), plotCount);

                    ImPlot::SetNextFillStyle(theme.scheme().cpuSystemFill);
                    ImPlot::PlotShaded("##CpuSystem", cpuTimeData.data(), yUserTop.data(), ySystemTop.data(), plotCount);

                    ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                    ImPlot::PlotLine("Total", cpuTimeData.data(), cpuData.data(), plotCount);

                    ImPlot::SetNextLineStyle(theme.scheme().cpuUser, 1.8F);
                    ImPlot::PlotLine("User", cpuTimeData.data(), cpuUserData.data(), plotCount);

                    ImPlot::SetNextLineStyle(theme.scheme().cpuSystem, 1.8F);
                    ImPlot::PlotLine("System", cpuTimeData.data(), cpuSystemData.data(), plotCount);

                    if (ImPlot::IsPlotHovered())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        if (const auto idxVal = hoveredIndexFromPlotX(cpuTimeData, mouse.x))
                        {
                            if (*idxVal < alignedCount)
                            {
                                ImGui::BeginTooltip();
                                const auto ageText = formatAgeSeconds(cpuTimeData[*idxVal]);
                                ImGui::TextUnformatted(ageText.c_str());
                                ImGui::Separator();
                                const double totalValue = cpuData[*idxVal];
                                const ImVec4 totalColor = theme.progressColor(totalValue);
                                ImGui::TextColored(totalColor, "Total: %s", UI::Format::percentCompact(totalValue).c_str());
                                ImGui::TextColored(
                                    theme.scheme().cpuUser, "User: %s", UI::Format::percentCompact(cpuUserData[*idxVal]).c_str());
                                ImGui::TextColored(
                                    theme.scheme().cpuSystem, "System: %s", UI::Format::percentCompact(cpuSystemData[*idxVal]).c_str());
                                ImGui::EndTooltip();
                            }
                        }
                    }
                }
                else
                {
                    ImPlot::PlotDummy("CPU");
                }

                ImPlot::EndPlot();
            }
        };

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MICROCHIP "  CPU (%zu samples)", alignedCount);
        renderHistoryWithNowBars("ProcessCPUHistoryOverview",
                                 HISTORY_PLOT_HEIGHT_DEFAULT,
                                 cpuPlot,
                                 {cpuTotalNow, cpuUserNow, cpuSystemNow},
                                 false,
                                 PROCESS_NOW_BAR_COLUMNS);
        ImGui::Spacing();
    }

    // Inline history for memory (overview) mirroring system memory chart layout
    if (!m_Timestamps.empty())
    {
        const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const size_t alignedCount =
            std::min({m_Timestamps.size(), m_MemoryHistory.size(), m_SharedHistory.size(), m_VirtualHistory.size()});

        if (alignedCount > 0)
        {
            const std::vector<double> timestamps = tailVector(m_Timestamps, alignedCount);
            std::vector<double> usedData = tailVector(m_MemoryHistory, alignedCount);
            std::vector<double> sharedData = tailVector(m_SharedHistory, alignedCount);
            std::vector<double> virtData = tailVector(m_VirtualHistory, alignedCount);

            const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
            std::vector<double> timeData = buildTimeAxisDoubles(timestamps, alignedCount, nowSeconds);

            const double usedNow = usedData.empty() ? 0.0 : usedData.back();
            const double sharedNow = sharedData.empty() ? 0.0 : sharedData.back();
            const double virtNowVal = virtData.empty() ? 0.0 : virtData.back();

            std::vector<NowBar> memoryBars;
            memoryBars.push_back({.valueText = UI::Format::percentCompact(usedNow),
                                  .label = "Memory Used",
                                  .value01 = UI::Numeric::percent01(usedNow),
                                  .color = theme.scheme().chartMemory});
            memoryBars.push_back({.valueText = UI::Format::percentCompact(sharedNow),
                                  .label = "Shared",
                                  .value01 = UI::Numeric::percent01(sharedNow),
                                  .color = theme.scheme().chartCpu});
            memoryBars.push_back({.valueText = UI::Format::percentCompact(virtNowVal),
                                  .label = "Virtual",
                                  .value01 = UI::Numeric::percent01(virtNowVal),
                                  .color = theme.scheme().chartIo});

            auto memoryPlot = [&]()
            {
                const UI::Widgets::PlotFontGuard fontGuard;
                if (ImPlot::BeginPlot("##ProcOverviewMemory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
                {
                    setupLegendDefault();
                    ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                    ImPlot::SetupAxisFormat(ImAxis_Y1, UI::Widgets::formatAxisPercent);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

                    // Draw peak working set as a horizontal reference line (never decreases)
                    if (m_PeakMemoryPercent > 0.0)
                    {
                        // Use a dashed line style with a distinct color for the peak
                        ImPlot::SetNextLineStyle(theme.scheme().chartPeakLine, 1.5F);

                        // Draw horizontal line at peak value across the entire X range
                        const double peakY = m_PeakMemoryPercent;
                        std::array<double, 2> peakX = {axisConfig.xMin, axisConfig.xMax};
                        std::array<double, 2> peakYVals = {peakY, peakY};
                        ImPlot::PlotLine("Peak", peakX.data(), peakYVals.data(), 2);
                    }

                    if (!usedData.empty())
                    {
                        plotLineWithFill("Used",
                                         timeData.data(),
                                         usedData.data(),
                                         UI::Numeric::checkedCount(usedData.size()),
                                         theme.scheme().chartMemory,
                                         theme.scheme().chartMemoryFill);
                    }

                    if (!sharedData.empty())
                    {
                        plotLineWithFill("Shared",
                                         timeData.data(),
                                         sharedData.data(),
                                         UI::Numeric::checkedCount(sharedData.size()),
                                         theme.scheme().chartCpu,
                                         theme.scheme().chartCpuFill);
                    }

                    if (!virtData.empty())
                    {
                        plotLineWithFill("Virtual",
                                         timeData.data(),
                                         virtData.data(),
                                         UI::Numeric::checkedCount(virtData.size()),
                                         theme.scheme().chartIo,
                                         theme.scheme().chartIoFill);
                    }

                    if (ImPlot::IsPlotHovered())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                        {
                            ImGui::BeginTooltip();
                            const auto ageText = formatAgeSeconds(timeData[*idxVal]);
                            ImGui::TextUnformatted(ageText.c_str());
                            if (*idxVal < usedData.size())
                            {
                                ImGui::TextColored(
                                    theme.scheme().chartMemory, "Used: %s", UI::Format::percentCompact(usedData[*idxVal]).c_str());
                            }
                            if (*idxVal < sharedData.size())
                            {
                                ImGui::TextColored(
                                    theme.scheme().chartCpu, "Shared: %s", UI::Format::percentCompact(sharedData[*idxVal]).c_str());
                            }
                            if (*idxVal < virtData.size())
                            {
                                ImGui::TextColored(
                                    theme.scheme().chartIo, "Virtual: %s", UI::Format::percentCompact(virtData[*idxVal]).c_str());
                            }
                            if (m_PeakMemoryPercent > 0.0)
                            {
                                ImGui::TextColored(
                                    theme.scheme().textWarning, "Peak: %s", UI::Format::percentCompact(m_PeakMemoryPercent).c_str());
                            }
                            ImGui::EndTooltip();
                        }
                    }

                    ImPlot::EndPlot();
                }
            };

            ImGui::Spacing();
            ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MEMORY "  Memory (%zu samples)", alignedCount);
            renderHistoryWithNowBars(
                "ProcessMemoryOverviewLayout", HISTORY_PLOT_HEIGHT_DEFAULT, memoryPlot, memoryBars, false, PROCESS_NOW_BAR_COLUMNS);
            ImGui::Spacing();
        }
    }
}

void ProcessDetailsPanel::renderThreadAndFaultHistory([[maybe_unused]] const Domain::ProcessSnapshot& proc)
{
    if (m_Timestamps.empty() || (m_ThreadHistory.empty() && m_PageFaultHistory.empty()))
    {
        return;
    }

    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const size_t alignedCount = std::min({m_Timestamps.size(), m_ThreadHistory.size(), m_PageFaultHistory.size()});
    if (alignedCount == 0)
    {
        return;
    }

    const auto& theme = UI::Theme::get();

    const std::vector<double> timestamps = tailVector(m_Timestamps, alignedCount);
    std::vector<double> threadData = tailVector(m_ThreadHistory, alignedCount);
    std::vector<double> faultData = tailVector(m_PageFaultHistory, alignedCount);

    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    std::vector<double> timeData = buildTimeAxisDoubles(timestamps, alignedCount, nowSeconds);

    // Use smoothed values for NowBars
    const double threadMax = seriesMax(threadData, m_SmoothedUsage.threadCount);
    const double faultMax = seriesMax(faultData, m_SmoothedUsage.pageFaultsPerSec);

    const NowBar threadsBar{.valueText = UI::Format::formatCountWithLabel(std::llround(m_SmoothedUsage.threadCount), "threads"),
                            .label = "Threads",
                            .value01 = (threadMax > 0.0) ? std::clamp(m_SmoothedUsage.threadCount / threadMax, 0.0, 1.0) : 0.0,
                            .color = theme.scheme().chartCpu};

    const NowBar faultsBar{.valueText = UI::Format::formatCountPerSecond(m_SmoothedUsage.pageFaultsPerSec),
                           .label = "Page Faults",
                           .value01 = (faultMax > 0.0) ? std::clamp(m_SmoothedUsage.pageFaultsPerSec / faultMax, 0.0, 1.0) : 0.0,
                           .color = theme.scheme().chartIo};

    auto plot = [&]()
    {
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot("##ProcThreadsFaults", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
        {
            setupLegendDefault();
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisLocalized);
            ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

            const int plotCount = UI::Numeric::checkedCount(alignedCount);
            plotLineWithFill(
                "Threads", timeData.data(), threadData.data(), plotCount, theme.scheme().chartCpu, theme.scheme().chartCpuFill);

            plotLineWithFill("Page Faults/s", timeData.data(), faultData.data(), plotCount, theme.accentColor(3));

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    if (*idxVal < alignedCount)
                    {
                        ImGui::BeginTooltip();
                        const auto ageText = formatAgeSeconds(timeData[*idxVal]);
                        ImGui::TextUnformatted(ageText.c_str());
                        ImGui::Separator();
                        ImGui::TextColored(theme.scheme().chartCpu,
                                           "Threads: %s",
                                           UI::Format::formatIntLocalized(std::llround(threadData[*idxVal])).c_str());
                        ImGui::TextColored(
                            theme.accentColor(3), "Page Faults: %s", UI::Format::formatCountPerSecond(faultData[*idxVal]).c_str());
                        ImGui::EndTooltip();
                    }
                }
            }

            ImPlot::EndPlot();
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GEARS "  Threads & Page Faults (%zu samples)", alignedCount);
    renderHistoryWithNowBars(
        "ProcessThreadFaultHistory", HISTORY_PLOT_HEIGHT_DEFAULT, plot, {threadsBar, faultsBar}, false, PROCESS_NOW_BAR_COLUMNS);
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderIoStats(const Domain::ProcessSnapshot& proc)
{
    const bool hasCurrent = (proc.ioReadBytesPerSec > 0.0 || proc.ioWriteBytesPerSec > 0.0);
    if (m_Timestamps.empty() && !hasCurrent)
    {
        return;
    }

    const size_t alignedCount = std::min({m_Timestamps.size(), m_IoReadHistory.size(), m_IoWriteHistory.size()});
    if (alignedCount == 0)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    const std::vector<double> timestamps = tailVector(m_Timestamps, alignedCount);
    std::vector<double> readData = tailVector(m_IoReadHistory, alignedCount);
    std::vector<double> writeData = tailVector(m_IoWriteHistory, alignedCount);

    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    std::vector<double> timeData = buildTimeAxisDoubles(timestamps, alignedCount, nowSeconds);

    // Use smoothed values for NowBars
    const double readMax = seriesMax(readData, m_SmoothedUsage.ioReadBytesPerSec);
    const double writeMax = seriesMax(writeData, m_SmoothedUsage.ioWriteBytesPerSec);

    const auto readUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.ioReadBytesPerSec);
    const auto writeUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.ioWriteBytesPerSec);

    const NowBar readBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.ioReadBytesPerSec, readUnit),
                         .label = "Disk Read",
                         .value01 = (readMax > 0.0) ? std::clamp(m_SmoothedUsage.ioReadBytesPerSec / readMax, 0.0, 1.0) : 0.0,
                         .color = theme.scheme().chartIo};

    const NowBar writeBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.ioWriteBytesPerSec, writeUnit),
                          .label = "Disk Write",
                          .value01 = (writeMax > 0.0) ? std::clamp(m_SmoothedUsage.ioWriteBytesPerSec / writeMax, 0.0, 1.0) : 0.0,
                          .color = theme.accentColor(1)};

    auto plot = [&]()
    {
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot("##ProcIoHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
        {
            setupLegendDefault();
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytesPerSec);
            ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

            const int plotCount = UI::Numeric::checkedCount(alignedCount);
            plotLineWithFill("Read", timeData.data(), readData.data(), plotCount, theme.scheme().chartIo, theme.scheme().chartIoFill);

            plotLineWithFill("Write", timeData.data(), writeData.data(), plotCount, theme.accentColor(1));

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    if (*idxVal < alignedCount)
                    {
                        ImGui::BeginTooltip();
                        const auto ageText = formatAgeSeconds(timeData[*idxVal]);
                        ImGui::TextUnformatted(ageText.c_str());
                        ImGui::TextColored(theme.scheme().chartIo, "Read: %s", UI::Format::formatBytesPerSec(readData[*idxVal]).c_str());
                        ImGui::TextColored(theme.accentColor(1), "Write: %s", UI::Format::formatBytesPerSec(writeData[*idxVal]).c_str());
                        ImGui::EndTooltip();
                    }
                }
            }

            ImPlot::EndPlot();
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_HARD_DRIVE "  I/O Statistics (%zu samples)", alignedCount);
    renderHistoryWithNowBars("ProcessIoHistory", HISTORY_PLOT_HEIGHT_DEFAULT, plot, {readBar, writeBar}, false, PROCESS_NOW_BAR_COLUMNS);
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderNetworkStats(const Domain::ProcessSnapshot& proc)
{
    const bool hasCurrent = (proc.netSentBytesPerSec > 0.0 || proc.netReceivedBytesPerSec > 0.0);
    if (m_Timestamps.empty() && !hasCurrent)
    {
        return;
    }

    const size_t alignedCount = std::min({m_Timestamps.size(), m_NetSentHistory.size(), m_NetRecvHistory.size()});
    if (alignedCount == 0)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    const std::vector<double> timestamps = tailVector(m_Timestamps, alignedCount);
    std::vector<double> sentData = tailVector(m_NetSentHistory, alignedCount);
    std::vector<double> recvData = tailVector(m_NetRecvHistory, alignedCount);

    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    std::vector<double> timeData = buildTimeAxisDoubles(timestamps, alignedCount, nowSeconds);

    // Use smoothed values for NowBars
    const double sentMax = seriesMax(sentData, m_SmoothedUsage.netSentBytesPerSec);
    const double recvMax = seriesMax(recvData, m_SmoothedUsage.netRecvBytesPerSec);

    const auto sentUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.netSentBytesPerSec);
    const auto recvUnit = UI::Format::unitForBytesPerSecond(m_SmoothedUsage.netRecvBytesPerSec);

    const NowBar sentBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.netSentBytesPerSec, sentUnit),
                         .label = "Network Sent",
                         .value01 = (sentMax > 0.0) ? std::clamp(m_SmoothedUsage.netSentBytesPerSec / sentMax, 0.0, 1.0) : 0.0,
                         .color = theme.scheme().chartCpu};

    const NowBar recvBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.netRecvBytesPerSec, recvUnit),
                         .label = "Network Received",
                         .value01 = (recvMax > 0.0) ? std::clamp(m_SmoothedUsage.netRecvBytesPerSec / recvMax, 0.0, 1.0) : 0.0,
                         .color = theme.accentColor(2)};

    auto plot = [&]()
    {
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot("##ProcNetworkHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
        {
            setupLegendDefault();
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytesPerSec);
            ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

            const int plotCount = UI::Numeric::checkedCount(alignedCount);
            plotLineWithFill("Sent", timeData.data(), sentData.data(), plotCount, theme.scheme().chartCpu, theme.scheme().chartCpuFill);

            plotLineWithFill("Received", timeData.data(), recvData.data(), plotCount, theme.accentColor(2));

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    if (*idxVal < alignedCount)
                    {
                        ImGui::BeginTooltip();
                        const auto ageText = formatAgeSeconds(timeData[*idxVal]);
                        ImGui::TextUnformatted(ageText.c_str());
                        ImGui::TextColored(
                            theme.scheme().chartCpu, "Avg Sent: %s", UI::Format::formatBytesPerSec(sentData[*idxVal]).c_str());
                        ImGui::TextColored(theme.accentColor(2), "Avg Recv: %s", UI::Format::formatBytesPerSec(recvData[*idxVal]).c_str());
                        ImGui::EndTooltip();
                    }
                }
            }

            ImPlot::EndPlot();
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_NETWORK_WIRED "  Network - Avg Rate (%zu samples)", alignedCount);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Average network bytes/sec since monitoring started for this process.");
    }
    renderHistoryWithNowBars(
        "ProcessNetworkHistory", HISTORY_PLOT_HEIGHT_DEFAULT, plot, {sentBar, recvBar}, false, PROCESS_NOW_BAR_COLUMNS);
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderPowerUsage(const Domain::ProcessSnapshot& proc)
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
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<double> powerData = tailVector(m_PowerHistory, alignedCount);
    const std::vector<double> timestamps = tailVector(m_Timestamps, alignedCount);
    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
    std::vector<double> timeData = buildTimeAxisDoubles(timestamps, alignedCount, nowSeconds);

    // Use smoothed value for NowBar
    const double powerMax = seriesMax(powerData, m_SmoothedUsage.powerWatts);

    const NowBar powerBar{.valueText = UI::Format::formatPowerCompact(m_SmoothedUsage.powerWatts),
                          .label = "Power Usage",
                          .value01 = (powerMax > 0.0) ? std::clamp(m_SmoothedUsage.powerWatts / powerMax, 0.0, 1.0) : 0.0,
                          .color = theme.scheme().textInfo};

    auto plot = [&]()
    {
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot("##ProcPowerHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
        {
            setupLegendDefault();
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisWatts);
            ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

            if (!powerData.empty())
            {
                plotLineWithFill(
                    "Power", timeData.data(), powerData.data(), UI::Numeric::checkedCount(powerData.size()), theme.scheme().textInfo);

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        if (*idxVal < powerData.size())
                        {
                            ImGui::BeginTooltip();
                            const auto ageText = formatAgeSeconds(timeData[*idxVal]);
                            ImGui::TextUnformatted(ageText.c_str());
                            ImGui::TextColored(
                                theme.scheme().textInfo, "Power: %s", UI::Format::formatPowerCompact(powerData[*idxVal]).c_str());
                            ImGui::EndTooltip();
                        }
                    }
                }
            }
            else
            {
                ImPlot::PlotDummy("Power");
            }

            ImPlot::EndPlot();
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_BOLT "  Power Usage (%zu samples)", alignedCount);
    renderHistoryWithNowBars("ProcessPowerHistory", HISTORY_PLOT_HEIGHT_DEFAULT, plot, {powerBar}, false, PROCESS_NOW_BAR_COLUMNS);
    ImGui::Spacing();
}

void ProcessDetailsPanel::renderGpuUsage(const Domain::ProcessSnapshot& proc)
{
    auto& theme = UI::Theme::get();

    // Show GPU info
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MICROCHIP "  GPU Usage");
    ImGui::Spacing();

    // Current GPU metrics
    if (ImGui::BeginTable("GPUCurrentMetrics", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 150.0F);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        // GPU Utilization
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("GPU Utilization:");
        ImGui::TableNextColumn();
        const ImVec4 gpuUtilColor = theme.scheme().gpuUtilization;
        ImGui::TextColored(gpuUtilColor, "%.1f%%", m_SmoothedUsage.gpuUtilPercent);

        // GPU Memory
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("GPU Memory:");
        ImGui::TableNextColumn();
        const ImVec4 gpuMemColor = theme.scheme().gpuMemory;
        const std::string memStr = UI::Format::formatBytes(m_SmoothedUsage.gpuMemoryBytes);
        ImGui::TextColored(gpuMemColor, "%s", memStr.c_str());

        // GPU Device(s)
        if (!proc.gpuDevices.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("GPU Device(s):");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(proc.gpuDevices.c_str());
        }

        // GPU Engines
        if (!proc.gpuEngines.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("Active Engines:");
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
            ImGui::Text("Video Encoder:");
            ImGui::TableNextColumn();
            const ImVec4 encColor = theme.scheme().gpuEncoder;
            ImGui::TextColored(encColor, "%.1f%%", proc.gpuEncoderUtil);
        }

        if (proc.gpuDecoderUtil > 0.0)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("Video Decoder:");
            ImGui::TableNextColumn();
            const ImVec4 decColor = theme.scheme().gpuDecoder;
            ImGui::TextColored(decColor, "%.1f%%", proc.gpuDecoderUtil);
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Per-GPU breakdown if available
    if (!proc.perGpuUsage.empty())
    {
        ImGui::Text("Per-GPU Breakdown:");
        ImGui::Spacing();

        const ImVec4 gpuUtilColor = theme.scheme().gpuUtilization;
        const ImVec4 gpuMemColor = theme.scheme().gpuMemory;

        for (const auto& gpuUsage : proc.perGpuUsage)
        {
            const std::string gpuLabel =
                std::format("{} {} [{}]", ICON_FA_MICROCHIP, gpuUsage.gpuName, gpuUsage.isIntegrated ? "Integrated" : "Discrete");

            if (ImGui::CollapsingHeader(gpuLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Indent();

                if (ImGui::BeginTable("PerGPUMetrics", 2, ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0F);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("Utilization:");
                    ImGui::TableNextColumn();
                    ImGui::TextColored(gpuUtilColor, "%.1f%%", gpuUsage.utilPercent);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("Memory:");
                    ImGui::TableNextColumn();
                    const std::string memoryStr = UI::Format::formatBytes(static_cast<double>(gpuUsage.memoryBytes));
                    ImGui::TextColored(gpuMemColor, "%s", memoryStr.c_str());

                    if (!gpuUsage.engines.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("Engines:");
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

    ImGui::Separator();
    ImGui::Spacing();

    // GPU history graphs (if we have history)
    if (!m_GpuUtilHistory.empty() && !m_Timestamps.empty())
    {
        const size_t alignedCount = std::min(m_GpuUtilHistory.size(), m_Timestamps.size());
        std::vector<double> gpuUtilVec = tailVector(m_GpuUtilHistory, alignedCount);
        std::vector<double> gpuMemVec = tailVector(m_GpuMemHistory, alignedCount);
        std::vector<double> timeVec = tailVector(m_Timestamps, alignedCount);
        const auto* gpuUtilData = gpuUtilVec.data();
        const auto* gpuMemData = gpuMemVec.data();
        const auto* timeData = timeVec.data();

        // GPU Utilization graph
        auto plotGpuUtil = [&]()
        {
            if (ImPlot::BeginPlot("##GPUUtilPlot", ImVec2(-1, -1), UI::Widgets::PLOT_FLAGS_DEFAULT))
            {
                ImPlot::SetupAxes("Time", "GPU %", UI::Widgets::X_AXIS_FLAGS_DEFAULT, UI::Widgets::Y_AXIS_FLAGS_DEFAULT);
                ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisLocalized);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImPlotCond_Always);

                if (alignedCount > 0)
                {
                    plotLineWithFill("GPU %",
                                     timeData,
                                     gpuUtilData,
                                     static_cast<int>(alignedCount),
                                     theme.scheme().gpuUtilization,
                                     theme.scheme().gpuUtilizationFill);

                    // Tooltip
                    if (ImPlot::IsPlotHovered())
                    {
                        const auto idxVal = hoveredIndexFromPlotX(gpuUtilVec, ImPlot::GetPlotMousePos().x);
                        if (idxVal.has_value())
                        {
                            ImGui::BeginTooltip();
                            const auto ageText = formatAgeSeconds(timeData[*idxVal]);
                            ImGui::TextUnformatted(ageText.c_str());
                            ImGui::TextColored(theme.scheme().textInfo, "GPU: %.1f%%", gpuUtilData[*idxVal]);
                            ImGui::EndTooltip();
                        }
                    }
                }
                else
                {
                    ImPlot::PlotDummy("GPU %");
                }

                ImPlot::EndPlot();
            }
        };

        // GPU Memory graph
        auto plotGpuMem = [&]()
        {
            if (ImPlot::BeginPlot("##GPUMemPlot", ImVec2(-1, -1), UI::Widgets::PLOT_FLAGS_DEFAULT))
            {
                ImPlot::SetupAxes("Time", "GPU Memory", UI::Widgets::X_AXIS_FLAGS_DEFAULT, UI::Widgets::Y_AXIS_FLAGS_DEFAULT);
                ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisLocalized);

                if (alignedCount > 0)
                {
                    plotLineWithFill("GPU Memory",
                                     timeData,
                                     gpuMemData,
                                     static_cast<int>(alignedCount),
                                     theme.scheme().gpuMemory,
                                     theme.scheme().gpuMemoryFill);

                    // Tooltip
                    if (ImPlot::IsPlotHovered())
                    {
                        const auto idxVal = hoveredIndexFromPlotX(gpuMemVec, ImPlot::GetPlotMousePos().x);
                        if (idxVal.has_value())
                        {
                            ImGui::BeginTooltip();
                            const auto ageText = formatAgeSeconds(timeData[*idxVal]);
                            ImGui::TextUnformatted(ageText.c_str());
                            const std::string memStr = UI::Format::formatBytes(static_cast<double>(gpuMemData[*idxVal]));
                            ImGui::TextColored(theme.scheme().textInfo, "GPU Memory: %s", memStr.c_str());
                            ImGui::EndTooltip();
                        }
                    }
                }
                else
                {
                    ImPlot::PlotDummy("GPU Memory");
                }

                ImPlot::EndPlot();
            }
        };

        // Now bars for current values
        const NowBar gpuUtilBar{
            .valueText = std::format("{:.1f}%", m_SmoothedUsage.gpuUtilPercent),
            .label = "GPU Utilization",
            .value01 = m_SmoothedUsage.gpuUtilPercent / 100.0,
            .color = theme.scheme().gpuUtilization,
        };

        const NowBar gpuMemBar{
            .valueText = UI::Format::formatBytes(m_SmoothedUsage.gpuMemoryBytes),
            .label = "GPU Memory",
            .value01 = 0.0, // Auto-scale by setting to 0
            .color = theme.scheme().gpuMemory,
        };

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CHART_LINE "  GPU Utilization History (%zu samples)", alignedCount);
        renderHistoryWithNowBars(
            "ProcessGPUUtilHistory", HISTORY_PLOT_HEIGHT_DEFAULT, plotGpuUtil, {gpuUtilBar}, false, PROCESS_NOW_BAR_COLUMNS);
        ImGui::Spacing();

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CHART_LINE "  GPU Memory History (%zu samples)", alignedCount);
        renderHistoryWithNowBars(
            "ProcessGPUMemHistory", HISTORY_PLOT_HEIGHT_DEFAULT, plotGpuMem, {gpuMemBar}, false, PROCESS_NOW_BAR_COLUMNS);
        ImGui::Spacing();
    }
    else
    {
        ImGui::TextColored(theme.scheme().textMuted, "Collecting GPU history data...");
    }
}

void ProcessDetailsPanel::trimHistory(double nowSeconds)
{
    const double cutoff = nowSeconds - m_MaxHistorySeconds;
    size_t removeCount = 0;
    while (!m_Timestamps.empty() && (m_Timestamps.front() < cutoff))
    {
        m_Timestamps.pop_front();
        ++removeCount;
    }

    auto trimDeque = [removeCount](auto& dq)
    {
        for (size_t i = 0; i < removeCount && !dq.empty(); ++i)
        {
            dq.pop_front();
        }
    };

    trimDeque(m_CpuHistory);
    trimDeque(m_CpuUserHistory);
    trimDeque(m_CpuSystemHistory);
    trimDeque(m_MemoryHistory);
    trimDeque(m_SharedHistory);
    trimDeque(m_VirtualHistory);
    trimDeque(m_ThreadHistory);
    trimDeque(m_PageFaultHistory);
    trimDeque(m_IoReadHistory);
    trimDeque(m_IoWriteHistory);
    trimDeque(m_NetSentHistory);
    trimDeque(m_NetRecvHistory);
    trimDeque(m_PowerHistory);
    trimDeque(m_GpuUtilHistory);
    trimDeque(m_GpuMemHistory);

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
    updateMin(m_PageFaultHistory.size());
    updateMin(m_IoReadHistory.size());
    updateMin(m_IoWriteHistory.size());
    updateMin(m_NetSentHistory.size());
    updateMin(m_NetRecvHistory.size());
    updateMin(m_PowerHistory.size());
    updateMin(m_GpuUtilHistory.size());
    updateMin(m_GpuMemHistory.size());

    if (minSize != std::numeric_limits<size_t>::max())
    {
        auto trimToMin = [minSize](auto& dq)
        {
            while (dq.size() > minSize)
            {
                dq.pop_front();
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
        trimToMin(m_PageFaultHistory);
        trimToMin(m_IoReadHistory);
        trimToMin(m_IoWriteHistory);
        trimToMin(m_NetSentHistory);
        trimToMin(m_NetRecvHistory);
        trimToMin(m_PowerHistory);
        trimToMin(m_GpuUtilHistory);
        trimToMin(m_GpuMemHistory);
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

    // Action result feedback
    if (!m_LastActionResult.empty())
    {
        const bool isError = m_LastActionResult.contains("Error") || m_LastActionResult.contains("Failed");
        const ImVec4 color = isError ? theme.scheme().textError : theme.scheme().textSuccess;
        ImGui::TextColored(color, "%s", m_LastActionResult.c_str());
        ImGui::Spacing();
    }

    // Confirmation dialog
    if (m_ShowConfirmDialog)
    {
        ImGui::OpenPopup("Confirm Action");
    }

    if (ImGui::BeginPopupModal("Confirm Action", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text(
            "Are you sure you want to %s process '%s' (PID %d)?", m_ConfirmAction.c_str(), m_CachedSnapshot.name.c_str(), m_SelectedPid);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Yes", ImVec2(120, 0)))
        {
            Platform::ProcessActionResult result;

            if (m_ConfirmAction == "terminate")
            {
                result = m_ProcessActions->terminate(m_SelectedPid);
            }
            else if (m_ConfirmAction == "kill")
            {
                result = m_ProcessActions->kill(m_SelectedPid);
            }
            else if (m_ConfirmAction == "stop")
            {
                result = m_ProcessActions->stop(m_SelectedPid);
            }
            else if (m_ConfirmAction == "resume")
            {
                result = m_ProcessActions->resume(m_SelectedPid);
            }

            if (result.success)
            {
                m_LastActionResult = "Success: " + m_ConfirmAction + " sent to PID " + std::to_string(m_SelectedPid);
            }
            else
            {
                m_LastActionResult = "Error: " + result.errorMessage;
            }
            m_ActionResultTimer = 5.0F;

            m_ShowConfirmDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("No", ImVec2(120, 0)))
        {
            m_ShowConfirmDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // Action buttons - use consistent sizing and 2x2 grid layout
    constexpr float BUTTON_WIDTH = 180.0F;
    constexpr float BUTTON_HEIGHT = 0.0F; // Use default height
    const ImVec2 buttonSize(BUTTON_WIDTH, BUTTON_HEIGHT);

    // Use a table for consistent alignment
    if (ImGui::BeginTable("ActionButtons", 2, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Col1", ImGuiTableColumnFlags_WidthFixed, BUTTON_WIDTH + 8.0F);
        ImGui::TableSetupColumn("Col2", ImGuiTableColumnFlags_WidthFixed, BUTTON_WIDTH + 8.0F);

        // Row 1: Terminate and Kill
        ImGui::TableNextRow();

        // Terminate - graceful shutdown
        ImGui::TableNextColumn();
        if (m_ActionCapabilities.canTerminate)
        {
            if (ImGui::Button(ICON_FA_XMARK " Terminate", buttonSize))
            {
                m_ConfirmAction = "terminate";
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
            if (ImGui::Button(ICON_FA_SKULL " Kill", buttonSize))
            {
                m_ConfirmAction = "kill";
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
            if (ImGui::Button(ICON_FA_PAUSE " Pause", buttonSize))
            {
                m_ConfirmAction = "stop";
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
            if (ImGui::Button(ICON_FA_PLAY " Resume", buttonSize))
            {
                m_ConfirmAction = "resume";
                m_ShowConfirmDialog = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Resume a paused process");
            }
        }

        ImGui::EndTable();
    }

    // Priority adjustment section
    if (m_ActionCapabilities.canSetPriority)
    {
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

        // theme already declared at start of function
        auto* drawList = ImGui::GetWindowDrawList();
        const ImGuiStyle& style = ImGui::GetStyle();

        // ========================================
        // Custom gradient priority slider
        // ----------------------------------------
        // Dimension rationale:
        // - SLIDER_WIDTH (400px): Fits comfortably in the Process Details panel
        //   while providing enough precision for the 40-value nice range.
        // - SLIDER_HEIGHT (12px): Slightly taller than ImGui's default frame height
        //   to make the gradient clearly visible without dominating the row.
        // - BADGE_HEIGHT (24px): Matches a typical label-sized pill that fits
        //   the 1-2 digit nice value text with padding.
        // - BADGE_ARROW_SIZE: Proportional to badge height (1/4) so the pointer
        //   visually connects the badge to the slider without overpowering it.
        // These could be theme-configurable in the future if needed.
        // ========================================
        constexpr float SLIDER_WIDTH = 400.0F;
        constexpr float SLIDER_HEIGHT = 12.0F;
        constexpr float BADGE_HEIGHT = 24.0F;
        constexpr float BADGE_ARROW_SIZE = 6.0F;

        // Calculate normalized position (0.0 = -20, 1.0 = 19)
        constexpr float NICE_RANGE = static_cast<float>(Domain::Priority::MAX_NICE - Domain::Priority::MIN_NICE);
        const float normalizedPos = static_cast<float>(m_PriorityNiceValue - Domain::Priority::MIN_NICE) / NICE_RANGE;

        // Get color for current nice value using a red-green-blue gradient:
        //   - Red (high priority, nice=-20): R=1.0, G=0.3, B=0.2
        //   - Green (normal, nice=0): R=0.5, G=0.8, B=0.2
        //   - Blue (low priority, nice=19): R=0.4, G=0.4, B=0.8
        // The gradient interpolates smoothly between these anchor colors.
        auto getNiceColor = [](int nice) -> ImVec4
        {
            // Normalize to 0-1 range
            const float t = static_cast<float>(nice - Domain::Priority::MIN_NICE) /
                            static_cast<float>(Domain::Priority::MAX_NICE - Domain::Priority::MIN_NICE);

            if (t < 0.5F)
            {
                // Red to Green (high priority to normal)
                const float localT = t * 2.0F;        // 0-1 within red-green range
                return ImVec4(1.0F - (localT * 0.5F), // R: 1.0 -> 0.5
                              0.3F + (localT * 0.5F), // G: 0.3 -> 0.8
                              0.2F,                   // B: constant low
                              1.0F);
            }
            // Green to Blue (normal to low priority)
            const float localT = (t - 0.5F) * 2.0F; // 0-1 within green-blue range
            return ImVec4(0.2F + (localT * 0.2F),   // R: 0.2 -> 0.4
                          0.8F - (localT * 0.4F),   // G: 0.8 -> 0.4
                          0.3F + (localT * 0.5F),   // B: 0.3 -> 0.8
                          1.0F);
        };

        // Reserve space for badge above slider
        const ImVec2 cursorStart = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(SLIDER_WIDTH, BADGE_HEIGHT + BADGE_ARROW_SIZE));

        // Draw the value badge/callout above the slider position
        {
            const float badgeX = cursorStart.x + (normalizedPos * SLIDER_WIDTH);
            const float badgeY = cursorStart.y;

            // Badge text
            const std::string valueText = std::to_string(m_PriorityNiceValue);
            const ImVec2 textSize = ImGui::CalcTextSize(valueText.c_str());
            const float badgeWidth = textSize.x + (style.FramePadding.x * 2.0F);
            const float badgeHalfWidth = badgeWidth * 0.5F;

            // Clamp badge position to stay within slider bounds
            const float clampedBadgeX = std::clamp(badgeX, cursorStart.x + badgeHalfWidth, cursorStart.x + SLIDER_WIDTH - badgeHalfWidth);

            // Badge rectangle
            const ImVec2 badgeMin(clampedBadgeX - badgeHalfWidth, badgeY);
            const ImVec2 badgeMax(clampedBadgeX + badgeHalfWidth, badgeY + BADGE_HEIGHT);

            // Badge color based on nice value
            const ImVec4 badgeColor = getNiceColor(m_PriorityNiceValue);
            const ImU32 badgeColorU32 = ImGui::ColorConvertFloat4ToU32(badgeColor);

            // Draw badge rectangle with rounded corners
            drawList->AddRectFilled(badgeMin, badgeMax, badgeColorU32, 4.0F);

            // Draw arrow pointing down from badge
            const ImVec2 arrowTip(badgeX, badgeMax.y + BADGE_ARROW_SIZE);
            const ImVec2 arrowLeft(badgeX - BADGE_ARROW_SIZE, badgeMax.y);
            const ImVec2 arrowRight(badgeX + BADGE_ARROW_SIZE, badgeMax.y);
            drawList->AddTriangleFilled(arrowLeft, arrowRight, arrowTip, badgeColorU32);

            // Draw badge text (white for contrast)
            const ImVec2 textPos(clampedBadgeX - (textSize.x * 0.5F), badgeY + ((BADGE_HEIGHT - textSize.y) * 0.5F));
            drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), valueText.c_str());
        }

        // Draw the gradient slider bar
        const ImVec2 sliderStart = ImGui::GetCursorScreenPos();
        const ImVec2 sliderMin = sliderStart;
        const ImVec2 sliderMax(sliderStart.x + SLIDER_WIDTH, sliderStart.y + SLIDER_HEIGHT);

        // Draw gradient background (red -> green -> blue)
        constexpr int GRADIENT_SEGMENTS = 40;
        constexpr float SEGMENT_WIDTH = SLIDER_WIDTH / static_cast<float>(GRADIENT_SEGMENTS);
        for (int i = 0; i < GRADIENT_SEGMENTS; ++i)
        {
            const float t1 = static_cast<float>(i) / static_cast<float>(GRADIENT_SEGMENTS);
            const float t2 = static_cast<float>(i + 1) / static_cast<float>(GRADIENT_SEGMENTS);
            const int nice1 = Domain::Priority::MIN_NICE + static_cast<int>(t1 * NICE_RANGE);
            const int nice2 = Domain::Priority::MIN_NICE + static_cast<int>(t2 * NICE_RANGE);
            const ImU32 col1 = ImGui::ColorConvertFloat4ToU32(getNiceColor(nice1));
            const ImU32 col2 = ImGui::ColorConvertFloat4ToU32(getNiceColor(nice2));

            const ImVec2 segMin(sliderMin.x + (static_cast<float>(i) * SEGMENT_WIDTH), sliderMin.y);
            const ImVec2 segMax(sliderMin.x + (static_cast<float>(i + 1) * SEGMENT_WIDTH), sliderMax.y);

            drawList->AddRectFilledMultiColor(segMin, segMax, col1, col2, col2, col1);
        }

        // Draw slider border
        drawList->AddRect(sliderMin, sliderMax, ImGui::GetColorU32(ImGuiCol_Border), 2.0F);

        // Draw slider thumb/handle
        {
            const float thumbX = sliderMin.x + (normalizedPos * SLIDER_WIDTH);
            const float thumbRadius = SLIDER_HEIGHT * 0.6F;
            const ImVec2 thumbCenter(thumbX, sliderMin.y + (SLIDER_HEIGHT * 0.5F));

            // Thumb outline
            drawList->AddCircleFilled(thumbCenter, thumbRadius + 2.0F, ImGui::GetColorU32(ImGuiCol_Border));
            // Thumb fill (white)
            drawList->AddCircleFilled(thumbCenter, thumbRadius, IM_COL32(255, 255, 255, 255));
        }

        // Make the slider interactive with an invisible button
        ImGui::InvisibleButton("##priority_slider", ImVec2(SLIDER_WIDTH, SLIDER_HEIGHT));
        if (ImGui::IsItemActive())
        {
            const float mouseX = ImGui::GetIO().MousePos.x;
            const float relX = std::clamp((mouseX - sliderMin.x) / SLIDER_WIDTH, 0.0F, 1.0F);
            const int newNice = Domain::Priority::MIN_NICE + static_cast<int>(relX * NICE_RANGE);
            if (newNice != m_PriorityNiceValue)
            {
                m_PriorityNiceValue = newNice;
                m_PriorityChanged = true;
                // Clear any previous error when user interacts with slider
                // This provides fresher feedback rather than showing stale errors
                m_PriorityError.clear();
            }
        }

        // Draw scale labels below the slider
        ImGui::Spacing();
        {
            const float contentStartX = ImGui::GetCursorPosX();

            // "High" label (left, colored red)
            ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textError);
            ImGui::TextUnformatted("High");
            ImGui::PopStyleColor();

            // Scale tick labels (muted color)
            // NOTE: The spacing in this string is approximate and may not perfectly align
            // with all font sizes. Consider dynamically positioning labels if precision is needed.
            ImGui::SameLine();
            ImGui::SetCursorPosX(contentStartX + 35.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textMuted);
            ImGui::TextUnformatted("-20   -15   -10   -5    0    5    10    15    19");
            ImGui::PopStyleColor();

            // "Low" label (right, colored blue)
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textInfo);
            ImGui::TextUnformatted("Low");
            ImGui::PopStyleColor();

            // "Default" label centered below the 0 position
            // 0 is at normalized position 0.5128 (20 out of 39 range)
            constexpr float ZERO_NORMALIZED = 20.0F / 39.0F;
            const float defaultX = contentStartX + (ZERO_NORMALIZED * SLIDER_WIDTH);
            const ImVec2 defaultSize = ImGui::CalcTextSize("Default");
            ImGui::SetCursorPosX(defaultX - (defaultSize.x * 0.5F));
            ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textMuted);
            ImGui::TextUnformatted("Default");
            ImGui::PopStyleColor();
        }

        // Tooltip on hover
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Nice value: -20 (highest priority) to 19 (lowest priority)\n"
                              "Lower values = higher priority (more CPU time)\n"
                              "Normal priority = 0\n\n"
                              "Note: Setting values below 0 typically requires root/admin privileges");
        }

        ImGui::Spacing();

        // ========================================
        // Action button (right-aligned)
        // ========================================
        const bool canApply = m_PriorityChanged && m_HasSnapshot;

        // Right-align the Apply button
        constexpr float APPLY_BUTTON_WIDTH = 120.0F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + SLIDER_WIDTH - APPLY_BUTTON_WIDTH);

        // Apply button with success (green) styling
        {
            ImGui::PushStyleColor(ImGuiCol_Button, theme.scheme().successButton);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.scheme().successButtonHovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.scheme().successButtonActive);

            if (!canApply)
            {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Apply", ImVec2(APPLY_BUTTON_WIDTH, 0)))
            {
                auto result = m_ProcessActions->setPriority(m_SelectedPid, m_PriorityNiceValue);
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

            ImGui::PopStyleColor(3);
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
}

} // namespace App
