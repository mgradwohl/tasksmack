#include "ProcessDetailsPanel.h"

#include "App/Panel.h"
#include "App/UserConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/Factory.h"
#include "Platform/IProcessActions.h"
#include "UI/Format.h"
#include "UI/HistoryWidgets.h"
#include "UI/Numeric.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
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
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

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
            m_Timestamps.push_back(nowSeconds);
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
        windowLabel = m_CachedSnapshot.name;
        windowLabel += "###ProcessDetails";
    }
    else
    {
        windowLabel = "Process Details###ProcessDetails";
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
        if (ImGui::BeginTabItem("Overview"))
        {
            renderBasicInfo(m_CachedSnapshot);
            ImGui::Separator();
            renderResourceUsage(m_CachedSnapshot);
            ImGui::Separator();
            renderIoStats(m_CachedSnapshot);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Actions"))
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
        m_Timestamps.clear();
        m_HistoryTimer = 0.0F;
        m_HasSnapshot = false;
        m_ShowConfirmDialog = false;
        m_LastActionResult.clear();
        m_SmoothedUsage = {};

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

    if (!m_SmoothedUsage.initialized || deltaTimeSeconds <= 0.0F)
    {
        m_SmoothedUsage.cpuPercent = targetCpu;
        m_SmoothedUsage.residentBytes = targetResident;
        m_SmoothedUsage.virtualBytes = targetVirtual;
        m_SmoothedUsage.initialized = true;
        return;
    }

    m_SmoothedUsage.cpuPercent = UI::Numeric::clampPercent(smoothTowards(m_SmoothedUsage.cpuPercent, targetCpu, alpha));
    m_SmoothedUsage.residentBytes = std::max(0.0, smoothTowards(m_SmoothedUsage.residentBytes, targetResident, alpha));
    m_SmoothedUsage.virtualBytes = smoothTowards(m_SmoothedUsage.virtualBytes, targetVirtual, alpha);
    m_SmoothedUsage.virtualBytes = std::max(m_SmoothedUsage.virtualBytes, m_SmoothedUsage.residentBytes);
}

void ProcessDetailsPanel::renderBasicInfo(const Domain::ProcessSnapshot& proc)
{
    const auto& theme = UI::Theme::get();

    const char* titleCommand = !proc.command.empty() ? proc.command.c_str() : proc.name.c_str();
    ImGui::TextWrapped("Command Line: %s", titleCommand);
    ImGui::Spacing();

    const auto computeLabelColumnWidth = []() -> float
    {
        // Keep labels from wrapping at large font sizes (prevents width/scrollbar jitter).
        constexpr std::array<const char*, 9> labels = {
            "PID",
            "Parent",
            "Name",
            "Status",
            "User",
            "Threads",
            "Nice",
            "CPU Time",
            "Page Faults",
        };

        float maxTextWidth = 0.0F;
        for (const char* label : labels)
        {
            maxTextWidth = std::max(maxTextWidth, ImGui::CalcTextSize(label).x);
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        return maxTextWidth + (style.CellPadding.x * 2.0F) + 6.0F;
    };

    const float labelColWidth = computeLabelColumnWidth();

    if (ImGui::BeginTable("BasicInfo",
                          4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                              ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label1", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, labelColWidth);
        ImGui::TableSetupColumn("Value1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Label2", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, labelColWidth);
        ImGui::TableSetupColumn("Value2", ImGuiTableColumnFlags_WidthStretch);

        auto renderStatusValue = [&]()
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

            ImGui::TextColored(statusColor, "%s", proc.displayState.c_str());
        };

        auto addLabel = [](const char* text)
        {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(text);
        };

        auto addValueText = [](const char* text)
        {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(text);
        };

        // Row 1: PID / Parent PID
        ImGui::TableNextRow();
        addLabel("PID");
        ImGui::TableNextColumn();
        ImGui::Text("%d", proc.pid);
        addLabel("Parent");
        ImGui::TableNextColumn();
        ImGui::Text("%d", proc.parentPid);

        // Row 2: Name / Status
        ImGui::TableNextRow();
        addLabel("Name");
        addValueText(proc.name.c_str());
        addLabel("Status");
        ImGui::TableNextColumn();
        renderStatusValue();

        // Row 3: User / Threads
        ImGui::TableNextRow();
        addLabel("User");
        addValueText(proc.user.empty() ? "-" : proc.user.c_str());
        addLabel("Threads");
        ImGui::TableNextColumn();
        if (proc.threadCount > 0)
        {
            ImGui::Text("%d", proc.threadCount);
        }
        else
        {
            ImGui::TextUnformatted("-");
        }

        // Row 4: Nice / CPU Time
        ImGui::TableNextRow();
        addLabel("Nice");
        ImGui::TableNextColumn();
        ImGui::Text("%d", proc.nice);
        addLabel("CPU Time");
        addValueText(UI::Format::formatCpuTimeCompact(proc.cpuTimeSeconds).c_str());

        // Row 5: Page Faults
        ImGui::TableNextRow();
        addLabel("Page Faults");
        ImGui::TableNextColumn();
        if (proc.pageFaults > 0)
        {
            ImGui::Text("%s", std::format("{:L}", proc.pageFaults).c_str());
        }
        else
        {
            ImGui::TextUnformatted("-");
        }

        ImGui::EndTable();
    }
}

void ProcessDetailsPanel::renderResourceUsage(const Domain::ProcessSnapshot& proc)
{
    const auto& theme = UI::Theme::get();

    // Ensure smoothing is initialized even if render is called before an update tick
    if (!m_SmoothedUsage.initialized)
    {
        updateSmoothedUsage(proc, m_LastDeltaSeconds);
    }

    const double cpuPercent = m_SmoothedUsage.initialized ? m_SmoothedUsage.cpuPercent : proc.cpuPercent;
    const double cpuClamped = std::clamp(cpuPercent, 0.0, 100.0);

    // Inline CPU history with paired now bar
    if (!m_Timestamps.empty() && !m_CpuHistory.empty())
    {
        const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const size_t alignedCount =
            std::min({m_Timestamps.size(), m_CpuHistory.size(), m_CpuUserHistory.size(), m_CpuSystemHistory.size()});

        std::vector<double> timestamps = tailVector(m_Timestamps, alignedCount);
        std::vector<double> cpuData = tailVector(m_CpuHistory, alignedCount);
        std::vector<double> cpuUserData = tailVector(m_CpuUserHistory, alignedCount);
        std::vector<double> cpuSystemData = tailVector(m_CpuSystemHistory, alignedCount);

        const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
        std::vector<double> cpuTimeData = buildTimeAxisDoubles(timestamps, alignedCount, nowSeconds);

        NowBar cpuTotalNow{.valueText = UI::Format::percentCompact(cpuClamped),
                           .value01 = UI::Numeric::percent01(cpuClamped),
                           .color = theme.progressColor(cpuClamped)};
        NowBar cpuUserNow{.valueText = UI::Format::percentCompact(proc.cpuUserPercent),
                          .value01 = UI::Numeric::percent01(proc.cpuUserPercent),
                          .color = theme.scheme().cpuUser};
        NowBar cpuSystemNow{.valueText = UI::Format::percentCompact(proc.cpuSystemPercent),
                            .value01 = UI::Numeric::percent01(proc.cpuSystemPercent),
                            .color = theme.scheme().cpuSystem};

        auto cpuPlot = [&]()
        {
            if (ImPlot::BeginPlot("##ProcOverviewCPU", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus))
            {
                ImPlot::SetupAxes("Time (s)", "%", X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
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
                                ImGui::Text("t: %.1fs", cpuTimeData[*idxVal]);
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

        ImGui::Text("CPU (%zu samples)", alignedCount);
        renderHistoryWithNowBars(
            "ProcessCPUHistoryOverview", HISTORY_PLOT_HEIGHT_DEFAULT, cpuPlot, {cpuTotalNow, cpuUserNow, cpuSystemNow});
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
            std::vector<double> timestamps = tailVector(m_Timestamps, alignedCount);
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
                if (ImPlot::BeginPlot(
                        "##ProcOverviewMemory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus))
                {
                    ImPlot::SetupAxes("Time (s)", "%", X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

                    if (!usedData.empty())
                    {
                        ImPlot::SetNextLineStyle(theme.scheme().chartMemory, 2.0F);
                        ImPlot::PlotLine("Used", timeData.data(), usedData.data(), UI::Numeric::checkedCount(usedData.size()));
                    }

                    if (!sharedData.empty())
                    {
                        ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                        ImPlot::PlotLine("Shared", timeData.data(), sharedData.data(), UI::Numeric::checkedCount(sharedData.size()));
                    }

                    if (!virtData.empty())
                    {
                        ImPlot::SetNextLineStyle(theme.scheme().chartIo, 2.0F);
                        ImPlot::PlotLine("Virtual", timeData.data(), virtData.data(), UI::Numeric::checkedCount(virtData.size()));
                    }

                    if (ImPlot::IsPlotHovered())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                        {
                            ImGui::BeginTooltip();
                            ImGui::Text("t: %.1fs", timeData[*idxVal]);
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
                            ImGui::EndTooltip();
                        }
                    }

                    ImPlot::EndPlot();
                }
            };

            ImGui::Spacing();
            ImGui::Text("Memory (%zu samples)", alignedCount);
            renderHistoryWithNowBars("ProcessMemoryOverviewLayout", HISTORY_PLOT_HEIGHT_DEFAULT, memoryPlot, memoryBars);
            ImGui::Spacing();
        }
    }
}

void ProcessDetailsPanel::renderIoStats(const Domain::ProcessSnapshot& proc)
{
    // Only show I/O stats if we have data
    if (proc.ioReadBytesPerSec == 0.0 && proc.ioWriteBytesPerSec == 0.0)
    {
        return;
    }

    ImGui::Text("I/O Statistics");
    ImGui::Spacing();

    auto formatRate = [](double bytesPerSec) -> std::pair<double, const char*>
    {
        if (bytesPerSec >= 1024.0 * 1024.0)
        {
            return {bytesPerSec / (1024.0 * 1024.0), "MB/s"};
        }
        if (bytesPerSec >= 1024.0)
        {
            return {bytesPerSec / 1024.0, "KB/s"};
        }
        return {bytesPerSec, "B/s"};
    };

    auto [readVal, readUnit] = formatRate(proc.ioReadBytesPerSec);
    auto [writeVal, writeUnit] = formatRate(proc.ioWriteBytesPerSec);

    ImGui::Text("Read:");
    ImGui::SameLine(80.0F);
    ImGui::Text("%.1f %s", readVal, readUnit);

    ImGui::Text("Write:");
    ImGui::SameLine(80.0F);
    ImGui::Text("%.1f %s", writeVal, writeUnit);
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
        bool isError = m_LastActionResult.contains("Error") || m_LastActionResult.contains("Failed");
        ImVec4 color = isError ? theme.scheme().textError : theme.scheme().textSuccess;
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
        if (ImGui::Button("Terminate (SIGTERM)", ImVec2(180, 0)))
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
        if (ImGui::Button("Kill (SIGKILL)", ImVec2(180, 0)))
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
}

} // namespace App
