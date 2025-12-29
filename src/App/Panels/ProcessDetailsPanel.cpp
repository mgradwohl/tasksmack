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

    if (m_SelectedPid == -1)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textMuted, "Select a process from the Processes panel to view details");
        ImGui::End();
        return;
    }

    if (!m_HasSnapshot)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textWarning, "Process %d not found (may have exited)", m_SelectedPid);
        ImGui::End();
        return;
    }

    // Tabs for different info sections
    if (ImGui::BeginTabBar("DetailsTabs"))
    {
        if (ImGui::BeginTabItem(ICON_FA_CIRCLE_INFO " Overview"))
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

        if (ImGui::BeginTabItem(ICON_FA_GEARS " Actions"))
        {
            renderActions();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
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
        m_Timestamps.clear();
        m_HistoryTimer = 0.0F;
        m_HasSnapshot = false;
        m_ShowConfirmDialog = false;
        m_LastActionResult.clear();
        m_SmoothedUsage = {};
        m_PeakMemoryPercent = 0.0;
        m_PriorityChanged = false;
        m_PriorityNiceValue = 0;

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
                ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textMuted);
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
    ImGui::TextColored(theme.scheme().textMuted, ICON_FA_ID_CARD " Identity");
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
    ImGui::TextColored(theme.scheme().textMuted, ICON_FA_CLOCK " Runtime");
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
                                 .value01 = UI::Numeric::percent01(m_SmoothedUsage.cpuPercent),
                                 .color = theme.progressColor(m_SmoothedUsage.cpuPercent)};
        const NowBar cpuUserNow{.valueText = UI::Format::percentCompact(m_SmoothedUsage.cpuUserPercent),
                                .value01 = UI::Numeric::percent01(m_SmoothedUsage.cpuUserPercent),
                                .color = theme.scheme().cpuUser};
        const NowBar cpuSystemNow{.valueText = UI::Format::percentCompact(m_SmoothedUsage.cpuSystemPercent),
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

        ImGui::Text(ICON_FA_MICROCHIP " CPU (%zu samples)", alignedCount);
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
                                  .value01 = UI::Numeric::percent01(usedNow),
                                  .color = theme.scheme().chartMemory});
            memoryBars.push_back({.valueText = UI::Format::percentCompact(sharedNow),
                                  .value01 = UI::Numeric::percent01(sharedNow),
                                  .color = theme.scheme().chartCpu});
            memoryBars.push_back({.valueText = UI::Format::percentCompact(virtNowVal),
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
                        ImVec4 peakColor = theme.scheme().textWarning; // Use warning color for visibility
                        peakColor.w = 0.7F;                            // Slightly transparent
                        ImPlot::SetNextLineStyle(peakColor, 1.5F);

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
            ImGui::Text(ICON_FA_MEMORY " Memory (%zu samples)", alignedCount);
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
                            .value01 = (threadMax > 0.0) ? std::clamp(m_SmoothedUsage.threadCount / threadMax, 0.0, 1.0) : 0.0,
                            .color = theme.scheme().chartCpu};

    const NowBar faultsBar{.valueText = UI::Format::formatCountPerSecond(m_SmoothedUsage.pageFaultsPerSec),
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

    ImGui::Text(ICON_FA_GEARS " Threads & Page Faults (%zu samples)", alignedCount);
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
                         .value01 = (readMax > 0.0) ? std::clamp(m_SmoothedUsage.ioReadBytesPerSec / readMax, 0.0, 1.0) : 0.0,
                         .color = theme.scheme().chartIo};

    const NowBar writeBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.ioWriteBytesPerSec, writeUnit),
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

    ImGui::Text(ICON_FA_HARD_DRIVE " I/O Statistics (%zu samples)", alignedCount);
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
                         .value01 = (sentMax > 0.0) ? std::clamp(m_SmoothedUsage.netSentBytesPerSec / sentMax, 0.0, 1.0) : 0.0,
                         .color = theme.scheme().chartCpu};

    const NowBar recvBar{.valueText = UI::Format::formatBytesPerSecWithUnit(m_SmoothedUsage.netRecvBytesPerSec, recvUnit),
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

    ImGui::Text(ICON_FA_NETWORK_WIRED " Network - Avg Rate (%zu samples)", alignedCount);
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

    ImGui::Text(ICON_FA_BOLT " Power Usage (%zu samples)", alignedCount);
    renderHistoryWithNowBars("ProcessPowerHistory", HISTORY_PLOT_HEIGHT_DEFAULT, plot, {powerBar}, false, PROCESS_NOW_BAR_COLUMNS);
    ImGui::Spacing();
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
    }
}

void ProcessDetailsPanel::renderActions()
{
    const auto& theme = UI::Theme::get();

    ImGui::TextColored(theme.scheme().textMuted, "Target: %s (PID %d)", m_CachedSnapshot.name.c_str(), m_SelectedPid);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Warning text
    ImGui::TextColored(theme.scheme().textWarning, "Warning: These actions affect the running process!");
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

    // Action buttons
    ImGui::BeginGroup();

    // Terminate (SIGTERM) - graceful
    if (m_ActionCapabilities.canTerminate)
    {
        if (ImGui::Button(ICON_FA_XMARK " Terminate (SIGTERM)", ImVec2(200, 0)))
        {
            m_ConfirmAction = "terminate";
            m_ShowConfirmDialog = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Send SIGTERM - request graceful shutdown");
        }
    }

    ImGui::SameLine();

    // Kill (SIGKILL) - forceful
    if (m_ActionCapabilities.canKill)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, theme.scheme().dangerButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.scheme().dangerButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.scheme().dangerButtonActive);
        if (ImGui::Button(ICON_FA_SKULL " Kill (SIGKILL)", ImVec2(200, 0)))
        {
            m_ConfirmAction = "kill";
            m_ShowConfirmDialog = true;
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Send SIGKILL - force kill (cannot be caught)");
        }
    }

    ImGui::Spacing();

    // Stop (SIGSTOP) - pause
    if (m_ActionCapabilities.canStop)
    {
        if (ImGui::Button("Stop (SIGSTOP)", ImVec2(180, 0)))
        {
            m_ConfirmAction = "stop";
            m_ShowConfirmDialog = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Send SIGSTOP - pause the process");
        }
    }

    ImGui::SameLine();

    // Continue (SIGCONT) - resume
    if (m_ActionCapabilities.canContinue)
    {
        if (ImGui::Button("Resume (SIGCONT)", ImVec2(180, 0)))
        {
            m_ConfirmAction = "resume";
            m_ShowConfirmDialog = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Send SIGCONT - resume a stopped process");
        }
    }

    ImGui::EndGroup();

    // Priority adjustment section
    if (m_ActionCapabilities.canSetPriority)
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Priority Adjustment (Nice Value)");
        ImGui::Spacing();

        // Initialize slider from current process nice value if not changed
        if (!m_PriorityChanged && m_HasSnapshot)
        {
            m_PriorityNiceValue = m_CachedSnapshot.nice;
        }

        // Nice value slider: -20 (highest priority) to 19 (lowest priority)
        ImGui::PushItemWidth(300);
        if (ImGui::SliderInt("##nice", &m_PriorityNiceValue, Domain::Priority::MIN_NICE, Domain::Priority::MAX_NICE, "%d"))
        {
            m_PriorityChanged = true;
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();

        // Priority label - uses shared Domain::Priority::getPriorityLabel()
        const std::string_view priorityLabel = Domain::Priority::getPriorityLabel(m_PriorityNiceValue);
        ImGui::Text("(%.*s)", static_cast<int>(priorityLabel.size()), priorityLabel.data());

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Nice value: -20 (highest priority) to 19 (lowest priority)\n"
                              "Lower values mean higher priority\n"
                              "Normal priority = 0\n"
                              "Note: Setting nice values below 0 (higher priority) typically requires root/admin privileges");
        }

        // Apply, Set Normal, and Reset buttons
        // Note: canApply relies on m_PriorityChanged flag rather than comparing with cached snapshot,
        // since the cached snapshot value may be stale if priority changed externally.
        const bool canApply = m_PriorityChanged && m_HasSnapshot;

        if (!canApply)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Apply", ImVec2(80, 0)))
        {
            auto result = m_ProcessActions->setPriority(m_SelectedPid, m_PriorityNiceValue);
            if (result.success)
            {
                m_LastActionResult =
                    "Success: Priority set to " + std::to_string(m_PriorityNiceValue) + " for PID " + std::to_string(m_SelectedPid);
                m_PriorityChanged = false;
            }
            else
            {
                m_LastActionResult = "Error: " + result.errorMessage;
            }
            m_ActionResultTimer = 5.0F;
        }
        if (!canApply)
        {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Apply the selected priority to the process");
        }

        ImGui::SameLine();

        // Set to Normal (nice=0) button - uses shared constant from PriorityConfig.h
        // Dual behavior by design: if slider is not at 0, clicking moves it to 0 (preview).
        // If slider is already at 0, clicking applies normal priority immediately.
        // This provides a one-click "reset to normal" shortcut. Tooltip explains this.
        const bool isAlreadyNormal = m_HasSnapshot && (m_CachedSnapshot.nice == Domain::Priority::NORMAL_NICE) && !m_PriorityChanged;
        const bool sliderAtNormal = (m_PriorityNiceValue == Domain::Priority::NORMAL_NICE);

        if (isAlreadyNormal)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Set Normal (0)", ImVec2(110, 0)))
        {
            if (sliderAtNormal)
            {
                // Slider already at 0, just apply it
                auto result = m_ProcessActions->setPriority(m_SelectedPid, Domain::Priority::NORMAL_NICE);
                if (result.success)
                {
                    m_LastActionResult = "Success: Priority set to Normal (0) for PID " + std::to_string(m_SelectedPid);
                    m_PriorityChanged = false;
                    m_PriorityNiceValue = Domain::Priority::NORMAL_NICE;
                }
                else
                {
                    m_LastActionResult = "Error: " + result.errorMessage;
                }
                m_ActionResultTimer = 5.0F;
            }
            else
            {
                // Move slider to 0
                m_PriorityNiceValue = Domain::Priority::NORMAL_NICE;
                m_PriorityChanged = true;
            }
        }
        if (isAlreadyNormal)
        {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (isAlreadyNormal)
            {
                ImGui::SetTooltip("Process is already at normal priority");
            }
            else
            {
                ImGui::SetTooltip("Set priority to Normal (nice=0, default priority)");
            }
        }

        ImGui::SameLine();

        if (!m_PriorityChanged)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Undo", ImVec2(60, 0)))
        {
            if (m_HasSnapshot)
            {
                m_PriorityNiceValue = m_CachedSnapshot.nice;
            }
            m_PriorityChanged = false;
        }
        if (!m_PriorityChanged)
        {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Undo slider changes (revert to current process priority)");
        }
    }
}

} // namespace App
