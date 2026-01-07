#include "SystemMetricsPanel.h"

#include "App/Panel.h"
#include "App/Panels/GpuSection.h"
#include "App/Panels/NetworkSection.h"
#include "App/UserConfig.h"
#include "Domain/GPUModel.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "Platform/Factory.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
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
using UI::Widgets::formatAgeSeconds;
using UI::Widgets::formatAxisLocalized;
using UI::Widgets::formatAxisWatts;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::PLOT_FLAGS_DEFAULT;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

// Get the appropriate battery icon based on charge level
[[nodiscard]] const char* getBatteryIcon(int chargePercent)
{
    if (chargePercent >= 87)
    {
        return ICON_FA_BATTERY_FULL;
    }
    if (chargePercent >= 62)
    {
        return ICON_FA_BATTERY_THREE_QUARTERS;
    }
    if (chargePercent >= 37)
    {
        return ICON_FA_BATTERY_HALF;
    }
    if (chargePercent >= 12)
    {
        return ICON_FA_BATTERY_QUARTER;
    }
    return ICON_FA_BATTERY_EMPTY;
}

// Use shared implementation from ChartWidgets
using UI::Widgets::cropFrontToSize;

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

[[nodiscard]] int checkedRoundSeconds(double seconds)
{
    const long rounded = std::lround(seconds);
    return Domain::Numeric::narrowOr<int>(rounded, std::numeric_limits<int>::max());
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
        const auto ageText = formatAgeSeconds(static_cast<double>(timeSec));
        ImGui::TextUnformatted(ageText.c_str());
        ImGui::Separator();
    }
    ImGui::TextColored(scheme.cpuUser, "User: %s", UI::Format::percentCompact(userPercent).c_str());
    ImGui::TextColored(scheme.cpuSystem, "System: %s", UI::Format::percentCompact(systemPercent).c_str());
    ImGui::TextColored(scheme.cpuIowait, "I/O Wait: %s", UI::Format::percentCompact(iowaitPercent).c_str());
    ImGui::TextColored(scheme.cpuIdle, "Idle: %s", UI::Format::percentCompact(idlePercent).c_str());
    ImGui::EndTooltip();
}

// Network interface utilities (isVirtualInterface, isBluetoothInterface, getSortedFilteredInterfaces)
// are now in App/Panels/NetInterfaceUtils.h to avoid duplication with NetworkPanel.cpp

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
    m_MaxHistorySeconds = Domain::Numeric::toDouble(settings.maxHistorySeconds);
    m_HistoryScrollSeconds = 0.0;
    m_RefreshAccumulatorSec = 0.0F;
    m_ForceRefresh = true;

    m_Model = std::make_unique<Domain::SystemModel>(Platform::makeSystemProbe(), Platform::makePowerProbe());
    m_Model->setMaxHistorySeconds(m_MaxHistorySeconds);

    m_StorageModel = std::make_unique<Domain::StorageModel>(Platform::makeDiskProbe());
    m_StorageModel->setMaxHistorySeconds(m_MaxHistorySeconds);

    m_GPUModel = std::make_unique<Domain::GPUModel>(Platform::makeGPUProbe());

    // Initial refresh to seed histories
    m_Model->refresh();
    m_StorageModel->sample();
    if (m_GPUModel)
    {
        m_GPUModel->refresh();
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

    const auto initialSnap = m_Model->snapshot();
    // NOTE: m_Hostname intentionally stores the raw hostname without any icon prefix.
    // UI code (e.g., tab labels) is responsible for adding icons when rendering.
    m_Hostname = initialSnap.hostname.empty() ? "System" : initialSnap.hostname;
}

void SystemMetricsPanel::onDetach()
{
    m_GPUModel.reset();
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

        if (m_GPUModel)
        {
            m_GPUModel->refresh();
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

    renderContent();

    ImGui::End();
}

void SystemMetricsPanel::renderContent()
{
    if (!m_Model)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textError, "System model not initialized");
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

    // Add padding inside tabs for better spacing
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0F, 8.0F));

    if (ImGui::BeginTabBar("SystemTabs"))
    {
        if (ImGui::BeginTabItem(ICON_FA_GAUGE_HIGH "  Overview"))
        {
            renderOverview();
            ImGui::EndTabItem();
        }

        if (snap.coreCount > 1)
        {
            if (ImGui::BeginTabItem(ICON_FA_MICROCHIP "  CPU Cores"))
            {
                renderPerCoreSection();
                ImGui::EndTabItem();
            }
        }

        // GPU tab - show if GPUs are available
        if (m_GPUModel)
        {
            const auto gpuInfos = m_GPUModel->gpuInfo();
            if (!gpuInfos.empty())
            {
                if (ImGui::BeginTabItem(ICON_FA_MICROCHIP "  GPU"))
                {
                    // Build context for GpuSection render function
                    GpuSection::RenderContext gpuCtx{
                        .gpuModel = m_GPUModel.get(),
                        .maxHistorySeconds = m_MaxHistorySeconds,
                        .historyScrollSeconds = m_HistoryScrollSeconds,
                        .lastDeltaSeconds = m_LastDeltaSeconds,
                        .refreshInterval = m_RefreshInterval,
                        .smoothedGPUs = &m_SmoothedGPUs,
                    };
                    GpuSection::renderGpuSection(gpuCtx);
                    ImGui::EndTabItem();
                }
            }
        }

        // Network and I/O tab - show if network counters are available
        if (m_Model != nullptr && m_Model->capabilities().hasNetworkCounters)
        {
            if (ImGui::BeginTabItem(ICON_FA_NETWORK_WIRED "  Network and I/O"))
            {
                // Build context for NetworkSection render functions
                NetworkSection::RenderContext netCtx{
                    .systemModel = m_Model.get(),
                    .storageModel = m_StorageModel.get(),
                    .maxHistorySeconds = m_MaxHistorySeconds,
                    .historyScrollSeconds = m_HistoryScrollSeconds,
                    .lastDeltaSeconds = m_LastDeltaSeconds,
                    .refreshInterval = m_RefreshInterval,
                    .smoothedDiskReadBytesPerSec = &m_SmoothedSystemIO.readBytesPerSec,
                    .smoothedDiskWriteBytesPerSec = &m_SmoothedSystemIO.writeBytesPerSec,
                    .smoothedDiskInitialized = &m_SmoothedSystemIO.initialized,
                    .smoothedNetSentBytesPerSec = &m_SmoothedNetwork.sentBytesPerSec,
                    .smoothedNetRecvBytesPerSec = &m_SmoothedNetwork.recvBytesPerSec,
                    .smoothedNetInitialized = &m_SmoothedNetwork.initialized,
                    .selectedNetworkInterface = &m_SelectedNetworkInterface,
                };
                NetworkSection::renderNetworkSection(netCtx);
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    ImGui::PopStyleVar(); // FramePadding
}

void SystemMetricsPanel::renderOverview()
{
    auto snap = m_Model->snapshot();

    updateSmoothedCpu(snap, m_LastDeltaSeconds);
    updateSmoothedMemory(snap, m_LastDeltaSeconds);

    // Update smoothed disk I/O if storage model is available
    if (m_StorageModel)
    {
        auto storageSnap = m_StorageModel->latestSnapshot();
        updateSmoothedDiskIO(storageSnap, m_LastDeltaSeconds);
    }

    // Header line: CPU Model | Cores | Freq | Uptime (right-aligned)
    // Format uptime string
    const std::string uptimeStr = UI::Format::formatUptimeShort(snap.uptimeSeconds);

    // Display: "CPU Model (N cores @ X.XX GHz)     Uptime: Xd Yh Zm"
    std::string coreInfo;
    if (snap.cpuFreqMHz > 0)
    {
        coreInfo = std::format(" ({} cores @ {:.2f} GHz)", snap.coreCount, Domain::Numeric::toDouble(snap.cpuFreqMHz) / 1000.0);
    }
    else
    {
        coreInfo = std::format(" ({} cores)", snap.coreCount);
    }

    const std::string processStr = (m_ProcessModel != nullptr)
                                     ? std::format("Processes: {}", UI::Format::formatIntLocalized(m_ProcessModel->processCount()))
                                     : std::string{};

    const ImGuiStyle& style = ImGui::GetStyle();
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float uptimeWidth = uptimeStr.empty() ? 0.0F : ImGui::CalcTextSize(uptimeStr.c_str()).x;
    const float processWidth = processStr.empty() ? 0.0F : ImGui::CalcTextSize(processStr.c_str()).x;
    const float spacer = (!processStr.empty() && !uptimeStr.empty()) ? style.ItemSpacing.x : 0.0F;
    const float rightBlockWidth = uptimeWidth + processWidth + spacer;

    // Calculate total GPU VRAM across all GPUs (for discrete GPUs with dedicated memory)
    std::uint64_t totalVramBytes = 0;
    if (m_GPUModel)
    {
        const auto gpuSnapshots = m_GPUModel->snapshots();
        for (const auto& gpuSnap : gpuSnapshots)
        {
            totalVramBytes += gpuSnap.memoryTotalBytes;
        }
    }

    // Format RAM and VRAM info to append to CPU line
    std::string memoryStr;
    if (totalVramBytes > 0)
    {
        memoryStr = std::format(", {} RAM, {} VRAM",
                                UI::Format::formatBytes(static_cast<double>(snap.memoryTotalBytes)),
                                UI::Format::formatBytes(static_cast<double>(totalVramBytes)));
    }
    else
    {
        memoryStr = std::format(", {} RAM", UI::Format::formatBytes(static_cast<double>(snap.memoryTotalBytes)));
    }

    // CPU model with core count, frequency, RAM, and VRAM
    ImGui::TextUnformatted(snap.cpuModel.c_str());
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(coreInfo.c_str());
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(memoryStr.c_str());

    // Right-align uptime
    if (rightBlockWidth > 0.0F)
    {
        ImGui::SameLine(std::max(0.0F, availWidth - rightBlockWidth));
        if (!processStr.empty())
        {
            ImGui::TextUnformatted(processStr.c_str());
            if (!uptimeStr.empty())
            {
                ImGui::SameLine(0.0F, spacer);
            }
        }
        if (!uptimeStr.empty())
        {
            ImGui::TextUnformatted(uptimeStr.c_str());
        }
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
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MICROCHIP "  CPU Usage (%zu samples)", cpuCount);

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
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot("##OverviewCPUHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT))
        {
            UI::Widgets::setupLegendDefault();
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, UI::Widgets::formatAxisPercent);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

            if (breakdownCount > 0)
            {
                std::vector<float> y0(breakdownCount, 0.0F);
                std::vector<float> yUserTop(breakdownCount);
                std::vector<float> ySystemTop(breakdownCount);
                std::vector<float> yIowaitTop(breakdownCount);

                for (size_t i = 0; i < breakdownCount; ++i)
                {
                    yUserTop[i] = cpuUserHist[i];
                    ySystemTop[i] = cpuUserHist[i] + cpuSystemHist[i];
                    yIowaitTop[i] = ySystemTop[i] + cpuIowaitHist[i];
                }

                ImPlot::SetNextFillStyle(theme.scheme().cpuUserFill);
                ImPlot::PlotShaded("User", breakdownTimeData.data(), y0.data(), yUserTop.data(), UI::Format::checkedCount(breakdownCount));

                ImPlot::SetNextFillStyle(theme.scheme().cpuSystemFill);
                ImPlot::PlotShaded(
                    "System", breakdownTimeData.data(), yUserTop.data(), ySystemTop.data(), UI::Format::checkedCount(breakdownCount));

                ImPlot::SetNextFillStyle(theme.scheme().cpuIowaitFill);
                ImPlot::PlotShaded(
                    "I/O Wait", breakdownTimeData.data(), ySystemTop.data(), yIowaitTop.data(), UI::Format::checkedCount(breakdownCount));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto si = hoveredIndexFromPlotX(breakdownTimeData, mouse.x))
                    {
                        showCpuBreakdownTooltip(theme.scheme(),
                                                true,
                                                checkedRoundSeconds(static_cast<double>(breakdownTimeData[*si])),
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
                ImPlot::PlotShaded("##CPUShaded", cpuTimeData.data(), cpuHist.data(), UI::Format::checkedCount(cpuHist.size()), 0.0);

                ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                ImPlot::PlotLine("CPU", cpuTimeData.data(), cpuHist.data(), UI::Format::checkedCount(cpuHist.size()));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(cpuTimeData, mouse.x))
                    {
                        ImGui::BeginTooltip();
                        const auto ageText = formatAgeSeconds(static_cast<double>(cpuTimeData[*idxVal]));
                        ImGui::TextUnformatted(ageText.c_str());
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
                       .label = "CPU Total",
                       .value01 = UI::Format::percent01(m_SmoothedCpu.total),
                       .color = theme.progressColor(m_SmoothedCpu.total)});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.user),
                       .label = "User",
                       .value01 = UI::Format::percent01(m_SmoothedCpu.user),
                       .color = theme.scheme().cpuUser});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.system),
                       .label = "System",
                       .value01 = UI::Format::percent01(m_SmoothedCpu.system),
                       .color = theme.scheme().cpuSystem});
    cpuBars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedCpu.iowait),
                       .label = "I/O Wait",
                       .value01 = UI::Format::percent01(m_SmoothedCpu.iowait),
                       .color = theme.scheme().cpuIowait});

    constexpr size_t OVERVIEW_NOW_BAR_COLUMNS = 4; // CPU: Total, User, System, I/O Wait
    renderHistoryWithNowBars("OverviewCPUHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, cpuPlot, cpuBars, false, OVERVIEW_NOW_BAR_COLUMNS);

    ImGui::Spacing();

    // Memory & Swap history (moved from Memory tab)
    {
        auto memHist = m_Model->memoryHistory();
        auto cachedHist = m_Model->memoryCachedHistory();
        auto swapHist = m_Model->swapHistory();
        double peakMemPercent = 0.0;

        ImGui::TextColored(
            theme.scheme().textPrimary, ICON_FA_MEMORY "  Memory & Swap (%zu samples)", std::min(memHist.size(), timestamps.size()));
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
            const UI::Widgets::PlotFontGuard fontGuard;
            if (ImPlot::BeginPlot("##MemorySwapHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT))
            {
                UI::Widgets::setupLegendDefault();
                ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                ImPlot::SetupAxisFormat(ImAxis_Y1, UI::Widgets::formatAxisPercent);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

                if (!memHist.empty())
                {
                    plotLineWithFill(
                        "Used", timeData.data(), memHist.data(), UI::Format::checkedCount(memHist.size()), theme.scheme().chartMemory);
                    peakMemPercent = static_cast<double>(*std::ranges::max_element(memHist));
                }

                if (!cachedHist.empty())
                {
                    plotLineWithFill("Cached",
                                     timeData.data(),
                                     cachedHist.data(),
                                     UI::Format::checkedCount(cachedHist.size()),
                                     theme.scheme().chartCpu);
                }

                if (!swapHist.empty())
                {
                    plotLineWithFill(
                        "Swap", timeData.data(), swapHist.data(), UI::Format::checkedCount(swapHist.size()), theme.scheme().chartIo);
                }

                if (peakMemPercent > 0.0)
                {
                    const float peak = UI::Format::toFloatNarrow(peakMemPercent);
                    const float xLine[2] = {UI::Format::toFloatNarrow(axisConfig.xMin), UI::Format::toFloatNarrow(axisConfig.xMax)};
                    const float yLine[2] = {peak, peak};
                    ImPlot::SetNextLineStyle(theme.scheme().textWarning, 1.5F);
                    ImPlot::PlotLine("##MemPeak", xLine, yLine, 2);
                }

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        ImGui::BeginTooltip();
                        const auto ageText = formatAgeSeconds(static_cast<double>(timeData[*idxVal]));
                        ImGui::TextUnformatted(ageText.c_str());

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
                                  .label = "Memory Used",
                                  .value01 = UI::Format::percent01(usedPercentClamped),
                                  .color = theme.scheme().chartMemory});

            const double cachedPercentClamped = std::clamp(m_SmoothedMemory.cachedPercent, 0.0, 100.0);
            memoryBars.push_back({.valueText = UI::Format::percentCompact(cachedPercentClamped),
                                  .label = "Memory Cached",
                                  .value01 = UI::Format::percent01(cachedPercentClamped),
                                  .color = theme.scheme().chartCpu});
        }

        if (snap.swapTotalBytes > 0)
        {
            const double swapPercentClamped = std::clamp(m_SmoothedMemory.swapPercent, 0.0, 100.0);
            memoryBars.push_back({.valueText = UI::Format::percentCompact(swapPercentClamped),
                                  .label = "Swap Used",
                                  .value01 = UI::Format::percent01(swapPercentClamped),
                                  .color = theme.scheme().chartIo});
        }

        renderHistoryWithNowBars(
            "MemorySwapHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, memoryPlot, memoryBars, false, OVERVIEW_NOW_BAR_COLUMNS);

        ImGui::Spacing();
    }

    // Power & Battery history chart (combines per-process power aggregation with battery charge %)
    if (m_ProcessModel != nullptr || snap.power.hasBattery)
    {
        // Get power history from ProcessModel (aggregated per-process power)
        std::vector<double> procTimestamps;
        std::vector<double> powerHistDouble;
        if (m_ProcessModel != nullptr)
        {
            procTimestamps = m_ProcessModel->historyTimestamps();
            powerHistDouble = m_ProcessModel->systemPowerHistory();
        }

        // Get battery charge history from SystemModel
        const auto batteryHistFloat = m_Model->batteryChargeHistory();

        // Align to timestamps - use process timestamps as primary if available, else system timestamps
        const auto& alignTimestamps = !procTimestamps.empty() ? procTimestamps : timestamps;
        const size_t powerCount = std::min(powerHistDouble.size(), alignTimestamps.size());
        const size_t batteryCount = std::min(batteryHistFloat.size(), timestamps.size());
        const size_t alignedCount = std::max(powerCount, batteryCount);

        if (alignedCount > 0)
        {
            // Convert power double history to float for ImPlot compatibility
            std::vector<float> powerHist;
            if (powerCount > 0)
            {
                powerHist.reserve(powerCount);
                const auto startIt = powerHistDouble.end() - static_cast<std::ptrdiff_t>(powerCount);
                for (auto it = startIt; it != powerHistDouble.end(); ++it)
                {
                    powerHist.push_back(static_cast<float>(*it));
                }
            }

            // Extract aligned battery history (filter out -1 = no data)
            std::vector<float> batteryHist;
            if (batteryCount > 0)
            {
                batteryHist.reserve(batteryCount);
                const auto startIt = batteryHistFloat.end() - static_cast<std::ptrdiff_t>(batteryCount);
                for (auto it = startIt; it != batteryHistFloat.end(); ++it)
                {
                    batteryHist.push_back(*it >= 0.0F ? *it : 0.0F);
                }
            }

            std::vector<float> timeData = buildTimeAxis(alignTimestamps, alignedCount, nowSeconds);
            const auto axis = makeTimeAxisConfig(alignTimestamps, m_MaxHistorySeconds, m_HistoryScrollSeconds);
            // Update smoothed values
            const float targetPower = powerHist.empty() ? 0.0F : powerHist.back();
            const float targetBattery = batteryHist.empty() ? 0.0F : batteryHist.back();
            updateSmoothedPower(targetPower, targetBattery, m_LastDeltaSeconds);

            // Compute max for power scale
            double powerMaxAbs = 1.0;
            for (const float v : powerHist)
            {
                powerMaxAbs = std::max(powerMaxAbs, static_cast<double>(std::abs(v)));
            }
            powerMaxAbs = std::max(powerMaxAbs, std::abs(m_SmoothedPower.watts));

            // Build NowBars
            std::vector<NowBar> bars;
            bars.push_back({.valueText = UI::Format::formatPowerCompact(m_SmoothedPower.watts),
                            .label = "Power Draw",
                            .value01 = std::clamp(std::abs(m_SmoothedPower.watts) / powerMaxAbs, 0.0, 1.0),
                            .color = theme.scheme().chartCpu});

            if (snap.power.hasBattery)
            {
                bars.push_back({.valueText = UI::Format::percentCompact(m_SmoothedPower.batteryChargePercent),
                                .label = "Battery Charge",
                                .value01 = UI::Format::percent01(m_SmoothedPower.batteryChargePercent),
                                .color = theme.scheme().chartMemory});
            }

            auto plot = [&]()
            {
                const UI::Widgets::PlotFontGuard fontGuard;
                if (ImPlot::BeginPlot("##PowerBatteryHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT))
                {
                    UI::Widgets::setupLegendDefault();

                    // Primary Y-axis: Power (Watts)
                    ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
                    ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisWatts);
                    ImPlot::SetupAxisLimits(ImAxis_X1, axis.xMin, axis.xMax, ImPlotCond_Always);

                    // Secondary Y-axis: Battery % (0-100) - hidden ticks to keep X-axis alignment
                    if (snap.power.hasBattery && !batteryHist.empty())
                    {
                        ImPlot::SetupAxis(ImAxis_Y2,
                                          "",
                                          ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels |
                                              ImPlotAxisFlags_NoTickMarks);
                        ImPlot::SetupAxisLimits(ImAxis_Y2, 0, 100, ImPlotCond_Always);
                    }

                    // Plot power on primary Y-axis
                    if (!powerHist.empty())
                    {
                        plotLineWithFill("Power",
                                         timeData.data(),
                                         powerHist.data(),
                                         UI::Format::checkedCount(powerHist.size()),
                                         theme.scheme().chartCpu);
                    }

                    // Plot battery charge on secondary Y-axis
                    if (snap.power.hasBattery && !batteryHist.empty())
                    {
                        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                        plotLineWithFill("Battery",
                                         timeData.data(),
                                         batteryHist.data(),
                                         UI::Format::checkedCount(batteryHist.size()),
                                         theme.scheme().chartMemory);
                        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1); // Reset to primary
                    }

                    // Tooltip
                    if (ImPlot::IsPlotHovered())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                        {
                            ImGui::BeginTooltip();
                            const auto ageText = formatAgeSeconds(static_cast<double>(timeData[*idxVal]));
                            ImGui::TextUnformatted(ageText.c_str());
                            ImGui::Separator();

                            if (*idxVal < powerHist.size())
                            {
                                const double powerVal = Domain::Numeric::toDouble(powerHist[*idxVal]);
                                ImGui::TextColored(theme.scheme().chartCpu, "Power: %s", UI::Format::formatPowerCompact(powerVal).c_str());
                            }
                            if (*idxVal < batteryHist.size() && snap.power.hasBattery)
                            {
                                const double batteryVal = Domain::Numeric::toDouble(batteryHist[*idxVal]);
                                ImGui::TextColored(
                                    theme.scheme().chartMemory, "Battery: %s", UI::Format::percentCompact(batteryVal).c_str());
                            }
                            ImGui::EndTooltip();
                        }
                    }

                    ImPlot::EndPlot();
                }
            };

            // Chart header with sample count and battery status
            std::string headerLeft;
            std::string headerRight;

            if (snap.power.hasBattery)
            {
                headerLeft = std::format(ICON_FA_BOLT "  Power & Battery ({} samples)", alignedCount);

                // Build right-aligned status string with icons
                const int chargeInt = snap.power.chargePercent;
                const char* batteryIcon = getBatteryIcon(chargeInt);

                if (snap.power.isCharging)
                {
                    if (snap.power.timeToFullSec > 0)
                    {
                        const auto hours = snap.power.timeToFullSec / 3600;
                        const auto mins = (snap.power.timeToFullSec % 3600) / 60;
                        headerRight = std::format("{} {} {}% ({:d}:{:02d} to full)", ICON_FA_BOLT, batteryIcon, chargeInt, hours, mins);
                    }
                    else
                    {
                        headerRight = std::format("{} {} {}%", ICON_FA_BOLT, batteryIcon, chargeInt);
                    }
                }
                else if (snap.power.isFull)
                {
                    headerRight = std::format("{} {} 100%", ICON_FA_PLUG, ICON_FA_BATTERY_FULL);
                }
                else if (snap.power.isDischarging)
                {
                    if (snap.power.timeToEmptySec > 0)
                    {
                        const auto hours = snap.power.timeToEmptySec / 3600;
                        const auto mins = (snap.power.timeToEmptySec % 3600) / 60;
                        headerRight = std::format("{} {}% ({:d}:{:02d} left)", batteryIcon, chargeInt, hours, mins);
                    }
                    else
                    {
                        headerRight = std::format("{} {}%", batteryIcon, chargeInt);
                    }
                }
                else
                {
                    headerRight = std::format("{} {}%", batteryIcon, chargeInt);
                }
            }
            else
            {
                headerLeft = std::format(ICON_FA_BOLT "  Power ({} samples)", alignedCount);
            }

            // Render header with left and right parts
            ImGui::TextColored(theme.scheme().textPrimary, "%s", headerLeft.c_str());
            if (!headerRight.empty())
            {
                // Calculate right-aligned position to align with chart's right edge (not NowBars)
                // NowBar column width: BAR_WIDTH * OVERVIEW_NOW_BAR_COLUMNS + spacing
                const ImGuiStyle& headerStyle = ImGui::GetStyle();
                const float barColumnWidth = (UI::Widgets::BAR_WIDTH * static_cast<float>(OVERVIEW_NOW_BAR_COLUMNS)) +
                                             (headerStyle.ItemSpacing.x * (static_cast<float>(OVERVIEW_NOW_BAR_COLUMNS) - 1.0F));
                const float chartRightEdge = ImGui::GetContentRegionAvail().x - barColumnWidth - headerStyle.CellPadding.x;
                const float rightTextWidth = ImGui::CalcTextSize(headerRight.c_str()).x;
                ImGui::SameLine(chartRightEdge - rightTextWidth);
                ImGui::TextUnformatted(headerRight.c_str());
            }

            // Tooltip with detailed info
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Power: Aggregated CPU-proportional estimate from all processes.");
                if (snap.power.hasBattery)
                {
                    ImGui::TextUnformatted("Battery: System battery charge percentage (0-100%).");
                    ImGui::Separator();
                    if (snap.power.healthPercent >= 0)
                    {
                        ImGui::Text("Health: %s", UI::Format::percentCompact(snap.power.healthPercent).c_str());
                    }
                    if (!snap.power.technology.empty())
                    {
                        ImGui::Text("Technology: %s", snap.power.technology.c_str());
                    }
                    if (!snap.power.model.empty())
                    {
                        ImGui::Text("Model: %s", snap.power.model.c_str());
                    }
                }
                ImGui::EndTooltip();
            }

            renderHistoryWithNowBars("PowerBatteryHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, plot, bars, false, OVERVIEW_NOW_BAR_COLUMNS);
            ImGui::Spacing();
        }
    }

    // Threads & Page Faults combined (aggregated from processes)
    if (m_ProcessModel != nullptr)
    {
        const auto procTimestamps = m_ProcessModel->historyTimestamps();
        const auto pageFaultHist = m_ProcessModel->systemPageFaultsHistory();
        const auto threadHist = m_ProcessModel->systemThreadCountHistory();
        const size_t alignedCount = std::min({procTimestamps.size(), pageFaultHist.size(), threadHist.size()});

        // Always use default axis config even with no data
        const auto axis = alignedCount > 0 ? makeTimeAxisConfig(procTimestamps, m_MaxHistorySeconds, m_HistoryScrollSeconds)
                                           : makeTimeAxisConfig({}, m_MaxHistorySeconds, m_HistoryScrollSeconds);

        std::vector<float> timeData;
        std::vector<float> faultData;
        std::vector<float> threadData;

        if (alignedCount > 0)
        {
            timeData = buildTimeAxis(procTimestamps, alignedCount, nowSeconds);
            faultData.assign(pageFaultHist.end() - static_cast<std::ptrdiff_t>(alignedCount), pageFaultHist.end());
            threadData.assign(threadHist.end() - static_cast<std::ptrdiff_t>(alignedCount), threadHist.end());

            // Update smoothed values
            const double targetThreads = static_cast<double>(threadData.back());
            const double targetFaults = static_cast<double>(faultData.back());
            updateSmoothedThreadsFaults(targetThreads, targetFaults, m_LastDeltaSeconds);
        }

        const double threadMax = threadData.empty()
                                   ? 1.0
                                   : std::max(m_SmoothedThreadsFaults.threads, static_cast<double>(*std::ranges::max_element(threadData)));
        const double faultMax = faultData.empty()
                                  ? 1.0
                                  : std::max(m_SmoothedThreadsFaults.pageFaults, static_cast<double>(*std::ranges::max_element(faultData)));

        const NowBar threadsBar{.valueText = UI::Format::formatCountWithLabel(std::llround(m_SmoothedThreadsFaults.threads), "threads"),
                                .label = "Threads",
                                .value01 = (threadMax > 0.0) ? std::clamp(m_SmoothedThreadsFaults.threads / threadMax, 0.0, 1.0) : 0.0,
                                .color = theme.scheme().chartCpu};
        const NowBar faultsBar{.valueText = UI::Format::formatCountPerSecond(m_SmoothedThreadsFaults.pageFaults),
                               .label = "Page Faults",
                               .value01 = (faultMax > 0.0) ? std::clamp(m_SmoothedThreadsFaults.pageFaults / faultMax, 0.0, 1.0) : 0.0,
                               .color = theme.accentColor(3)};

        auto plot = [&]()
        {
            const UI::Widgets::PlotFontGuard fontGuard;
            if (ImPlot::BeginPlot("##ThreadsFaultsHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT))
            {
                UI::Widgets::setupLegendDefault();
                ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
                ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisLocalized);
                ImPlot::SetupAxisLimits(ImAxis_X1, axis.xMin, axis.xMax, ImPlotCond_Always);

                const int count = UI::Format::checkedCount(alignedCount);
                plotLineWithFill("Threads", timeData.data(), threadData.data(), count, theme.scheme().chartCpu);
                plotLineWithFill("Page Faults/s", timeData.data(), faultData.data(), count, theme.accentColor(3));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        if (*idxVal < alignedCount)
                        {
                            ImGui::BeginTooltip();
                            const auto ageText = formatAgeSeconds(static_cast<double>(timeData[*idxVal]));
                            ImGui::TextUnformatted(ageText.c_str());
                            ImGui::Separator();
                            ImGui::TextColored(theme.scheme().chartCpu,
                                               "Threads: %s",
                                               UI::Format::formatIntLocalized(std::llround(threadData[*idxVal])).c_str());
                            ImGui::TextColored(theme.accentColor(3),
                                               "Page Faults: %s",
                                               UI::Format::formatCountPerSecond(static_cast<double>(faultData[*idxVal])).c_str());
                            ImGui::EndTooltip();
                        }
                    }
                }

                ImPlot::EndPlot();
            }
        };

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GEARS "  Threads & Page Faults (%zu samples)", alignedCount);
        renderHistoryWithNowBars(
            "ThreadsFaultsHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, plot, {threadsBar, faultsBar}, false, OVERVIEW_NOW_BAR_COLUMNS);
        ImGui::Spacing();
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

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CHART_LINE "  CPU History (%zu samples)", cpuHist.size());
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
    {
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot("##CPUHistory", ImVec2(-1, 200), PLOT_FLAGS_DEFAULT))
        {
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, UI::Widgets::formatAxisPercent);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

            if (!cpuHist.empty())
            {
                ImPlot::SetNextFillStyle(theme.scheme().chartCpuFill);
                ImPlot::PlotShaded("##CPUShaded", timeData.data(), cpuHist.data(), UI::Format::checkedCount(cpuHist.size()), 0.0);

                // Draw the line on top of the shaded region.
                ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                ImPlot::PlotLine("##CPU", timeData.data(), cpuHist.data(), UI::Format::checkedCount(cpuHist.size()));

                if (ImPlot::IsPlotHovered())
                {
                    const size_t n = cpuHist.size();
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        const double timeSec = static_cast<double>(timeData[*idxVal]);
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
                            const auto ageText = formatAgeSeconds(static_cast<double>(timeSec));
                            ImGui::TextUnformatted(ageText.c_str());
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

    // CPU model header (same as Overview tab)
    std::string coreInfo;
    if (snap.cpuFreqMHz > 0)
    {
        coreInfo = std::format(" ({} cores @ {:.2f} GHz)", snap.coreCount, Domain::Numeric::toDouble(snap.cpuFreqMHz) / 1000.0);
    }
    else
    {
        coreInfo = std::format(" ({} cores)", snap.coreCount);
    }
    ImGui::TextUnformatted(snap.cpuModel.c_str());
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(coreInfo.c_str());
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
        const float gridWidth = ImGui::GetContentRegionAvail().x;
        constexpr float minCellWidth = 240.0F;
        const float barWidth = ImGui::GetFrameHeight(); // Match prior horizontal bar height for visual consistency
        const float cellWidth = minCellWidth + barWidth;
        const size_t gridCols = std::max<size_t>(1, static_cast<size_t>(gridWidth / cellWidth));
        const int gridColsInt = UI::Format::checkedCount(gridCols);
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

                    const std::string coreLabel = std::format(ICON_FA_MICROCHIP " Core {}", coreIdx);

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
                            const UI::Widgets::PlotFontGuard fontGuard;
                            if (ImPlot::BeginPlot("##PerCorePlot", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT))
                            {
                                ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                                ImPlot::SetupAxisFormat(ImAxis_Y1, UI::Widgets::formatAxisPercent);
                                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
                                ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

                                plotLineWithFill("##Core",
                                                 timeData.data(),
                                                 samples.data(),
                                                 UI::Format::checkedCount(timeData.size()),
                                                 theme.scheme().chartCpu);

                                if (ImPlot::IsPlotHovered() && !timeData.empty())
                                {
                                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                                    {
                                        ImGui::BeginTooltip();
                                        const auto ageText = formatAgeSeconds(static_cast<double>(timeData[*idxVal]));
                                        ImGui::TextUnformatted(ageText.c_str());
                                        if (*idxVal < samples.size())
                                        {
                                            ImGui::TextColored(
                                                theme.scheme().chartCpu, "CPU: %.1f%%", static_cast<double>(samples[*idxVal]));
                                        }
                                        ImGui::EndTooltip();
                                    }
                                }
                                ImPlot::EndPlot();
                            }
                        };

                        const double smoothed =
                            (coreIdx < m_SmoothedPerCore.size()) ? m_SmoothedPerCore[coreIdx] : snap.cpuPerCore[coreIdx].totalPercent;
                        const NowBar bar{.valueText = UI::Format::percentCompact(smoothed),
                                         .label = std::format("Core {}", coreIdx),
                                         .value01 = UI::Format::percent01(smoothed),
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
    const double targetCached = clampPercent(snap.memoryCachedPercent);
    const double targetSwap = clampPercent(snap.swapUsedPercent);

    if (!m_SmoothedMemory.initialized)
    {
        m_SmoothedMemory.usedPercent = targetMem;
        m_SmoothedMemory.cachedPercent = targetCached;
        m_SmoothedMemory.swapPercent = targetSwap;
        m_SmoothedMemory.initialized = true;
        return;
    }

    m_SmoothedMemory.usedPercent = clampPercent(smoothTowards(m_SmoothedMemory.usedPercent, targetMem, alpha));
    m_SmoothedMemory.cachedPercent = clampPercent(smoothTowards(m_SmoothedMemory.cachedPercent, targetCached, alpha));
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

void SystemMetricsPanel::updateSmoothedPower(float targetWatts, float targetBatteryPercent, float deltaTimeSeconds)
{
    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);
    const double targetW = static_cast<double>(targetWatts);
    const double targetB = static_cast<double>(targetBatteryPercent);

    if (!m_SmoothedPower.initialized)
    {
        m_SmoothedPower.watts = targetW;
        m_SmoothedPower.batteryChargePercent = targetB;
        m_SmoothedPower.initialized = true;
        return;
    }

    m_SmoothedPower.watts = smoothTowards(m_SmoothedPower.watts, targetW, alpha);
    m_SmoothedPower.batteryChargePercent = smoothTowards(m_SmoothedPower.batteryChargePercent, targetB, alpha);
}

void SystemMetricsPanel::updateSmoothedThreadsFaults(double targetThreads, double targetFaults, float deltaTimeSeconds)
{
    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);

    if (!m_SmoothedThreadsFaults.initialized)
    {
        m_SmoothedThreadsFaults.threads = targetThreads;
        m_SmoothedThreadsFaults.pageFaults = targetFaults;
        m_SmoothedThreadsFaults.initialized = true;
        return;
    }

    m_SmoothedThreadsFaults.threads = smoothTowards(m_SmoothedThreadsFaults.threads, targetThreads, alpha);
    m_SmoothedThreadsFaults.pageFaults = smoothTowards(m_SmoothedThreadsFaults.pageFaults, targetFaults, alpha);
}

void SystemMetricsPanel::updateSmoothedSystemIO(double targetRead, double targetWrite, float deltaTimeSeconds)
{
    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);

    if (!m_SmoothedSystemIO.initialized)
    {
        m_SmoothedSystemIO.readBytesPerSec = targetRead;
        m_SmoothedSystemIO.writeBytesPerSec = targetWrite;
        m_SmoothedSystemIO.initialized = true;
        return;
    }

    m_SmoothedSystemIO.readBytesPerSec = smoothTowards(m_SmoothedSystemIO.readBytesPerSec, targetRead, alpha);
    m_SmoothedSystemIO.writeBytesPerSec = smoothTowards(m_SmoothedSystemIO.writeBytesPerSec, targetWrite, alpha);
}

void SystemMetricsPanel::updateSmoothedNetwork(double targetSent, double targetRecv, float deltaTimeSeconds)
{
    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);

    if (!m_SmoothedNetwork.initialized)
    {
        m_SmoothedNetwork.sentBytesPerSec = targetSent;
        m_SmoothedNetwork.recvBytesPerSec = targetRecv;
        m_SmoothedNetwork.initialized = true;
        return;
    }

    m_SmoothedNetwork.sentBytesPerSec = smoothTowards(m_SmoothedNetwork.sentBytesPerSec, targetSent, alpha);
    m_SmoothedNetwork.recvBytesPerSec = smoothTowards(m_SmoothedNetwork.recvBytesPerSec, targetRecv, alpha);
}

} // namespace App
