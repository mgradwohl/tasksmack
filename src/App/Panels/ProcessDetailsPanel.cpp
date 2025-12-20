#include "ProcessDetailsPanel.h"

#include "App/UserConfig.h"
#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/HistoryWidgets.h"
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
#include <string>
#include <vector>

namespace
{

using UI::Widgets::buildTimeAxis;
using UI::Widgets::computeAlpha;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

[[nodiscard]] auto formatCpuTime(double seconds) -> std::string
{
    seconds = std::max(0.0, seconds);

    const uint64_t totalMs = static_cast<uint64_t>(seconds * 1000.0);
    const uint64_t hours = totalMs / (1000ULL * 60ULL * 60ULL);
    const uint64_t minutes = (totalMs / (1000ULL * 60ULL)) % 60ULL;
    const uint64_t secs = (totalMs / 1000ULL) % 60ULL;
    const uint64_t centis = (totalMs / 10ULL) % 100ULL;

    if (hours > 0)
    {
        return std::format("{}:{:02}:{:02}.{:02}", hours, minutes, secs, centis);
    }

    return std::format("{}:{:02}.{:02}", minutes, secs, centis);
}

} // namespace

namespace App
{

ProcessDetailsPanel::ProcessDetailsPanel()
    : Panel("Process Details"),
      m_MaxHistorySeconds(static_cast<double>(App::UserConfig::get().settings().maxHistorySeconds)),
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
            m_CpuHistory.push_back(static_cast<float>(snapshot->cpuPercent));
            m_CpuUserHistory.push_back(static_cast<float>(snapshot->cpuUserPercent));
            m_CpuSystemHistory.push_back(static_cast<float>(snapshot->cpuSystemPercent));

            // Derive system total memory from RSS percent to express other metrics as percents for consistent charting.
            const double usedPercent = std::clamp(snapshot->memoryPercent, 0.0, 100.0);
            double totalMemoryBytes = 0.0;
            if (usedPercent > 0.0 && snapshot->memoryBytes > 0)
            {
                totalMemoryBytes = (static_cast<double>(snapshot->memoryBytes) * 100.0) / usedPercent;
            }

            auto toPercent = [totalMemoryBytes](uint64_t bytes) -> double
            {
                if (totalMemoryBytes <= 0.0)
                {
                    return 0.0;
                }
                return std::clamp((static_cast<double>(bytes) / totalMemoryBytes) * 100.0, 0.0, 100.0);
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

void ProcessDetailsPanel::setSelectedPid(int32_t pid)
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
    auto clampPercent = [](double value)
    {
        return std::clamp(value, 0.0, 100.0);
    };

    const auto refreshMs = std::chrono::milliseconds(App::UserConfig::get().settings().refreshIntervalMs);
    const double alpha = computeAlpha(static_cast<double>(deltaTimeSeconds), refreshMs);

    const double targetCpu = clampPercent(snapshot.cpuPercent);
    const double targetResident = static_cast<double>(snapshot.memoryBytes);
    const double targetVirtual = static_cast<double>(std::max(snapshot.virtualBytes, snapshot.memoryBytes));

    if (!m_SmoothedUsage.initialized || deltaTimeSeconds <= 0.0F)
    {
        m_SmoothedUsage.cpuPercent = targetCpu;
        m_SmoothedUsage.residentBytes = targetResident;
        m_SmoothedUsage.virtualBytes = targetVirtual;
        m_SmoothedUsage.initialized = true;
        return;
    }

    m_SmoothedUsage.cpuPercent = clampPercent(smoothTowards(m_SmoothedUsage.cpuPercent, targetCpu, alpha));
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
        constexpr std::array<const char*, 8> labels = {
            "PID",
            "Parent",
            "Name",
            "Status",
            "User",
            "Threads",
            "Nice",
            "CPU Time",
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
        addValueText(formatCpuTime(proc.cpuTimeSeconds).c_str());

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
            std::min({m_Timestamps.size(), static_cast<size_t>(m_CpuHistory.size()), m_CpuUserHistory.size(), m_CpuSystemHistory.size()});

        std::vector<double> timestamps(m_Timestamps.begin(), m_Timestamps.end());
        std::vector<double> cpuData(m_CpuHistory.begin(), m_CpuHistory.end());
        std::vector<double> cpuUserData(m_CpuUserHistory.begin(), m_CpuUserHistory.end());
        std::vector<double> cpuSystemData(m_CpuSystemHistory.begin(), m_CpuSystemHistory.end());

        if (timestamps.size() > alignedCount)
        {
            timestamps.erase(timestamps.begin(), timestamps.begin() + static_cast<std::ptrdiff_t>(timestamps.size() - alignedCount));
        }
        if (cpuData.size() > alignedCount)
        {
            cpuData.erase(cpuData.begin(), cpuData.begin() + static_cast<std::ptrdiff_t>(cpuData.size() - alignedCount));
        }
        if (cpuUserData.size() > alignedCount)
        {
            cpuUserData.erase(cpuUserData.begin(), cpuUserData.begin() + static_cast<std::ptrdiff_t>(cpuUserData.size() - alignedCount));
        }
        if (cpuSystemData.size() > alignedCount)
        {
            cpuSystemData.erase(cpuSystemData.begin(),
                                cpuSystemData.begin() + static_cast<std::ptrdiff_t>(cpuSystemData.size() - alignedCount));
        }

        const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
        std::vector<float> cpuTimeData = buildTimeAxis(timestamps, alignedCount, nowSeconds);

        NowBar cpuTotalNow{.valueText = UI::Format::percentCompact(cpuClamped),
                           .value01 = static_cast<float>(cpuClamped / 100.0),
                           .color = theme.progressColor(cpuClamped)};
        NowBar cpuUserNow{.valueText = UI::Format::percentCompact(proc.cpuUserPercent),
                          .value01 = static_cast<float>(std::clamp(proc.cpuUserPercent, 0.0, 100.0) / 100.0),
                          .color = theme.scheme().cpuUser};
        NowBar cpuSystemNow{.valueText = UI::Format::percentCompact(proc.cpuSystemPercent),
                            .value01 = static_cast<float>(std::clamp(proc.cpuSystemPercent, 0.0, 100.0) / 100.0),
                            .color = theme.scheme().cpuSystem};

        std::vector<float> cpuTotalSeries(cpuData.begin(), cpuData.end());
        std::vector<float> cpuUserSeries(cpuUserData.begin(), cpuUserData.end());
        std::vector<float> cpuSystemSeries(cpuSystemData.begin(), cpuSystemData.end());

        auto cpuPlot = [&]()
        {
            if (ImPlot::BeginPlot("##ProcOverviewCPU", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus))
            {
                ImPlot::SetupAxes("Time (s)", "%", X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

                if (alignedCount > 0)
                {
                    std::vector<float> y0(alignedCount, 0.0F);
                    std::vector<float> yUserTop(alignedCount);
                    std::vector<float> ySystemTop(alignedCount);

                    for (size_t i = 0; i < alignedCount; ++i)
                    {
                        yUserTop[i] = static_cast<float>(cpuUserData[i]);
                        ySystemTop[i] = static_cast<float>(cpuUserData[i] + cpuSystemData[i]);
                    }

                    ImVec4 userFill = theme.scheme().cpuUser;
                    userFill.w = 0.35F;
                    ImPlot::SetNextFillStyle(userFill);
                    ImPlot::PlotShaded("##CpuUser", cpuTimeData.data(), y0.data(), yUserTop.data(), static_cast<int>(alignedCount));

                    ImVec4 systemFill = theme.scheme().cpuSystem;
                    systemFill.w = 0.35F;
                    ImPlot::SetNextFillStyle(systemFill);
                    ImPlot::PlotShaded(
                        "##CpuSystem", cpuTimeData.data(), yUserTop.data(), ySystemTop.data(), static_cast<int>(alignedCount));

                    ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                    ImPlot::PlotLine("Total", cpuTimeData.data(), cpuTotalSeries.data(), static_cast<int>(alignedCount));

                    ImPlot::SetNextLineStyle(theme.scheme().cpuUser, 1.8F);
                    ImPlot::PlotLine("User", cpuTimeData.data(), cpuUserSeries.data(), static_cast<int>(alignedCount));

                    ImPlot::SetNextLineStyle(theme.scheme().cpuSystem, 1.8F);
                    ImPlot::PlotLine("System", cpuTimeData.data(), cpuSystemSeries.data(), static_cast<int>(alignedCount));

                    if (ImPlot::IsPlotHovered())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        const int idx = hoveredIndexFromPlotX(cpuTimeData, mouse.x);
                        const auto idxVal = static_cast<size_t>(idx);
                        if (idx >= 0 && idxVal < alignedCount)
                        {
                            ImGui::BeginTooltip();
                            ImGui::Text("t: %.1fs", static_cast<double>(cpuTimeData[static_cast<size_t>(idx)]));
                            ImGui::Separator();
                            const double totalValue = cpuData[static_cast<size_t>(idx)];
                            const ImVec4 totalColor = theme.progressColor(totalValue);
                            ImGui::TextColored(
                                totalColor, "Total: %s", UI::Format::percentCompact(static_cast<double>(totalValue)).c_str());
                            ImGui::TextColored(
                                theme.scheme().cpuUser,
                                "User: %s",
                                UI::Format::percentCompact(static_cast<double>(cpuUserData[static_cast<size_t>(idx)])).c_str());
                            ImGui::TextColored(
                                theme.scheme().cpuSystem,
                                "System: %s",
                                UI::Format::percentCompact(static_cast<double>(cpuSystemData[static_cast<size_t>(idx)])).c_str());
                            ImGui::EndTooltip();
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
            std::vector<double> timestamps(m_Timestamps.begin(), m_Timestamps.end());
            std::vector<double> usedData(m_MemoryHistory.begin(), m_MemoryHistory.end());
            std::vector<double> sharedData(m_SharedHistory.begin(), m_SharedHistory.end());
            std::vector<double> virtData(m_VirtualHistory.begin(), m_VirtualHistory.end());

            auto trimToAligned = [alignedCount](std::vector<double>& data)
            {
                if (data.size() > alignedCount)
                {
                    data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(data.size() - alignedCount));
                }
            };

            trimToAligned(timestamps);
            trimToAligned(usedData);
            trimToAligned(sharedData);
            trimToAligned(virtData);

            const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, 0.0);
            std::vector<float> timeData = buildTimeAxis(timestamps, alignedCount, nowSeconds);
            std::vector<float> usedSeries(usedData.begin(), usedData.end());
            std::vector<float> sharedSeries(sharedData.begin(), sharedData.end());
            std::vector<float> virtSeries(virtData.begin(), virtData.end());

            const double usedNow = usedData.empty() ? 0.0 : usedData.back();
            const double sharedNow = sharedData.empty() ? 0.0 : sharedData.back();
            const double virtNowVal = virtData.empty() ? 0.0 : virtData.back();

            std::vector<NowBar> memoryBars;
            memoryBars.push_back({.valueText = UI::Format::percentCompact(usedNow),
                                  .value01 = static_cast<float>(std::clamp(usedNow, 0.0, 100.0) / 100.0),
                                  .color = theme.scheme().chartMemory});
            memoryBars.push_back({.valueText = UI::Format::percentCompact(sharedNow),
                                  .value01 = static_cast<float>(std::clamp(sharedNow, 0.0, 100.0) / 100.0),
                                  .color = theme.scheme().chartCpu});
            memoryBars.push_back({.valueText = UI::Format::percentCompact(virtNowVal),
                                  .value01 = static_cast<float>(std::clamp(virtNowVal, 0.0, 100.0) / 100.0),
                                  .color = theme.scheme().chartIo});

            auto memoryPlot = [&]()
            {
                if (ImPlot::BeginPlot(
                        "##ProcOverviewMemory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus))
                {
                    ImPlot::SetupAxes("Time (s)", "%", X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

                    if (!usedSeries.empty())
                    {
                        ImPlot::SetNextLineStyle(theme.scheme().chartMemory, 2.0F);
                        ImPlot::PlotLine("Used", timeData.data(), usedSeries.data(), static_cast<int>(usedSeries.size()));
                    }

                    if (!sharedSeries.empty())
                    {
                        ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                        ImPlot::PlotLine("Shared", timeData.data(), sharedSeries.data(), static_cast<int>(sharedSeries.size()));
                    }

                    if (!virtSeries.empty())
                    {
                        ImPlot::SetNextLineStyle(theme.scheme().chartIo, 2.0F);
                        ImPlot::PlotLine("Virtual", timeData.data(), virtSeries.data(), static_cast<int>(virtSeries.size()));
                    }

                    if (ImPlot::IsPlotHovered())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        const int idx = hoveredIndexFromPlotX(timeData, mouse.x);
                        if (idx >= 0)
                        {
                            const auto idxVal = static_cast<size_t>(idx);
                            ImGui::BeginTooltip();
                            ImGui::Text("t: %.1fs", static_cast<double>(timeData[static_cast<size_t>(idx)]));
                            if (idxVal < usedSeries.size())
                            {
                                ImGui::TextColored(
                                    theme.scheme().chartMemory,
                                    "Used: %s",
                                    UI::Format::percentCompact(static_cast<double>(usedSeries[static_cast<size_t>(idx)])).c_str());
                            }
                            if (idxVal < sharedSeries.size())
                            {
                                ImGui::TextColored(
                                    theme.scheme().chartCpu,
                                    "Shared: %s",
                                    UI::Format::percentCompact(static_cast<double>(sharedSeries[static_cast<size_t>(idx)])).c_str());
                            }
                            if (idxVal < virtSeries.size())
                            {
                                ImGui::TextColored(
                                    theme.scheme().chartIo,
                                    "Virtual: %s",
                                    UI::Format::percentCompact(static_cast<double>(virtSeries[static_cast<size_t>(idx)])).c_str());
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
