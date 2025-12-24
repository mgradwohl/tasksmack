#include "SystemMetricsPanel.h"

#include "App/Panel.h"
#include "App/UserConfig.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/HistoryWidgets.h"
#include "UI/Numeric.h"
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
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

[[nodiscard]] std::string formatBytesPerSecond(double bytesPerSec)
{
    const auto bytes = static_cast<uint64_t>(std::max(0.0, bytesPerSec));
    const auto unit = UI::Format::unitForTotalBytes(bytes);
    return UI::Format::formatBytesWithUnit(bytes, unit);
}

void drawProgressBarWithOverlay(double fraction01, const std::string& overlay, const ImVec4& color)
{
    const double clamped = std::clamp(fraction01, 0.0, 1.0);
    const float fraction = UI::Numeric::toFloatNarrow(clamped); // Narrowing: ImGui::ProgressBar expects float in [0, 1]
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), "");
    UI::Widgets::drawRightAlignedOverlayText(overlay.c_str());
    ImGui::PopStyleColor();
}

template<typename T> void cropFrontToSize(std::vector<T>& data, size_t targetSize)
{
    if (data.size() > targetSize)
    {
        const size_t removeCount = data.size() - targetSize;
        using Diff = std::ptrdiff_t;
        const Diff removeCountDiff = UI::Numeric::narrowOr<Diff>(removeCount, std::numeric_limits<Diff>::min());
        if (removeCountDiff == std::numeric_limits<Diff>::min())
        {
            data.clear();
            return;
        }

        data.erase(data.begin(), data.begin() + removeCountDiff);
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

[[nodiscard]] int checkedRoundSeconds(float seconds)
{
    const long rounded = std::lroundf(seconds);
    return UI::Numeric::narrowOr<int>(rounded, std::numeric_limits<int>::max());
}

void showCpuBreakdownTooltip(const UI::ColorScheme& scheme,
                             bool showTime,
                             int timeSec,
                             float userPercent,
                             float systemPercent,
                             float iowaitPercent,
                             float idlePercent)
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
    m_MaxHistorySeconds = UI::Numeric::toDouble(settings.maxHistorySeconds);
    m_HistoryScrollSeconds = 0.0;
    m_RefreshAccumulatorSec = 0.0F;
    m_ForceRefresh = true;

    m_Model = std::make_unique<Domain::SystemModel>(Platform::makeSystemProbe());
    m_Model->setMaxHistorySeconds(m_MaxHistorySeconds);

    m_StorageModel = std::make_unique<Domain::StorageModel>(Platform::makeDiskProbe());
    m_StorageModel->setMaxHistorySeconds(m_MaxHistorySeconds);

    // Initial refresh to seed histories
    m_Model->refresh();
    m_StorageModel->sample();

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
    m_StorageModel.reset();
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
    using SecondsF = std::chrono::duration<float>;
    const float intervalSec = std::chrono::duration_cast<SecondsF>(m_RefreshInterval).count();
    const bool intervalElapsed = (intervalSec > 0.0F) && (m_RefreshAccumulatorSec >= intervalSec);

    if (m_ForceRefresh || intervalElapsed)
    {
        m_Model->setMaxHistorySeconds(m_MaxHistorySeconds);
        m_Model->refresh();

        if (m_StorageModel)
        {
            m_StorageModel->setMaxHistorySeconds(m_MaxHistorySeconds);
            m_StorageModel->sample();
        }

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
    const int coreCount = snap.coreCount;
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

    // Update smoothed disk I/O if storage model is available
    if (m_StorageModel)
    {
        auto storageSnap = m_StorageModel->snapshot();
        updateSmoothedDiskIO(storageSnap, m_LastDeltaSeconds);
    }

    // Header line: CPU Model | Cores | Freq | Uptime (right-aligned)
    // Format uptime string
    std::string uptimeStr;
    if (snap.uptimeSeconds > 0)
    {
        std::uint64_t days = snap.uptimeSeconds / 86400;
        std::uint64_t hours = (snap.uptimeSeconds % 86400) / 3600;
        std::uint64_t minutes = (snap.uptimeSeconds % 3600) / 60;

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
        coreInfo = std::format(" ({} cores @ {:.2f} GHz)", snap.coreCount, UI::Numeric::toDouble(snap.cpuFreqMHz) / 1000.0);
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

                ImPlot::SetNextFillStyle(theme.scheme().cpuUserFill);
                ImPlot::PlotShaded(
                    "##CpuUser", breakdownTimeData.data(), y0.data(), yUserTop.data(), UI::Numeric::checkedCount(breakdownCount));

                ImPlot::SetNextFillStyle(theme.scheme().cpuSystemFill);
                ImPlot::PlotShaded(
                    "##CpuSystem", breakdownTimeData.data(), yUserTop.data(), ySystemTop.data(), UI::Numeric::checkedCount(breakdownCount));

                ImPlot::SetNextFillStyle(theme.scheme().cpuIowaitFill);
                ImPlot::PlotShaded("##CpuIowait",
                                   breakdownTimeData.data(),
                                   ySystemTop.data(),
                                   yIowaitTop.data(),
                                   UI::Numeric::checkedCount(breakdownCount));

                ImPlot::SetNextFillStyle(theme.scheme().cpuIdleFill);
                ImPlot::PlotShaded(
                    "##CpuIdle", breakdownTimeData.data(), yIowaitTop.data(), yTotalTop.data(), UI::Numeric::checkedCount(breakdownCount));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto si = hoveredIndexFromPlotX(breakdownTimeData, mouse.x))
                    {
                        showCpuBreakdownTooltip(theme.scheme(),
                                                true,
                                                checkedRoundSeconds(breakdownTimeData[*si]),
                                                cpuUserHist[*si],
                                                cpuSystemHist[*si],
                                                cpuIowaitHist[*si],
                                                cpuIdleHist[*si]);
                    }
                }
            }
            else if (!cpuHist.empty())
            {
                ImPlot::SetNextFillStyle(theme.scheme().chartCpuFill);
                ImPlot::PlotShaded("##CPUShaded", cpuTimeData.data(), cpuHist.data(), UI::Numeric::checkedCount(cpuHist.size()), 0.0);

                ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                ImPlot::PlotLine("##CPU", cpuTimeData.data(), cpuHist.data(), UI::Numeric::checkedCount(cpuHist.size()));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(cpuTimeData, mouse.x))
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(std::format("t: {:.1f}s", cpuTimeData[*idxVal]).c_str());
                        ImGui::Text("CPU: %s", UI::Format::percentCompact(cpuHist[*idxVal]).c_str());
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
                       .value01 = UI::Numeric::percent01(m_SmoothedCpu.total),
                       .color = theme.progressColor(m_SmoothedCpu.total)});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.user),
                       .value01 = UI::Numeric::percent01(m_SmoothedCpu.user),
                       .color = theme.scheme().cpuUser});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.system),
                       .value01 = UI::Numeric::percent01(m_SmoothedCpu.system),
                       .color = theme.scheme().cpuSystem});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.iowait),
                       .value01 = UI::Numeric::percent01(m_SmoothedCpu.iowait),
                       .color = theme.scheme().cpuIowait});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.idle),
                       .value01 = UI::Numeric::percent01(m_SmoothedCpu.idle),
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
                    ImPlot::PlotLine("Used", timeData.data(), memHist.data(), UI::Numeric::checkedCount(memHist.size()));
                }

                if (!cachedHist.empty())
                {
                    ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                    ImPlot::PlotLine("Cached", timeData.data(), cachedHist.data(), UI::Numeric::checkedCount(cachedHist.size()));
                }

                if (!swapHist.empty())
                {
                    ImPlot::SetNextLineStyle(theme.scheme().chartIo, 2.0F);
                    ImPlot::PlotLine("Swap", timeData.data(), swapHist.data(), UI::Numeric::checkedCount(swapHist.size()));
                }

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(std::format("t: {:.1f}s", timeData[*idxVal]).c_str());

                        if (*idxVal < memHist.size())
                        {
                            ImGui::TextColored(
                                theme.scheme().chartMemory, "Used: %s", UI::Format::percentCompact(memHist[*idxVal]).c_str());
                        }
                        if (*idxVal < cachedHist.size())
                        {
                            ImGui::TextColored(
                                theme.scheme().chartCpu, "Cached: %s", UI::Format::percentCompact(cachedHist[*idxVal]).c_str());
                        }
                        if (*idxVal < swapHist.size())
                        {
                            ImGui::TextColored(theme.scheme().chartIo, "Swap: %s", UI::Format::percentCompact(swapHist[*idxVal]).c_str());
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
                                  .value01 = UI::Numeric::percent01(usedPercentClamped),
                                  .color = theme.scheme().chartMemory});

            const double cachedPercent = std::clamp(snap.memoryCachedPercent, 0.0, 100.0);
            memoryBars.push_back({.valueText = UI::Format::percentCompact(cachedPercent),
                                  .value01 = UI::Numeric::percent01(cachedPercent),
                                  .color = theme.scheme().chartCpu});
        }

        if (snap.swapTotalBytes > 0)
        {
            const double swapPercentClamped = std::clamp(m_SmoothedMemory.swapPercent, 0.0, 100.0);
            memoryBars.push_back({.valueText = UI::Format::percentCompact(swapPercentClamped),
                                  .value01 = UI::Numeric::percent01(swapPercentClamped),
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
    const std::string memOverlay =
        UI::Format::bytesUsedTotalPercentCompact(snap.memoryUsedBytes, snap.memoryTotalBytes, m_SmoothedMemory.usedPercent);
    ImVec4 memColor = theme.progressColor(m_SmoothedMemory.usedPercent);
    drawProgressBarWithOverlay(UI::Numeric::percent01(m_SmoothedMemory.usedPercent), memOverlay, memColor);

    // Swap usage bar (if available) with themed color
    if (snap.swapTotalBytes > 0)
    {
        ImGui::Text("Swap:");
        ImGui::SameLine(m_OverviewLabelWidth);
        const std::string swapOverlay =
            UI::Format::bytesUsedTotalPercentCompact(snap.swapUsedBytes, snap.swapTotalBytes, m_SmoothedMemory.swapPercent);
        ImVec4 swapColor = theme.progressColor(m_SmoothedMemory.swapPercent);
        drawProgressBarWithOverlay(UI::Numeric::percent01(m_SmoothedMemory.swapPercent), swapOverlay, swapColor);
    }

    // Disk I/O metrics (if storage model is available)
    if (m_StorageModel && m_SmoothedDiskIO.initialized)
    {
        ImGui::Text("Disk I/O:");
        ImGui::SameLine(m_OverviewLabelWidth);
        const std::string diskOverlay = std::format("R: {:.1f} MB/s  W: {:.1f} MB/s  Util: {:.1f}%%",
                                                    m_SmoothedDiskIO.readMBps,
                                                    m_SmoothedDiskIO.writeMBps,
                                                    m_SmoothedDiskIO.avgUtilization);
        ImVec4 diskColor = theme.progressColor(m_SmoothedDiskIO.avgUtilization);
        drawProgressBarWithOverlay(UI::Numeric::percent01(m_SmoothedDiskIO.avgUtilization), diskOverlay, diskColor);
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
            ImPlot::SetNextFillStyle(theme.scheme().chartCpuFill);
            ImPlot::PlotShaded("##CPUShaded", timeData.data(), cpuHist.data(), UI::Numeric::checkedCount(cpuHist.size()), 0.0);

            // Draw the line on top of the shaded region.
            ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
            ImPlot::PlotLine("##CPU", timeData.data(), cpuHist.data(), UI::Numeric::checkedCount(cpuHist.size()));

            if (ImPlot::IsPlotHovered())
            {
                const size_t n = cpuHist.size();
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    const float timeSec = timeData[*idxVal];
                    if ((breakdownCount == n) && (*idxVal < breakdownCount))
                    {
                        const size_t si = *idxVal;
                        showCpuBreakdownTooltip(theme.scheme(),
                                                true,
                                                checkedRoundSeconds(timeSec),
                                                cpuUserHist[si],
                                                cpuSystemHist[si],
                                                cpuIowaitHist[si],
                                                cpuIdleHist[si]);
                    }
                    else
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(std::format("t: {:.1f}s", timeSec).c_str());
                        ImGui::Text("CPU: %s", UI::Format::percentCompact(cpuHist[*idxVal]).c_str());
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
        const size_t gridCols = std::max<size_t>(1, static_cast<size_t>(gridWidth / cellWidth));
        const int gridColsInt = UI::Numeric::checkedCount(gridCols);
        const size_t gridRows = (coreCount + gridCols - 1) / gridCols;

        if (ImGui::BeginTable("PerCoreGrid", gridColsInt, ImGuiTableFlags_SizingStretchSame))
        {
            for (size_t row = 0; row < gridRows; ++row)
            {
                ImGui::TableNextRow();
                for (size_t col = 0; col < gridCols; ++col)
                {
                    const size_t coreIdx = (row * gridCols) + col;
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
                    if (ImGui::BeginChild(childId.c_str(), ImVec2(-FLT_MIN, childHeight), ImGuiChildFlags_Borders))
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
                                ImPlot::PlotLine("##Core", timeData.data(), samples.data(), UI::Numeric::checkedCount(timeData.size()));
                                ImPlot::EndPlot();
                            }
                        };

                        double smoothed =
                            (coreIdx < m_SmoothedPerCore.size()) ? m_SmoothedPerCore[coreIdx] : snap.cpuPerCore[coreIdx].totalPercent;
                        NowBar bar{.valueText = UI::Format::percentCompact(smoothed),
                                   .value01 = UI::Numeric::percent01(smoothed),
                                   .color = theme.progressColor(smoothed)};

                        std::vector<NowBar> bars;
                        bars.push_back(bar);
                        const std::string tableId = std::format("CoreLayout{}", coreIdx);
                        renderHistoryWithNowBars(tableId.c_str(), HISTORY_PLOT_HEIGHT_DEFAULT, plotFn, bars, false, 0, true);
                    }
                    ImGui::EndChild();
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

    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);

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

    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);

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

    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);
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

void SystemMetricsPanel::updateSmoothedDiskIO(const Domain::StorageSnapshot& snap, float deltaTimeSeconds)
{
    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);

    // Aggregate disk I/O across all devices
    double totalReadMBps = 0.0;
    double totalWriteMBps = 0.0;
    double avgUtilization = 0.0;
    size_t deviceCount = 0;

    for (const auto& disk : snap.disks)
    {
        totalReadMBps += disk.readBytesPerSec / 1048576.0; // Convert to MB/s
        totalWriteMBps += disk.writeBytesPerSec / 1048576.0;
        avgUtilization += disk.utilizationPercent;
        ++deviceCount;
    }

    if (deviceCount > 0)
    {
        avgUtilization /= static_cast<double>(deviceCount);
    }

    if (!m_SmoothedDiskIO.initialized)
    {
        m_SmoothedDiskIO.readMBps = totalReadMBps;
        m_SmoothedDiskIO.writeMBps = totalWriteMBps;
        m_SmoothedDiskIO.avgUtilization = avgUtilization;
        m_SmoothedDiskIO.initialized = true;
        return;
    }

    m_SmoothedDiskIO.readMBps = smoothTowards(m_SmoothedDiskIO.readMBps, totalReadMBps, alpha);
    m_SmoothedDiskIO.writeMBps = smoothTowards(m_SmoothedDiskIO.writeMBps, totalWriteMBps, alpha);
    m_SmoothedDiskIO.avgUtilization = smoothTowards(m_SmoothedDiskIO.avgUtilization, avgUtilization, alpha);
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
                  std::to_underlying(theme.currentFontSize()),
                  m_OverviewLabelWidth,
                  m_PerCoreLabelWidth);
}

} // namespace App
