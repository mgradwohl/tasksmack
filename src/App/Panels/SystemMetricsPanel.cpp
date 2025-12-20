#include "SystemMetricsPanel.h"

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
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <vector>

namespace App
{

namespace
{

using UI::Widgets::computeAlpha;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::PLOT_FLAGS_DEFAULT;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

void drawProgressBarWithOverlay(float fraction, const std::string& overlay, const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), "");
    UI::Widgets::drawRightAlignedOverlayText(overlay.c_str());
    ImGui::PopStyleColor();
}

template<typename T> void cropFrontToSize(std::vector<T>& data, size_t targetSize)
{
    if (data.size() > targetSize)
    {
        data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(data.size() - targetSize));
    }
}

struct HistoryRange
{
    size_t start = 0;
    size_t count = 0;
};

using UI::Widgets::buildTimeAxis;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::renderHistoryWithNowBars;

void showCpuBreakdownTooltip(const UI::ColorScheme& scheme,
                             bool showTime,
                             int timeSec,
                             double userPercent,
                             double systemPercent,
                             double iowaitPercent,
                             double idlePercent)
{
    ImGui::BeginTooltip();
    if (showTime)
    {
        ImGui::Text("t: %ds", timeSec);
        ImGui::Separator();
    }
    ImGui::TextColored(scheme.cpuUser, "User: %s", UI::Format::percentCompact(userPercent).c_str());
    ImGui::TextColored(scheme.cpuSystem, "System: %s", UI::Format::percentCompact(systemPercent).c_str());
    ImGui::TextColored(scheme.cpuIowait, "I/O Wait: %s", UI::Format::percentCompact(iowaitPercent).c_str());
    ImGui::TextColored(scheme.cpuIdle, "Idle: %s", UI::Format::percentCompact(idlePercent).c_str());
    ImGui::EndTooltip();
}

} // namespace

SystemMetricsPanel::SystemMetricsPanel() : Panel("System")
{
}

SystemMetricsPanel::~SystemMetricsPanel()
{
    m_Model.reset();
}

void SystemMetricsPanel::onAttach()
{
    auto& settings = UserConfig::get().settings();
    m_RefreshInterval = std::chrono::milliseconds(settings.refreshIntervalMs);
    m_MaxHistorySeconds = static_cast<double>(settings.maxHistorySeconds);
    m_HistoryScrollSeconds = 0.0;
    m_RefreshAccumulatorSec = 0.0F;
    m_ForceRefresh = true;

    m_Model = std::make_unique<Domain::SystemModel>(Platform::makeSystemProbe());
    m_Model->setMaxHistorySeconds(m_MaxHistorySeconds);

    // Initial refresh to seed histories
    m_Model->refresh();
    m_TimestampsCache = m_Model->timestamps();
    if (!m_TimestampsCache.empty())
    {
        m_CurrentNowSeconds = m_TimestampsCache.back();
    }
    else
    {
        m_CurrentNowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    m_ForceRefresh = false;

    const auto initialSnap = m_Model->snapshot();
    m_Hostname = initialSnap.hostname.empty() ? "System" : initialSnap.hostname;
}

void SystemMetricsPanel::onDetach()
{
    m_Model.reset();
}

void SystemMetricsPanel::setSamplingInterval(std::chrono::milliseconds interval)
{
    m_RefreshInterval = interval;
    m_RefreshAccumulatorSec = 0.0F;
    m_ForceRefresh = true;
}

void SystemMetricsPanel::requestRefresh()
{
    m_ForceRefresh = true;
}

void SystemMetricsPanel::onUpdate(float deltaTime)
{
    m_LastDeltaSeconds = deltaTime;

    if (!m_Model)
    {
        return;
    }

    m_RefreshAccumulatorSec += deltaTime;
    const float intervalSec = static_cast<float>(m_RefreshInterval.count()) / 1000.0F;
    const bool intervalElapsed = (intervalSec > 0.0F) && (m_RefreshAccumulatorSec >= intervalSec);

    if (m_ForceRefresh || intervalElapsed)
    {
        m_Model->setMaxHistorySeconds(m_MaxHistorySeconds);
        m_Model->refresh();
        m_TimestampsCache = m_Model->timestamps();
        if (!m_TimestampsCache.empty())
        {
            m_CurrentNowSeconds = m_TimestampsCache.back();
        }
        else
        {
            m_CurrentNowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        m_ForceRefresh = false;

        const auto snap = m_Model->snapshot();
        if (!snap.hostname.empty())
        {
            m_Hostname = snap.hostname;
        }

        if (intervalSec > 0.0F)
        {
            while (m_RefreshAccumulatorSec >= intervalSec)
            {
                m_RefreshAccumulatorSec -= intervalSec;
            }
        }
        else
        {
            m_RefreshAccumulatorSec = 0.0F;
        }
    }
}

void SystemMetricsPanel::render(bool* open)
{
    if (!ImGui::Begin(m_Hostname.c_str(), open))
    {
        ImGui::End();
        return;
    }

    if (!m_Model)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textError, "System model not initialized");
        ImGui::End();
        return;
    }

    const auto& theme = UI::Theme::get();
    if (theme.currentFontSize() != m_LastFontSize)
    {
        m_LastFontSize = theme.currentFontSize();
        m_LayoutDirty = true;
    }

    auto snap = m_Model->snapshot();
    const size_t coreCount = static_cast<size_t>(snap.coreCount);
    if (coreCount != m_LastCoreCount)
    {
        m_LastCoreCount = coreCount;
        m_LayoutDirty = true;
    }

    if (m_LayoutDirty)
    {
        updateCachedLayout();
        m_LayoutDirty = false;
    }

    if (ImGui::BeginTabBar("SystemTabs"))
    {
        if (ImGui::BeginTabItem("Overview"))
        {
            renderOverview();
            ImGui::EndTabItem();
        }

        if (snap.coreCount > 1)
        {
            if (ImGui::BeginTabItem("CPU Cores"))
            {
                renderPerCoreSection();
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void SystemMetricsPanel::renderOverview()
{
    auto snap = m_Model->snapshot();

    updateSmoothedCpu(snap, m_LastDeltaSeconds);
    updateSmoothedMemory(snap, m_LastDeltaSeconds);

    // Header line: CPU Model | Cores | Freq | Uptime (right-aligned)
    // Format uptime string
    std::string uptimeStr;
    if (snap.uptimeSeconds > 0)
    {
        uint64_t days = snap.uptimeSeconds / 86400;
        uint64_t hours = (snap.uptimeSeconds % 86400) / 3600;
        uint64_t minutes = (snap.uptimeSeconds % 3600) / 60;

        if (days > 0)
        {
            uptimeStr = std::format("Up: {}d {}h {}m", days, hours, minutes);
        }
        else if (hours > 0)
        {
            uptimeStr = std::format("Up: {}h {}m", hours, minutes);
        }
        else
        {
            uptimeStr = std::format("Up: {}m", minutes);
        }
    }

    // Display: "CPU Model (N cores @ X.XX GHz)     Uptime: Xd Yh Zm"
    std::string coreInfo;
    if (snap.cpuFreqMHz > 0)
    {
        coreInfo = std::format(" ({} cores @ {:.2f} GHz)", snap.coreCount, static_cast<double>(snap.cpuFreqMHz) / 1000.0);
    }
    else
    {
        coreInfo = std::format(" ({} cores)", snap.coreCount);
    }

    float availWidth = ImGui::GetContentRegionAvail().x;
    float uptimeWidth = ImGui::CalcTextSize(uptimeStr.c_str()).x;

    // CPU model with core count and frequency
    ImGui::TextUnformatted(snap.cpuModel.c_str());
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(coreInfo.c_str());

    // Right-align uptime
    if (!uptimeStr.empty())
    {
        ImGui::SameLine(availWidth - uptimeWidth);
        ImGui::TextUnformatted(uptimeStr.c_str());
    }

    ImGui::Spacing();

    // Get theme for colored progress bars
    auto& theme = UI::Theme::get();

    auto cpuHist = m_Model->cpuHistory();
    auto cpuUserHist = m_Model->cpuUserHistory();
    auto cpuSystemHist = m_Model->cpuSystemHistory();
    auto cpuIowaitHist = m_Model->cpuIowaitHistory();
    auto cpuIdleHist = m_Model->cpuIdleHistory();
    const auto& timestamps = m_TimestampsCache;
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, m_HistoryScrollSeconds);

    const size_t cpuCount = std::min(cpuHist.size(), timestamps.size());
    // CPU history with vertical now bars (total + breakdown)
    ImGui::Text("CPU Usage (last %zu samples)", cpuCount);

    cropFrontToSize(cpuHist, cpuCount);
    std::vector<float> cpuTimeData = buildTimeAxis(timestamps, cpuCount, nowSeconds);

    const size_t breakdownCount =
        std::min({cpuUserHist.size(), cpuSystemHist.size(), cpuIowaitHist.size(), cpuIdleHist.size(), timestamps.size()});
    cropFrontToSize(cpuUserHist, breakdownCount);
    cropFrontToSize(cpuSystemHist, breakdownCount);
    cropFrontToSize(cpuIowaitHist, breakdownCount);
    cropFrontToSize(cpuIdleHist, breakdownCount);
    std::vector<float> breakdownTimeData = buildTimeAxis(timestamps, breakdownCount, nowSeconds);

    auto cpuPlot = [&]()
    {
        if (ImPlot::BeginPlot("##OverviewCPUHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT))
        {
            ImPlot::SetupAxes("Time (s)", "%", X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

            if (breakdownCount > 0)
            {
                std::vector<float> y0(breakdownCount, 0.0F);
                std::vector<float> yUserTop(breakdownCount);
                std::vector<float> ySystemTop(breakdownCount);
                std::vector<float> yIowaitTop(breakdownCount);
                std::vector<float> yTotalTop(breakdownCount);

                for (size_t i = 0; i < breakdownCount; ++i)
                {
                    yUserTop[i] = cpuUserHist[i];
                    ySystemTop[i] = cpuUserHist[i] + cpuSystemHist[i];
                    yIowaitTop[i] = ySystemTop[i] + cpuIowaitHist[i];
                    yTotalTop[i] = yIowaitTop[i] + cpuIdleHist[i];
                }

                ImVec4 userFill = theme.scheme().cpuUser;
                userFill.w = 0.35F;
                ImPlot::SetNextFillStyle(userFill);
                ImPlot::PlotShaded("##CpuUser", breakdownTimeData.data(), y0.data(), yUserTop.data(), static_cast<int>(breakdownCount));

                ImVec4 systemFill = theme.scheme().cpuSystem;
                systemFill.w = 0.35F;
                ImPlot::SetNextFillStyle(systemFill);
                ImPlot::PlotShaded(
                    "##CpuSystem", breakdownTimeData.data(), yUserTop.data(), ySystemTop.data(), static_cast<int>(breakdownCount));

                ImVec4 iowaitFill = theme.scheme().cpuIowait;
                iowaitFill.w = 0.35F;
                ImPlot::SetNextFillStyle(iowaitFill);
                ImPlot::PlotShaded(
                    "##CpuIowait", breakdownTimeData.data(), ySystemTop.data(), yIowaitTop.data(), static_cast<int>(breakdownCount));

                ImVec4 idleFill = theme.scheme().cpuIdle;
                idleFill.w = 0.20F;
                ImPlot::SetNextFillStyle(idleFill);
                ImPlot::PlotShaded(
                    "##CpuIdle", breakdownTimeData.data(), yIowaitTop.data(), yTotalTop.data(), static_cast<int>(breakdownCount));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    const int idx = hoveredIndexFromPlotX(breakdownTimeData, mouse.x);
                    if (idx >= 0)
                    {
                        const size_t si = static_cast<size_t>(idx);
                        showCpuBreakdownTooltip(theme.scheme(),
                                                true,
                                                static_cast<int>(std::round(breakdownTimeData[si])),
                                                static_cast<double>(cpuUserHist[si]),
                                                static_cast<double>(cpuSystemHist[si]),
                                                static_cast<double>(cpuIowaitHist[si]),
                                                static_cast<double>(cpuIdleHist[si]));
                    }
                }
            }
            else if (!cpuHist.empty())
            {
                ImVec4 fillColor = theme.scheme().chartCpu;
                fillColor.w = 0.3F;
                ImPlot::SetNextFillStyle(fillColor);
                ImPlot::PlotShaded("##CPUShaded", cpuTimeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()), 0.0);

                ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                ImPlot::PlotLine("##CPU", cpuTimeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    const int idx = hoveredIndexFromPlotX(cpuTimeData, mouse.x);
                    if (idx >= 0)
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("t: %.1fs", static_cast<double>(cpuTimeData[static_cast<size_t>(idx)]));
                        ImGui::Text("CPU: %s", UI::Format::percentCompact(static_cast<double>(cpuHist[static_cast<size_t>(idx)])).c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
            else
            {
                ImPlot::PlotDummy("##CPU");
            }

            ImPlot::EndPlot();
        }
    };

    std::vector<NowBar> cpuBars;
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.total),
                       .value01 = static_cast<float>(std::clamp(m_SmoothedCpu.total, 0.0, 100.0) / 100.0),
                       .color = theme.progressColor(m_SmoothedCpu.total)});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.user),
                       .value01 = static_cast<float>(std::clamp(m_SmoothedCpu.user, 0.0, 100.0) / 100.0),
                       .color = theme.scheme().cpuUser});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.system),
                       .value01 = static_cast<float>(std::clamp(m_SmoothedCpu.system, 0.0, 100.0) / 100.0),
                       .color = theme.scheme().cpuSystem});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.iowait),
                       .value01 = static_cast<float>(std::clamp(m_SmoothedCpu.iowait, 0.0, 100.0) / 100.0),
                       .color = theme.scheme().cpuIowait});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.idle),
                       .value01 = static_cast<float>(std::clamp(m_SmoothedCpu.idle, 0.0, 100.0) / 100.0),
                       .color = theme.scheme().cpuIdle});

    constexpr size_t OVERVIEW_NOW_BAR_COLUMNS = 5; // Keep CPU and Memory charts aligned
    renderHistoryWithNowBars("OverviewCPUHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, cpuPlot, cpuBars, false, OVERVIEW_NOW_BAR_COLUMNS);

    ImGui::Spacing();

    // Memory & Swap history (moved from Memory tab)
    {
        auto memHist = m_Model->memoryHistory();
        auto cachedHist = m_Model->memoryCachedHistory();
        auto swapHist = m_Model->swapHistory();

        ImGui::Text("Memory & Swap (last %zu samples)", std::min(memHist.size(), timestamps.size()));
        ImGui::Spacing();

        const size_t memCount = std::min(memHist.size(), timestamps.size());
        const size_t cachedCount = std::min(cachedHist.size(), timestamps.size());
        const size_t swapCount = std::min(swapHist.size(), timestamps.size());

        size_t alignedCount = memCount;
        if (cachedCount > 0)
        {
            alignedCount = std::min(alignedCount, cachedCount);
        }
        if (swapCount > 0)
        {
            alignedCount = std::min(alignedCount, swapCount);
        }

        cropFrontToSize(memHist, alignedCount);
        cropFrontToSize(cachedHist, std::min(cachedCount, alignedCount));
        cropFrontToSize(swapHist, std::min(swapCount, alignedCount));
        std::vector<float> timeData = buildTimeAxis(timestamps, alignedCount, nowSeconds);

        auto memoryPlot = [&]()
        {
            if (ImPlot::BeginPlot(
                    "##MemorySwapHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT | ImPlotFlags_NoLegend))
            {
                ImPlot::SetupAxes("Time (s)", "%", X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

                if (!memHist.empty())
                {
                    ImPlot::SetNextLineStyle(theme.scheme().chartMemory, 2.0F);
                    ImPlot::PlotLine("Used", timeData.data(), memHist.data(), static_cast<int>(memHist.size()));
                }

                if (!cachedHist.empty())
                {
                    ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                    ImPlot::PlotLine("Cached", timeData.data(), cachedHist.data(), static_cast<int>(cachedHist.size()));
                }

                if (!swapHist.empty())
                {
                    ImPlot::SetNextLineStyle(theme.scheme().chartIo, 2.0F);
                    ImPlot::PlotLine("Swap", timeData.data(), swapHist.data(), static_cast<int>(swapHist.size()));
                }

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    const int idx = hoveredIndexFromPlotX(timeData, mouse.x);
                    if (idx >= 0)
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("t: %.1fs", static_cast<double>(timeData[static_cast<size_t>(idx)]));
                        if (idx < (static_cast<int>(memHist.size())))
                        {
                            ImGui::TextColored(theme.scheme().chartMemory,
                                               "Used: %s",
                                               UI::Format::percentCompact(static_cast<double>(memHist[static_cast<size_t>(idx)])).c_str());
                        }
                        const auto idxVal = static_cast<size_t>(idx);
                        if (idxVal < cachedHist.size())
                        {
                            ImGui::TextColored(theme.scheme().chartCpu,
                                               "Cached: %s",
                                               UI::Format::percentCompact(static_cast<double>(cachedHist[idxVal])).c_str());
                        }
                        if (idxVal < swapHist.size())
                        {
                            ImGui::TextColored(theme.scheme().chartIo,
                                               "Swap: %s",
                                               UI::Format::percentCompact(static_cast<double>(swapHist[idxVal])).c_str());
                        }
                        ImGui::EndTooltip();
                    }
                }

                ImPlot::EndPlot();
            }
        };

        std::vector<NowBar> memoryBars;
        if (snap.memoryTotalBytes > 0)
        {
            const double usedPercentClamped = std::clamp(m_SmoothedMemory.usedPercent, 0.0, 100.0);
            memoryBars.push_back({.valueText = UI::Format::percentCompact(usedPercentClamped),
                                  .value01 = static_cast<float>(usedPercentClamped / 100.0),
                                  .color = theme.scheme().chartMemory});

            const double rawCachedPercent =
                static_cast<double>(snap.memoryCachedBytes) / static_cast<double>(snap.memoryTotalBytes) * 100.0;
            const double cachedPercent = std::clamp(rawCachedPercent, 0.0, 100.0);
            memoryBars.push_back({.valueText = UI::Format::percentCompact(cachedPercent),
                                  .value01 = static_cast<float>(cachedPercent / 100.0),
                                  .color = theme.scheme().chartCpu});
        }

        if (snap.swapTotalBytes > 0)
        {
            const double swapPercentClamped = std::clamp(m_SmoothedMemory.swapPercent, 0.0, 100.0);
            memoryBars.push_back({.valueText = UI::Format::percentCompact(swapPercentClamped),
                                  .value01 = static_cast<float>(swapPercentClamped / 100.0),
                                  .color = theme.scheme().chartIo});
        }

        renderHistoryWithNowBars(
            "MemorySwapHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, memoryPlot, memoryBars, false, OVERVIEW_NOW_BAR_COLUMNS);

        ImGui::Spacing();
    }

    // Load average (Linux only, shows nothing on Windows)
    if (snap.loadAvg1 > 0.0 || snap.loadAvg5 > 0.0 || snap.loadAvg15 > 0.0)
    {
        ImGui::Text("Load Avg:");
        ImGui::SameLine(m_OverviewLabelWidth);

        const std::string loadStr = std::format("{:.2f} / {:.2f} / {:.2f} (1/5/15 min)", snap.loadAvg1, snap.loadAvg5, snap.loadAvg15);
        ImGui::TextUnformatted(loadStr.c_str());
    }

    // Memory usage bar with themed color
    ImGui::Text("Memory:");
    ImGui::SameLine(m_OverviewLabelWidth);
    float memFraction = static_cast<float>(m_SmoothedMemory.usedPercent) / 100.0F;

    const std::string memOverlay =
        UI::Format::bytesUsedTotalPercentCompact(snap.memoryUsedBytes, snap.memoryTotalBytes, m_SmoothedMemory.usedPercent);
    ImVec4 memColor = theme.progressColor(m_SmoothedMemory.usedPercent);
    drawProgressBarWithOverlay(memFraction, memOverlay, memColor);

    // Swap usage bar (if available) with themed color
    if (snap.swapTotalBytes > 0)
    {
        ImGui::Text("Swap:");
        ImGui::SameLine(m_OverviewLabelWidth);
        float swapFraction = static_cast<float>(m_SmoothedMemory.swapPercent) / 100.0F;

        const std::string swapOverlay =
            UI::Format::bytesUsedTotalPercentCompact(snap.swapUsedBytes, snap.swapTotalBytes, m_SmoothedMemory.swapPercent);
        ImVec4 swapColor = theme.progressColor(m_SmoothedMemory.swapPercent);
        drawProgressBarWithOverlay(swapFraction, swapOverlay, swapColor);
    }
}

void SystemMetricsPanel::renderCpuSection()
{
    const auto& theme = UI::Theme::get();
    auto snap = m_Model->snapshot();
    auto cpuHist = m_Model->cpuHistory();
    auto cpuUserHist = m_Model->cpuUserHistory();
    auto cpuSystemHist = m_Model->cpuSystemHistory();
    auto cpuIowaitHist = m_Model->cpuIowaitHistory();
    auto cpuIdleHist = m_Model->cpuIdleHistory();
    const auto& timestamps = m_TimestampsCache;
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, m_HistoryScrollSeconds);

    ImGui::Text("CPU History (last %zu samples)", cpuHist.size());
    ImGui::Spacing();

    const size_t timeCount = std::min(cpuHist.size(), timestamps.size());
    cropFrontToSize(cpuHist, timeCount);
    std::vector<float> timeData = buildTimeAxis(timestamps, timeCount, nowSeconds);

    const size_t breakdownCount =
        std::min({cpuUserHist.size(), cpuSystemHist.size(), cpuIowaitHist.size(), cpuIdleHist.size(), timestamps.size()});
    cropFrontToSize(cpuUserHist, breakdownCount);
    cropFrontToSize(cpuSystemHist, breakdownCount);
    cropFrontToSize(cpuIowaitHist, breakdownCount);
    cropFrontToSize(cpuIdleHist, breakdownCount);

    // CPU Usage Plot
    if (ImPlot::BeginPlot("##CPUHistory", ImVec2(-1, 200), PLOT_FLAGS_DEFAULT))
    {
        ImPlot::SetupAxes("Time (s)", "%", X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

        if (!cpuHist.empty())
        {
            ImVec4 fillColor = theme.scheme().chartCpu;
            fillColor.w = 0.3F; // Semi-transparent fill
            ImPlot::SetNextFillStyle(fillColor);
            ImPlot::PlotShaded("##CPUShaded", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()), 0.0);

            // Draw the line on top of the shaded region.
            ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
            ImPlot::PlotLine("##CPU", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()));

            if (ImPlot::IsPlotHovered())
            {
                const size_t n = cpuHist.size();
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                const int idx = hoveredIndexFromPlotX(timeData, mouse.x);
                if (idx >= 0)
                {
                    const auto idxVal = static_cast<size_t>(idx);
                    const float timeSec = timeData[idxVal];
                    if ((breakdownCount == n) && (idxVal < breakdownCount))
                    {
                        const size_t si = idxVal;
                        showCpuBreakdownTooltip(theme.scheme(),
                                                true,
                                                static_cast<int>(std::round(timeSec)),
                                                static_cast<double>(cpuUserHist[si]),
                                                static_cast<double>(cpuSystemHist[si]),
                                                static_cast<double>(cpuIowaitHist[si]),
                                                static_cast<double>(cpuIdleHist[si]));
                    }
                    else
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("t: %.1fs", static_cast<double>(timeSec));
                        ImGui::Text("CPU: %s", UI::Format::percentCompact(static_cast<double>(cpuHist[idxVal])).c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
        }
        else
        {
            ImPlot::PlotDummy("##CPU");
        }

        ImPlot::EndPlot();
    }

    ImGui::Spacing();

    // Current values
    ImGui::Text("Current: %.1f%% (User: %.1f%%, System: %.1f%%)", m_SmoothedCpu.total, m_SmoothedCpu.user, m_SmoothedCpu.system);
}

void SystemMetricsPanel::renderPerCoreSection()
{
    auto snap = m_Model->snapshot();
    auto perCoreHist = m_Model->perCoreHistory();
    auto& theme = UI::Theme::get();
    ImGui::Text("Per-Core CPU Usage (%d cores)", snap.coreCount);
    ImGui::Spacing();

    const size_t numCores = snap.cpuPerCore.size();
    if (numCores == 0)
    {
        ImGui::TextColored(theme.scheme().textMuted, "No per-core data available");
        return;
    }

    updateSmoothedPerCore(snap, m_LastDeltaSeconds);

    // ========================================
    // Per-core history grid with vertical now bars
    // ========================================
    // Removed redundant heading to reduce clutter

    const auto& timestamps = m_TimestampsCache;
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, m_HistoryScrollSeconds);

    if (perCoreHist.empty() || timestamps.empty())
    {
        ImGui::TextColored(theme.scheme().textMuted, "Collecting data...");
        return;
    }

    const size_t coreCount = perCoreHist.size();

    // Grid layout
    {
        float gridWidth = ImGui::GetContentRegionAvail().x;
        constexpr float minCellWidth = 240.0F;
        const float barWidth = ImGui::GetFrameHeight(); // Match prior horizontal bar height for visual consistency
        const float cellWidth = minCellWidth + barWidth;
        int gridCols = std::max(1, static_cast<int>(gridWidth / cellWidth));
        int gridRows = static_cast<int>((coreCount + static_cast<size_t>(gridCols) - 1) / static_cast<size_t>(gridCols));

        if (ImGui::BeginTable("PerCoreGrid", gridCols, ImGuiTableFlags_SizingStretchSame))
        {
            for (int row = 0; row < gridRows; ++row)
            {
                ImGui::TableNextRow();
                for (int col = 0; col < gridCols; ++col)
                {
                    const auto coreIdx =
                        (static_cast<std::size_t>(row) * static_cast<std::size_t>(gridCols)) + static_cast<std::size_t>(col);
                    ImGui::TableNextColumn();

                    if (coreIdx >= coreCount)
                    {
                        continue;
                    }

                    const auto& samples = perCoreHist[coreIdx];
                    if (samples.empty())
                    {
                        ImGui::TextColored(theme.scheme().textMuted, "Core %zu\nCollecting data...", coreIdx);
                        continue;
                    }

                    const std::string coreLabel = std::format("Core {}", coreIdx);

                    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
                    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);
                    const std::string childId = std::format("CoreCell{}", coreIdx);
                    const float labelHeight = ImGui::GetTextLineHeight();
                    const float spacingY = ImGui::GetStyle().ItemSpacing.y;
                    const float childHeight =
                        labelHeight + spacingY + HISTORY_PLOT_HEIGHT_DEFAULT + UI::Widgets::BAR_WIDTH + (spacingY * 2.0F);
                    const bool childOpen = ImGui::BeginChild(childId.c_str(), ImVec2(-FLT_MIN, childHeight), ImGuiChildFlags_Borders);
                    if (childOpen)
                    {
                        const float availableWidth = ImGui::GetContentRegionAvail().x;
                        const float labelWidth = ImGui::CalcTextSize(coreLabel.c_str()).x;
                        const float labelOffset = std::max(0.0F, (availableWidth - labelWidth) * 0.5F);
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelOffset);
                        ImGui::TextUnformatted(coreLabel.c_str());
                        ImGui::Spacing();

                        std::vector<float> timeData = buildTimeAxis(timestamps, samples.size(), nowSeconds);
                        auto plotFn = [&]()
                        {
                            if (ImPlot::BeginPlot("##PerCorePlot", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT))
                            {
                                ImPlot::SetupAxes("Time (s)", "%", X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
                                ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

                                ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                                ImPlot::PlotLine("##Core", timeData.data(), samples.data(), static_cast<int>(timeData.size()));
                                ImPlot::EndPlot();
                            }
                        };

                        double smoothed =
                            (coreIdx < m_SmoothedPerCore.size()) ? m_SmoothedPerCore[coreIdx] : snap.cpuPerCore[coreIdx].totalPercent;
                        NowBar bar{.valueText = UI::Format::percentCompact(smoothed),
                                   .value01 = static_cast<float>(smoothed) / 100.0F,
                                   .color = theme.progressColor(smoothed)};

                        std::vector<NowBar> bars;
                        bars.push_back(bar);
                        const std::string tableId = std::format("CoreLayout{}", coreIdx);
                        renderHistoryWithNowBars(tableId.c_str(), HISTORY_PLOT_HEIGHT_DEFAULT, plotFn, bars, false, 0, true);
                    }
                    if (childOpen)
                    {
                        ImGui::EndChild();
                    }
                    ImGui::PopStyleColor(2);
                }
            }
            ImGui::EndTable();
        }
    }
}

void SystemMetricsPanel::updateSmoothedCpu(const Domain::SystemSnapshot& snap, float deltaTimeSeconds)
{
    auto clampPercent = [](double value)
    {
        return std::clamp(value, 0.0, 100.0);
    };

    const double alpha = computeAlpha(static_cast<double>(deltaTimeSeconds), m_RefreshInterval);

    const double targetTotal = clampPercent(snap.cpuTotal.totalPercent);
    const double targetUser = clampPercent(snap.cpuTotal.userPercent);
    const double targetSystem = clampPercent(snap.cpuTotal.systemPercent);
    const double targetIowait = clampPercent(snap.cpuTotal.iowaitPercent);
    const double targetIdle = clampPercent(snap.cpuTotal.idlePercent);

    if (!m_SmoothedCpu.initialized)
    {
        m_SmoothedCpu.total = targetTotal;
        m_SmoothedCpu.user = targetUser;
        m_SmoothedCpu.system = targetSystem;
        m_SmoothedCpu.iowait = targetIowait;
        m_SmoothedCpu.idle = targetIdle;
        m_SmoothedCpu.initialized = true;
        return;
    }

    auto step = [alpha](double current, double target)
    {
        return current + (alpha * (target - current));
    };

    m_SmoothedCpu.total = clampPercent(step(m_SmoothedCpu.total, targetTotal));
    m_SmoothedCpu.user = clampPercent(step(m_SmoothedCpu.user, targetUser));
    m_SmoothedCpu.system = clampPercent(step(m_SmoothedCpu.system, targetSystem));
    m_SmoothedCpu.iowait = clampPercent(step(m_SmoothedCpu.iowait, targetIowait));
    m_SmoothedCpu.idle = clampPercent(step(m_SmoothedCpu.idle, targetIdle));
}

void SystemMetricsPanel::updateSmoothedMemory(const Domain::SystemSnapshot& snap, float deltaTimeSeconds)
{
    auto clampPercent = [](double value)
    {
        return std::clamp(value, 0.0, 100.0);
    };

    const double alpha = computeAlpha(static_cast<double>(deltaTimeSeconds), m_RefreshInterval);

    const double targetMem = clampPercent(snap.memoryUsedPercent);
    const double targetSwap = clampPercent(snap.swapUsedPercent);

    if (!m_SmoothedMemory.initialized)
    {
        m_SmoothedMemory.usedPercent = targetMem;
        m_SmoothedMemory.swapPercent = targetSwap;
        m_SmoothedMemory.initialized = true;
        return;
    }

    m_SmoothedMemory.usedPercent = clampPercent(smoothTowards(m_SmoothedMemory.usedPercent, targetMem, alpha));
    m_SmoothedMemory.swapPercent = clampPercent(smoothTowards(m_SmoothedMemory.swapPercent, targetSwap, alpha));
}

void SystemMetricsPanel::updateSmoothedPerCore(const Domain::SystemSnapshot& snap, float deltaTimeSeconds)
{
    auto clampPercent = [](double value)
    {
        return std::clamp(value, 0.0, 100.0);
    };

    const double alpha = computeAlpha(static_cast<double>(deltaTimeSeconds), m_RefreshInterval);
    const size_t numCores = snap.cpuPerCore.size();
    m_SmoothedPerCore.resize(numCores, 0.0);

    for (size_t i = 0; i < numCores; ++i)
    {
        const double target = clampPercent(snap.cpuPerCore[i].totalPercent);
        double& current = m_SmoothedPerCore[i];
        if (deltaTimeSeconds <= 0.0F)
        {
            current = target;
            continue;
        }
        current = clampPercent(smoothTowards(current, target, alpha));
    }
}

void SystemMetricsPanel::updateCachedLayout()
{
    auto& theme = UI::Theme::get();

    // Overview: width needed for "CPU Usage:" label + spacing
    m_OverviewLabelWidth = ImGui::CalcTextSize("CPU Usage:").x + ImGui::GetStyle().ItemSpacing.x;

    // Per-core: width needed for max core number (e.g., "31" for 32 cores)
    if (m_LastCoreCount > 0)
    {
        const std::string maxLabel = std::format("{}", m_LastCoreCount - 1);
        m_PerCoreLabelWidth = ImGui::CalcTextSize(maxLabel.c_str()).x;
    }
    else
    {
        m_PerCoreLabelWidth = ImGui::CalcTextSize("0").x;
    }

    spdlog::debug("SystemMetricsPanel: cached layout updated (font={}, overviewWidth={:.1f}, perCoreWidth={:.1f})",
                  static_cast<int>(theme.currentFontSize()),
                  m_OverviewLabelWidth,
                  m_PerCoreLabelWidth);
}

} // namespace App
