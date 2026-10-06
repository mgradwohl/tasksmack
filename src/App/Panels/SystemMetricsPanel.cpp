#include "SystemMetricsPanel.h"

#include "App/Panel.h"
#include "App/Panels/AdaptiveIntervalUtils.h"
#include "App/Panels/CpuCoreGridIds.h"
#include "App/Panels/CpuCoresSection.h"
#include "App/Panels/CpuSummaryText.h"
#include "App/Panels/GpuSection.h"
#include "App/Panels/MemorySection.h"
#include "App/Panels/NetworkSection.h"
#include "App/ShellMetrics.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/GPUModel.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "Platform/Factory.h"
#include "PowerStatusText.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/LineLayout.h"
#include "UI/RateAxis.h"
#include "UI/TabContent.h"
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
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace App
{

namespace
{

using UI::Widgets::computeAlpha;
using UI::Widgets::formatAxisLocalized;
using UI::Widgets::formatAxisWatts;
using UI::Widgets::frameTimeAxis;
using UI::Widgets::plotLineWithFill;

struct HistoryRange
{
    size_t start = 0;
    size_t count = 0;
};

using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::NowBarList;
using UI::Widgets::plotSeries;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::SeriesRole;
using UI::Widgets::seriesStyle;

/// Hover tooltip for the system CPU chart: the age of the hovered sample to a tenth of a second, as
/// every other chart shows it, then Total and each band of the stack.
///
/// Total is busy time, 100 - (idle + iowait), so it includes irq, softirq and steal time that the
/// User/System bands do not; showing it is what makes the tooltip agree with the Total line and the
/// Total bar. I/O Wait is idle time, not busy (#1157), and is listed after it.
// One label per series, shared by its value-strip entry, tooltip row and NowBar (#1008).
constexpr const char* CPU_TOTAL_LABEL = "Total";
constexpr const char* CPU_USER_LABEL = "User";
constexpr const char* CPU_SYSTEM_LABEL = "System";
constexpr const char* CPU_IOWAIT_LABEL = "I/O Wait";
constexpr const char* CPU_IDLE_LABEL = "Idle";
constexpr const char* POWER_LABEL = "Power";
constexpr const char* BATTERY_LABEL = "Battery";
// A series on a chart's right-hand axis ends in " →", pointing at it (setupSecondaryRateAxis(), #1206); in
// its value-strip entry and tooltip rows the mark follows the value (SECONDARY_AXIS_MARK, #1300).
constexpr const char* BATTERY_Y2_LABEL = "Battery →"; // Beside Power, on its own 0-100 % axis
constexpr const char* THREADS_LABEL = "Threads";
constexpr const char* FAULTS_LABEL = "Page Faults →"; // Right-hand axis; its values carry the "/s" ("12.0/s"), #1202

void showCpuBreakdownTooltip(const UI::ColorScheme& scheme,
                             double ageSeconds,
                             float totalPercent,
                             float userPercent,
                             float systemPercent,
                             std::optional<float> iowaitPercent,
                             float idlePercent)
{
    std::vector<UI::Widgets::TooltipRow> rows{
        {.label = CPU_TOTAL_LABEL, .color = scheme.chartCpu, .value = UI::Format::formatPercent(totalPercent)},
        {.label = CPU_USER_LABEL, .color = scheme.cpuUser, .value = UI::Format::formatPercent(userPercent)},
        {.label = CPU_SYSTEM_LABEL, .color = scheme.cpuSystem, .value = UI::Format::formatPercent(systemPercent)},
    };
    // Absent, not 0 %, where the platform does not report it (Windows, #1031).
    if (iowaitPercent.has_value())
    {
        rows.push_back({.label = CPU_IOWAIT_LABEL, .color = scheme.cpuIowait, .value = UI::Format::formatPercent(*iowaitPercent)});
    }
    rows.push_back({.label = CPU_IDLE_LABEL, .color = scheme.cpuIdle, .value = UI::Format::formatPercent(idlePercent)});
    UI::Widgets::renderHistoryTooltip(ageSeconds, rows);
}

// Every Overview chart reserves the same NowBar columns so their time axes line up: as many as the
// chart with the most bars has. Memory and Resources have three, Power and Battery two; CPU has
// three (Total, User, System) plus I/O Wait where the platform reports it (#1031), so the column is
// one bar wider only there (SystemMetricsPanel::overviewNowBarColumns()).
constexpr size_t OVERVIEW_NOW_BAR_COLUMNS_WITHOUT_IOWAIT = 3;

// Network interface utilities (isVirtualInterface, isBluetoothInterface, getSortedFilteredInterfaces)
// are now in App/Panels/NetInterfaceUtils.h to avoid duplication with NetworkPanel.cpp

} // namespace

SystemMetricsPanel::SystemMetricsPanel() : Panel("System")
{}

std::size_t SystemMetricsPanel::overviewNowBarColumns() const
{
    const bool hasIoWait = (m_Model != nullptr) && m_Model->capabilities().hasIoWait;
    return OVERVIEW_NOW_BAR_COLUMNS_WITHOUT_IOWAIT + (hasIoWait ? 1U : 0U);
}

float SystemMetricsPanel::overviewNowBarColumnWidth() const
{
    // As the Overview lays it out: overviewNowBarColumns() bars with item spacing between them, in
    // a table column that ImGui separates from the plot with CellPadding.x either side (#1207).
    const ImGuiStyle& style = ImGui::GetStyle();
    const auto columns = static_cast<float>(overviewNowBarColumns());
    return (UI::Widgets::nowBarWidth(ImGui::GetFontSize()) * columns) + (style.ItemSpacing.x * (columns - 1.0F)) +
           (style.CellPadding.x * 2.0F);
}
SystemMetricsPanel::~SystemMetricsPanel()
{
    // Mirrors onDetach()'s order/completeness (#782): BackgroundSampler observes the models via
    // weak_ptr, so destruction order among these six members isn't itself a safety requirement
    // today, but keeping the destructor in sync with onDetach() means that invariant doesn't
    // have to be re-verified by hand if a future member's safety ever does depend on order.
    m_Sampler.reset();
    m_GPUPublication.reset();
    m_StoragePublication.reset();
    m_SystemPublication.reset();
    m_GPUModel.reset();
    m_StorageModel.reset();
    m_Model.reset();
}

void SystemMetricsPanel::onAttach()
{
    // m_RefreshInterval and m_MaxHistorySeconds start at the SamplingConfig defaults; ShellLayer
    // raises the configured values as events on its first update (#1079).
    m_HistoryScrollSeconds = 0.0;
    m_ForceRefresh = true;

    m_Model = std::make_shared<Domain::SystemModel>(Platform::makeSystemProbe(), Platform::makePowerProbe());
    m_Model->setMaxHistorySeconds(m_MaxHistorySeconds);
    // Config-file only (not in Settings), so applied once here, before the first refresh (#1291).
    m_Model->setMaxSaneNetworkRate(UserConfig::get().settings().maxSaneRateBps);

    m_StorageModel = std::make_shared<Domain::StorageModel>(Platform::makeDiskProbe());
    m_StorageModel->setMaxHistorySeconds(m_MaxHistorySeconds);

    m_GPUModel = std::make_shared<Domain::GPUModel>(Platform::makeGPUProbe());
    m_GPUModel->setMaxHistorySeconds(m_MaxHistorySeconds);

    // Initial refresh to seed histories
    m_Model->refresh();
    m_StorageModel->sample();
    if (m_GPUModel)
    {
        m_GPUModel->refresh();
    }

    Domain::SamplerConfig samplerCfg;
    samplerCfg.interval = m_RefreshInterval;
    samplerCfg.firstSampleAfterInterval = true; // seeded synchronously above (#1102)
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(samplerCfg);
    m_Sampler->addSamplable(m_Model);
    m_Sampler->addSamplable(m_StorageModel);
    if (m_GPUModel)
    {
        m_Sampler->addSamplable(m_GPUModel);
    }
    m_Sampler->start();

    m_SystemPublication = m_Model->publication();
    m_ChartDataGeneration = UI::Widgets::nextChartDataGeneration();
    m_StoragePublication = m_StorageModel->publication();
    m_GPUPublication = m_GPUModel ? m_GPUModel->publication() : nullptr;
    m_TimestampsCache = m_SystemPublication->timestamps;
    if (!m_TimestampsCache.empty())
    {
        m_CurrentNowSeconds = m_TimestampsCache.back();
    }
    else
    {
        m_CurrentNowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    m_ForceRefresh = false;

    m_CachedSnapshot = m_SystemPublication->snapshot;
    // NOTE: m_Hostname intentionally stores the raw hostname without any icon prefix.
    // UI code (e.g., tab labels) is responsible for adding icons when rendering.
    m_Hostname = m_CachedSnapshot.hostname.empty() ? "System" : m_CachedSnapshot.hostname;
}

void SystemMetricsPanel::onDetach()
{
    m_Sampler.reset();
    m_ProcessModel.reset();
    m_GPUPublication.reset();
    m_StoragePublication.reset();
    m_SystemPublication.reset();
    m_GPUModel.reset();
    m_StorageModel.reset();
    m_Model.reset();
}

void SystemMetricsPanel::setSamplingInterval(std::chrono::milliseconds interval, bool forceSample)
{
    if (interval == m_RefreshInterval)
    {
        return;
    }
    m_RefreshInterval = interval;
    if (m_Sampler)
    {
        m_Sampler->setInterval(interval);
    }
    if (forceSample)
    {
        m_ForceRefresh = true;
        requestRefresh(); // Consume flag semantics for older calls
    }
}

void SystemMetricsPanel::requestRefresh()
{
    if (m_Sampler)
    {
        m_Sampler->requestRefresh();
    }
    m_ForceRefresh = false; // Consumed since sampler will refresh
}

void SystemMetricsPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            m_IsActiveTab = (e.tabName() == "SystemOverview");
            return false;
        });
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            // The startup value (#1079) is applied without forcing a sample: the models were just
            // seeded, so one now would cover only a few ms (#1102).
            setSamplingInterval(std::chrono::milliseconds(e.getIntervalMs()), !e.isInitial());
            return false;
        });
    dispatcher.dispatch<Core::HistoryDurationChangedEvent>(
        [this](Core::HistoryDurationChangedEvent& e)
        {
            // Clamped as the models clamp it, so the charts' axes span the window the models keep (#1145).
            const double seconds = Domain::Sampling::clampHistorySeconds(Domain::Numeric::toDouble(e.getSeconds()));
            // Whole seconds, so anything under half a second apart is the same setting.
            if (std::abs(seconds - m_MaxHistorySeconds) < 0.5)
            {
                return false; // unchanged: nothing to trim or refresh
            }
            m_MaxHistorySeconds = seconds;
            if (m_Model)
            {
                m_Model->setMaxHistorySeconds(m_MaxHistorySeconds);
            }
            if (m_StorageModel)
            {
                m_StorageModel->setMaxHistorySeconds(m_MaxHistorySeconds);
            }
            if (m_GPUModel)
            {
                m_GPUModel->setMaxHistorySeconds(m_MaxHistorySeconds);
            }
            // Republish promptly for a user's change; not for the startup value, just after the seed (#1102).
            m_ForceRefresh = m_ForceRefresh || !e.isInitial();
            return false; // Allow others to react
        });
}

void SystemMetricsPanel::onUpdate(float deltaTime)
{
    m_LastDeltaSeconds = deltaTime;

    if (!m_Model)
    {
        return;
    }

    // Do not throttle the background sampler just because the user is resizing the UI.
    const auto effectiveInterval = AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(m_RefreshInterval, m_IsActiveTab, false);
    if (m_Sampler && m_Sampler->interval() != effectiveInterval)
    {
        m_Sampler->setInterval(effectiveInterval);
    }

    if (m_ForceRefresh)
    {
        if (m_Sampler)
        {
            m_Sampler->requestRefresh();
        }
        m_ForceRefresh = false;
    }

    if (!m_SystemPublication || m_Model->publicationVersion() != m_SystemPublication->version)
    {
        m_SystemPublication = m_Model->publication();
        m_ChartDataGeneration = UI::Widgets::nextChartDataGeneration();
        m_TimestampsCache = m_SystemPublication->timestamps;
        if (!m_TimestampsCache.empty())
        {
            m_CurrentNowSeconds = m_TimestampsCache.back();
        }
        else
        {
            m_CurrentNowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        m_CachedSnapshot = m_SystemPublication->snapshot;
        if (!m_CachedSnapshot.hostname.empty())
        {
            m_Hostname = m_CachedSnapshot.hostname;
        }
    }
    if (m_StorageModel && (!m_StoragePublication || m_StorageModel->publicationVersion() != m_StoragePublication->version))
    {
        m_StoragePublication = m_StorageModel->publication();
        m_ChartDataGeneration = UI::Widgets::nextChartDataGeneration();
    }
    if (m_GPUModel && (!m_GPUPublication || m_GPUModel->publicationVersion() != m_GPUPublication->version))
    {
        m_GPUPublication = m_GPUModel->publication();
        m_ChartDataGeneration = UI::Widgets::nextChartDataGeneration();
    }
    if (const auto processModel = m_ProcessModel.lock(); processModel != nullptr)
    {
        Domain::ProcessSystemHistories histories;
        if (processModel->tryCopySystemHistoriesIfNewer(m_ProcessHistoryVersion, histories))
        {
            m_ProcessHistoryVersion = histories.version;
            m_ProcessHistoryTimestamps = std::move(histories.timestamps);
            m_ProcessPowerHistory = std::move(histories.power);
            m_ProcessPageFaultsHistory = std::move(histories.pageFaults);
            m_ProcessThreadCountHistory = std::move(histories.threadCount);
            m_ProcessHandleCountHistory = std::move(histories.handleCount);
            m_ProcessCapabilities = histories.capabilities; // current, not the startup set (#1254)
            m_ChartDataGeneration = UI::Widgets::nextChartDataGeneration();
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

    // Skip rendering when tab is inactive (data collection continues in onUpdate)
    if (!m_IsActiveTab)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    if (theme.currentFontSize() != m_LastFontSize)
    {
        m_LastFontSize = theme.currentFontSize();
        m_LayoutDirty = true;
    }

    // A reference, not a copy: m_CachedSnapshot is only reassigned in onUpdate(), never while
    // rendering, and a copy duplicated every per-core and per-interface vector each frame (#1017).
    const auto& snap = m_CachedSnapshot;

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

    // Add padding inside tabs for better spacing, scaled like the style it overrides (#971)
    const float tabPaddingScale = theme.styleScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ShellMetrics::TAB_PADDING_X * tabPaddingScale, ShellMetrics::SUB_TAB_PADDING_Y * tabPaddingScale));

    if (ImGui::BeginTabBar("SystemTabs", ImGuiTabBarFlags_DrawSelectedOverline))
    {
        // Each tab's body scrolls in its own child, so the tab bar itself stays in view (#968).
        if (ImGui::BeginTabItem(ICON_FA_GAUGE_HIGH "  Overview"))
        {
            {
                const UI::Widgets::TabContentScope content("##OverviewContent");
                renderOverview();
            }
            ImGui::EndTabItem();
        }

        if (CpuCoresSection::showCpuCoresTab(snap.seenCoreIds, static_cast<std::size_t>(snap.coreCount)))
        {
            if (ImGui::BeginTabItem(ICON_FA_MICROCHIP "  CPU Cores"))
            {
                // Build context for CpuCoresSection render function
                CpuCoresSection::RenderContext cpuCtx{
                    .publication = m_SystemPublication.get(),
                    .chartDataGeneration = m_ChartDataGeneration,
                    .maxHistorySeconds = m_MaxHistorySeconds,
                    .historyScrollSeconds = m_HistoryScrollSeconds,
                    .lastDeltaSeconds = m_LastDeltaSeconds,
                    .refreshInterval = m_RefreshInterval,
                    .smoothedPerCore = &m_SmoothedPerCore,
                };
                {
                    const UI::Widgets::TabContentScope content("##CpuCoresContent");
                    CpuCoresSection::renderCpuCoresSection(cpuCtx);
                }
                ImGui::EndTabItem();
            }
        }

        // GPU tab - always available; content handles missing GPUs gracefully
        if (m_GPUModel)
        {
            if (ImGui::BeginTabItem(ICON_FA_MICROCHIP "  GPU"))
            {
                // Build context for GpuSection render function
                GpuSection::RenderContext gpuCtx{
                    .publication = m_GPUPublication.get(),
                    .chartDataGeneration = m_ChartDataGeneration,
                    .maxHistorySeconds = m_MaxHistorySeconds,
                    .historyScrollSeconds = m_HistoryScrollSeconds,
                    .lastDeltaSeconds = m_LastDeltaSeconds,
                    .refreshInterval = m_RefreshInterval,
                    .smoothedGPUs = &m_SmoothedGPUs,
                    .cache = &m_GpuFrameCache,
                };
                {
                    const UI::Widgets::TabContentScope content("##GpuContent");
                    UI::Widgets::FillPlotLayout fill(m_GpuFill);
                    gpuCtx.fill = &fill;
                    GpuSection::renderGpuSection(gpuCtx);
                }
                ImGui::EndTabItem();
            }
        }

        // Network and I/O tab - show if network counters are available
        if (m_Model != nullptr && m_Model->capabilities().hasNetworkCounters)
        {
            if (ImGui::BeginTabItem(ICON_FA_NETWORK_WIRED "  Network and I/O"))
            {
                // Build context for NetworkSection render functions
                NetworkSection::RenderContext netCtx{
                    .systemPublication = m_SystemPublication.get(),
                    .storagePublication = m_StoragePublication.get(),
                    .chartDataGeneration = m_ChartDataGeneration,
                    .hasNetworkCounters = m_Model != nullptr && m_Model->capabilities().hasNetworkCounters,
                    .maxHistorySeconds = m_MaxHistorySeconds,
                    .historyScrollSeconds = m_HistoryScrollSeconds,
                    .lastDeltaSeconds = m_LastDeltaSeconds,
                    .refreshInterval = m_RefreshInterval,
                    .smoothedDiskReadBytesPerSec = &m_SmoothedSystemIO.readBytesPerSec,
                    .smoothedDiskWriteBytesPerSec = &m_SmoothedSystemIO.writeBytesPerSec,
                    .smoothedDiskInitialized = &m_SmoothedSystemIO.initialized,
                    .smoothedPerDisk = &m_SmoothedPerDisk,
                    .smoothedNetSentBytesPerSec = &m_SmoothedNetwork.sentBytesPerSec,
                    .smoothedNetRecvBytesPerSec = &m_SmoothedNetwork.recvBytesPerSec,
                    .smoothedNetInitialized = &m_SmoothedNetwork.initialized,
                    .selectedNetworkInterface = &m_SelectedNetworkInterface,
                    .showAllInterfaces = &m_ShowAllInterfaces,
                    .interfacesWithTraffic = &m_InterfacesWithTraffic,
                    .fillState = &m_NetworkFill,
                    .cache = &m_NetworkFrameCache,
                };
                {
                    const UI::Widgets::TabContentScope content("##NetworkContent");
                    NetworkSection::renderNetworkSection(netCtx);
                }
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    ImGui::PopStyleVar(); // FramePadding
}

void SystemMetricsPanel::renderOverview()
{
    const auto& snap = m_CachedSnapshot; // See renderContent() (#1017)
    // Held for the frame: ProcessesPanel owns the model and may already have released it (#1176).
    const std::shared_ptr<Domain::ProcessModel> processModel = m_ProcessModel.lock();

    // Every chart on this tab shares the height available, between a font-relative minimum and
    // maximum (UI/HistoryPlotHeight.h), instead of a fixed 180px that left up to a third of a tall
    // window empty (#922). FillPlotLayout measures the non-plot content as this function renders
    // and feeds it to the next frame.
    UI::Widgets::FillPlotLayout fill(m_OverviewFill);
    const float plotHeight = fill.plotHeight();
    // The CPU, Memory, Power and Resources charts share their plot edges, though only Resources
    // (and Power with a battery) has a right-hand axis (#1206). All reserve overviewNowBarColumns().
    const UI::Widgets::AlignedChartStack alignedCharts("##OverviewCharts");

    updateSmoothedCpu(snap, m_LastDeltaSeconds);
    updateSmoothedMemory(snap, m_LastDeltaSeconds);

    // Header line: CPU Model | Cores | Freq | Uptime (right-aligned). Its strings come from the
    // publications and the process count, so they are rebuilt only when one of those changes (#1171).
    const std::size_t processCount = (processModel != nullptr) ? processModel->processCount() : 0;
    const std::uint64_t systemVersion = m_SystemPublication ? m_SystemPublication->version : 0;
    const std::uint64_t gpuVersion = m_GPUPublication ? m_GPUPublication->version : 0;
    if (!m_OverviewHeader.valid || m_OverviewHeader.systemVersion != systemVersion || m_OverviewHeader.gpuVersion != gpuVersion ||
        m_OverviewHeader.processCount != processCount || m_OverviewHeader.hasProcessModel != (processModel != nullptr))
    {
        // Built in a fresh OverviewHeaderText and moved in whole, validity last: a render exception is
        // caught and the app carries on, so a rebuild that throws part-way must leave the cache stale.
        OverviewHeaderText fresh;
        fresh.uptime = UI::Format::formatUptimeShort(snap.uptimeSeconds);

        // Display: "CPU Model (N logical processors @ X.XX GHz)     Uptime: Xd Yh Zm"
        // The same summary as the CPU Cores header (#1180).
        fresh.coreInfo = Detail::cpuCoreSummary(snap.coreCount, snap.cpuFreqMHz);

        fresh.processes =
            (processModel != nullptr) ? std::format("Processes: {}", UI::Format::formatIntLocalized(processCount)) : std::string{};

        // Total dedicated VRAM: discrete GPUs only, an integrated GPU's "memory" being system RAM (#1114).
        const std::uint64_t totalVramBytes = m_GPUPublication ? GpuSection::totalDedicatedVramBytes(m_GPUPublication->snapshots) : 0;
        // RAM and VRAM, appended to the CPU line
        fresh.memory = (totalVramBytes > 0) ? std::format(", {} RAM, {} VRAM",
                                                          UI::Format::formatBytes(static_cast<double>(snap.memoryTotalBytes)),
                                                          UI::Format::formatBytes(static_cast<double>(totalVramBytes)))
                                            : std::format(", {} RAM", UI::Format::formatBytes(static_cast<double>(snap.memoryTotalBytes)));
        fresh.systemVersion = systemVersion;
        fresh.gpuVersion = gpuVersion;
        fresh.processCount = processCount;
        fresh.hasProcessModel = (processModel != nullptr);
        fresh.valid = true;
        m_OverviewHeader = std::move(fresh);
    }
    const std::string& uptimeStr = m_OverviewHeader.uptime;
    const std::string& coreInfo = m_OverviewHeader.coreInfo;
    const std::string& processStr = m_OverviewHeader.processes;
    const std::string& memoryStr = m_OverviewHeader.memory;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float uptimeWidth = uptimeStr.empty() ? 0.0F : ImGui::CalcTextSize(uptimeStr.c_str()).x;
    const float processWidth = processStr.empty() ? 0.0F : ImGui::CalcTextSize(processStr.c_str()).x;
    const float spacer = (!processStr.empty() && !uptimeStr.empty()) ? style.ItemSpacing.x : 0.0F;
    const float rightBlockWidth = uptimeWidth + processWidth + spacer;

    // CPU model with core count, frequency, RAM, and VRAM
    ImGui::TextUnformatted(snap.cpuModel.c_str());
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(coreInfo.c_str());
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(memoryStr.c_str());

    // Right-align process count and uptime -- beside the CPU summary when both fit, otherwise on
    // a line of their own. Positioned from the right edge alone they were drawn straight over the
    // summary on a window too narrow for both (#967).
    if (rightBlockWidth > 0.0F)
    {
        // Window-local X throughout: the space ImGui::SameLine() and SetCursorPosX() work in. The
        // right edge is therefore the line's start plus the width available from it -- availWidth
        // on its own is a width, and used as a position it stopped a padding's width short of the
        // content's right edge, which is where this block had always been drawn.
        const float lineStartX = ImGui::GetCursorStartPos().x;
        const float summaryEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
        const auto placement = UI::LineLayout::placeTrailingBlock(
            lineStartX, summaryEndX, lineStartX + availWidth, rightBlockWidth, style.ItemSpacing.x * 2.0F);
        if (placement.sameLine)
        {
            ImGui::SameLine(placement.x);
        }
        else
        {
            ImGui::SetCursorPosX(placement.x);
        }
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

    const auto& cpuHist = m_SystemPublication->cpuHistory;
    const auto& cpuUserHist = m_SystemPublication->cpuUserHistory;
    const auto& cpuSystemHist = m_SystemPublication->cpuSystemHistory;
    const auto& cpuIowaitHist = m_SystemPublication->cpuIowaitHistory;
    const auto& cpuIdleHist = m_SystemPublication->cpuIdleHistory;
    const auto& timestamps = m_TimestampsCache;
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)
    const auto axisConfig = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, m_HistoryScrollSeconds);

    const size_t cpuCount = std::min(cpuHist.size(), timestamps.size());
    const auto cpuData = UI::Widgets::tailAlignedSpan(cpuHist, cpuCount).values;
    // CPU history with vertical now bars (total + breakdown)
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MICROCHIP "  CPU Usage (%zu samples)", cpuCount);

    const auto cpuTimeData = frameTimeAxis(timestamps, cpuCount, nowSeconds);

    const size_t breakdownCount =
        std::min({cpuUserHist.size(), cpuSystemHist.size(), cpuIowaitHist.size(), cpuIdleHist.size(), timestamps.size()});
    const auto cpuUserData = UI::Widgets::tailAlignedSpan(cpuUserHist, breakdownCount).values;
    const auto cpuSystemData = UI::Widgets::tailAlignedSpan(cpuSystemHist, breakdownCount).values;
    const auto cpuIowaitData = UI::Widgets::tailAlignedSpan(cpuIowaitHist, breakdownCount).values;
    const auto cpuIdleData = UI::Widgets::tailAlignedSpan(cpuIdleHist, breakdownCount).values;
    const auto breakdownTimeData = frameTimeAxis(timestamps, breakdownCount, nowSeconds);

    // I/O Wait is drawn -- fill, strip entry, tooltip row and bar -- only where the platform
    // reports it. Windows does not, and showed a permanently empty series and bar (#1031).
    const bool showIowait = (m_Model != nullptr) && m_Model->capabilities().hasIoWait;

    auto cpuPlot = [&]()
    {
        // The Total line is drawn from the adopted publication, so its reduction is kept until
        // m_ChartDataGeneration next changes (HistoryChartConfig::dataGeneration, #1139).
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(UI::Widgets::percentHistoryConfig("##OverviewCPUHistory", axisConfig.xMin, axisConfig.xMax),
                                    plotHeight),
            m_ChartDataGeneration));
        if (chart.active())
        {
            UI::Widgets::drawCollectingHint(cpuData.size()); // The same "no data yet" state on every chart (#1013)
            if (breakdownCount > 0)
            {
                // The bands are drawn with ImPlot directly, so they are capped here like every
                // plotLineWithFill series (#1022), reduced together so they still line up. Points are
                // chosen by each band's own value (User is its own top), not by the cumulative tops:
                // a System spike while User falls by as much leaves System's top flat, and would be
                // dropped if the tops chose the points.
                //
                // The choice of points is kept until m_ChartDataGeneration changes (#1139): reducing
                // the whole history and copying every band out of it each frame was most of this
                // chart's cost at the largest history settings. Each frame only builds the kept
                // points, at most LINE_PLOT_MAX_POINTS_DENSE of them.
                const std::span<const UI::Widgets::ReducedPoint> points =
                    m_CpuStackReduction.points({.generation = m_ChartDataGeneration,
                                                .dataId = 0,
                                                .count = breakdownCount,
                                                .maxOut = UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE},
                                               [&](std::vector<UI::Widgets::ReducedPoint>& out)
                                               {
                                                   UI::Widgets::reduceAlignedPoints<float>(breakdownTimeData,
                                                                                           {cpuUserData, cpuSystemData, cpuIowaitData},
                                                                                           UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE,
                                                                                           nowSeconds,
                                                                                           out);
                                               });

                auto& y0 = m_CpuStackY0;
                auto& yUserTop = m_CpuStackYUser;
                auto& ySystemTop = m_CpuStackYSystem;
                auto& yIowaitTop = m_CpuStackYIowait;
                auto& yBusyTop = m_CpuStackYBusy;
                m_CpuStackX.resize(points.size());
                y0.assign(points.size(), 0.0);
                yUserTop.resize(points.size());
                ySystemTop.resize(points.size());
                yIowaitTop.resize(points.size());
                yBusyTop.resize(points.size());
                for (std::size_t k = 0; k < points.size(); ++k)
                {
                    // A gap point is NaN in every band (see UI::Widgets::reduceAlignedSeries).
                    const auto i = static_cast<std::size_t>(points[k].index);
                    m_CpuStackX[k] = breakdownTimeData[i];
                    if (points[k].gap)
                    {
                        yUserTop[k] = ySystemTop[k] = yIowaitTop[k] = yBusyTop[k] = std::numeric_limits<double>::quiet_NaN();
                        continue;
                    }
                    // PlotShaded fills between two Y series, so the stack needs cumulative tops.
                    yUserTop[k] = static_cast<double>(cpuUserData[i]);
                    ySystemTop[k] = yUserTop[k] + static_cast<double>(cpuSystemData[i]);
                    // I/O Wait is idle time, not busy (#1157): its band sits on the busy total
                    // (100 - idle - iowait, which the Total line follows) rather than on System, so
                    // the Total line runs along its bottom edge instead of through it.
                    yIowaitTop[k] = 100.0 - static_cast<double>(cpuIdleData[i]);
                    yBusyTop[k] = yIowaitTop[k] - static_cast<double>(cpuIowaitData[i]);
                }

                // The bands reach "now" like every plotLineWithFill series: the last sample held to
                // x = 0 (#1016), unless it is too old to pass for current (#1147).
                UI::Widgets::holdLastValuesToNow(m_CpuStackX,
                                                 {&y0, &yUserTop, &ySystemTop, &yIowaitTop, &yBusyTop},
                                                 UI::Widgets::maxHoldSecondsForAxis(breakdownTimeData));
                const int stackCount = UI::Format::checkedCount(m_CpuStackX.size());

                // ImPlot's shaded renderer has no NaN handling, so each band is filled run by run over
                // the points where both of its edges have a reading: a gap point (a missed sample) or
                // a band with no reading is drawn as a gap, not as triangles through NaN (#1149).
                const auto shadeBand =
                    [&](const char* label, const std::vector<double>& lower, const std::vector<double>& upper, const ImVec4& fillColor)
                {
                    UI::Widgets::forEachJointFiniteRun(
                        lower.data(),
                        upper.data(),
                        stackCount,
                        [&](int runStart, int runLength)
                        {
                            const auto at = static_cast<std::size_t>(runStart);
                            ImPlot::PlotShaded(
                                label, &m_CpuStackX[at], &lower[at], &upper[at], runLength, {ImPlotProp_FillColor, fillColor});
                        });
                };
                shadeBand(CPU_USER_LABEL, y0, yUserTop, theme.scheme().cpuUserFill);
                shadeBand(CPU_SYSTEM_LABEL, yUserTop, ySystemTop, theme.scheme().cpuSystemFill);
                if (showIowait)
                {
                    shadeBand(CPU_IOWAIT_LABEL, yBusyTop, yIowaitTop, theme.scheme().cpuIowaitFill);
                }

                // An edge along the top of each band, under the band's own label, in its opaque
                // colour (#1192). The bands are the chart's fill, so each edge is drawn as a secondary
                // series: its own marker shape, shown on its value-strip swatch too, so the bands
                // differ by more than colour (#1198).
                const auto bandEdge = [&](const char* label, const std::vector<double>& top, const ImVec4& color, std::size_t slot)
                {
                    const UI::Widgets::SeriesStyle style = seriesStyle(SeriesRole::Secondary, slot);
                    ImPlot::PlotLine(label,
                                     m_CpuStackX.data(),
                                     top.data(),
                                     stackCount,
                                     {ImPlotProp_LineColor, color, ImPlotProp_LineWeight, UI::Widgets::lineWeight(style.lineWeightPx)});
                    UI::Widgets::plotSeriesMarkers(label, m_CpuStackX.data(), top.data(), stackCount, color, style);
                };
                bandEdge(CPU_USER_LABEL, yUserTop, theme.scheme().cpuUser, 0);
                bandEdge(CPU_SYSTEM_LABEL, ySystemTop, theme.scheme().cpuSystem, 1);
                if (showIowait)
                {
                    bandEdge(CPU_IOWAIT_LABEL, yIowaitTop, theme.scheme().cpuIowait, 2);
                }

                // Total over the busy bands. It is 100 - (idle + iowait), so it includes irq, softirq
                // and steal time the User/System bands do not: without it the "CPU Total" bar had no
                // series, and the top of the stack understated the load whenever that other time was
                // significant. The I/O Wait band sits on top of it.
                if (!cpuData.empty())
                {
                    // The primary series' weight, but no fill of its own: the bands below are the fill.
                    plotLineWithFill(CPU_TOTAL_LABEL,
                                     cpuTimeData.data(),
                                     cpuData.data(),
                                     UI::Format::checkedCount(cpuData.size()),
                                     theme.scheme().chartCpu,
                                     theme.scheme().chartCpuFill,
                                     UI::Widgets::PRIMARY_SERIES_WEIGHT,
                                     false);
                }

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto si = hoveredIndexFromPlotX(breakdownTimeData, mouse.x))
                    {
                        // Total comes from its own series, looked up at the same moment.
                        const auto totalIdx = hoveredIndexFromPlotX(cpuTimeData, mouse.x);
                        showCpuBreakdownTooltip(theme.scheme(),
                                                static_cast<double>(breakdownTimeData[*si]),
                                                totalIdx ? cpuData[*totalIdx]
                                                         : (100.0F - cpuIdleData[*si] - (showIowait ? cpuIowaitData[*si] : 0.0F)),
                                                cpuUserData[*si],
                                                cpuSystemData[*si],
                                                showIowait ? std::optional<float>(cpuIowaitData[*si]) : std::nullopt,
                                                cpuIdleData[*si]);
                    }
                }
            }
        }
    };

    NowBarList cpuBars;
    cpuBars.push_back({.valueText = UI::Format::formatPercent(m_SmoothedCpu.total),
                       .label = CPU_TOTAL_LABEL,
                       .tooltipText = {},
                       .value01 = UI::Format::percent01(m_SmoothedCpu.total),
                       .color = theme.scheme().chartCpu}); // The Total line's colour (#1192)
    cpuBars.push_back({.valueText = UI::Format::formatPercent(m_SmoothedCpu.user),
                       .label = CPU_USER_LABEL,
                       .tooltipText = {},
                       .value01 = UI::Format::percent01(m_SmoothedCpu.user),
                       .color = theme.scheme().cpuUser});
    cpuBars.push_back({.valueText = UI::Format::formatPercent(m_SmoothedCpu.system),
                       .label = CPU_SYSTEM_LABEL,
                       .tooltipText = {},
                       .value01 = UI::Format::percent01(m_SmoothedCpu.system),
                       .color = theme.scheme().cpuSystem});
    if (showIowait)
    {
        cpuBars.push_back({
            .valueText = UI::Format::formatPercent(m_SmoothedCpu.iowait),
            .label = CPU_IOWAIT_LABEL,
            .tooltipText = {},
            .value01 = UI::Format::percent01(m_SmoothedCpu.iowait),
            .color = theme.scheme().cpuIowait,
        });
    }

    renderHistoryWithNowBars("OverviewCPUHistoryLayout", plotHeight, cpuPlot, cpuBars, false, overviewNowBarColumns());
    fill.addPlot();

    ImGui::Spacing();

    // Memory & Swap history section
    {
        MemorySection::RenderContext memCtx{
            .publication = m_SystemPublication.get(),
            .chartDataGeneration = m_ChartDataGeneration,
            .maxHistorySeconds = m_MaxHistorySeconds,
            .historyScrollSeconds = m_HistoryScrollSeconds,
            .lastDeltaSeconds = m_LastDeltaSeconds,
            .refreshInterval = m_RefreshInterval,
            .smoothedMemory = &m_SmoothedMemory,
            .plotHeight = plotHeight,
        };
        MemorySection::renderMemorySection(memCtx, timestamps, nowSeconds, static_cast<int>(overviewNowBarColumns()));
        fill.addPlot();
        ImGui::Spacing();
    }

    // Power & Battery history chart (combines per-process power aggregation with battery charge %).
    // Power is drawn only where the process probe actually measures it: on Windows it does not, and
    // used to show a fabricated figure (#1028). Without it the chart is a plain Battery chart.
    const bool hasProcessPower = (processModel != nullptr) && m_ProcessCapabilities.hasPowerUsage;
    if (hasProcessPower || snap.power.hasBattery)
    {
        // Get power history from ProcessModel (aggregated per-process power)
        // Get battery charge history from SystemModel
        const auto& batteryHistFloat = m_SystemPublication->batteryChargeHistory;

        // Each series against the timestamps of the sampler that produced it. Power is aggregated
        // by ProcessModel and battery charge is read by SystemModel; the two run on their own
        // intervals and phases, so drawing battery against the process timestamps (as this once
        // did) put every battery sample at the wrong time and paired mismatched samples in the
        // tooltip.
        const size_t powerCount = hasProcessPower ? std::min(m_ProcessPowerHistory.size(), m_ProcessHistoryTimestamps.size()) : 0;
        const size_t batteryCount = std::min(batteryHistFloat.size(), timestamps.size());
        const size_t alignedCount = std::max(powerCount, batteryCount);

        // Rendered from the first frame, empty and showing the collecting hint until samples arrive,
        // like every other chart; it used to appear only once it had data (#1013).
        {
            // A view into the power history, plotted as doubles -- no per-frame copy (#1018).
            const auto powerHist = UI::Widgets::tailAlignedSpan(m_ProcessPowerHistory, powerCount).values;

            // Battery history, with the model's "no reading" value (-1) as NaN: a gap in the line,
            // not a dive to 0 %. Rebuilt into a member each frame, reusing its capacity (#1171).
            std::vector<float>& batteryHist = m_BatteryChartHistory;
            batteryHist.clear();
            if (batteryCount > 0)
            {
                const auto startIt = batteryHistFloat.end() - static_cast<std::ptrdiff_t>(batteryCount);
                for (auto it = startIt; it != batteryHistFloat.end(); ++it)
                {
                    batteryHist.push_back(*it >= 0.0F ? *it : std::numeric_limits<float>::quiet_NaN());
                }
            }

            const auto powerTimeData = frameTimeAxis(m_ProcessHistoryTimestamps, powerCount, nowSeconds);
            const auto batteryTimeData = frameTimeAxis(timestamps, batteryCount, nowSeconds);
            const auto axis = makeTimeAxisConfig(timestamps, m_MaxHistorySeconds, m_HistoryScrollSeconds);
            // Update smoothed values: the latest *reading*, skipping trailing gaps.
            const float targetPower = powerHist.empty() ? 0.0F : static_cast<float>(powerHist.back()); // updateSmoothedPower takes float
            const auto finiteBattery = batteryHist | std::views::reverse;
            const auto lastBattery = std::ranges::find_if(finiteBattery, [](float v) { return !std::isnan(v); });
            const float targetBattery = (lastBattery != finiteBattery.end()) ? *lastBattery : 0.0F;
            if (alignedCount > 0)
            {
                updateSmoothedPower(targetPower, targetBattery, m_LastDeltaSeconds);
            }

            // One upper bound for the power axis and its bar, so the bar and line agree (#1003). Sized
            // to the samples the window shows, not the one trimming keeps left of it (#1145), and to the
            // bar's smoothed value, which can still be easing down from a peak that has just left it.
            const double powerAxisUpper = UI::Widgets::easedRateAxisUpperBound(
                "##PowerBatteryHistory",
                UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(powerTimeData, axis.xMin, powerHist),
                                               {UI::Widgets::currentIfAvailable(hasProcessPower, m_SmoothedPower.watts)}),
                UI::Widgets::RATE_AXIS_MIN_SPAN_WATTS);

            // Beside Power, Battery is drawn on the right-hand axis; alone it takes the primary one.
            const char* const batteryLabel = hasProcessPower ? BATTERY_Y2_LABEL : BATTERY_LABEL;

            // Build NowBars
            NowBarList bars;
            if (hasProcessPower)
            {
                bars.push_back({
                    .valueText = powerHist.empty() ? UI::Format::formatPowerCompact(m_SmoothedPower.watts)
                                                   : UI::Format::formatPowerOrZero(m_SmoothedPower.watts),
                    .label = POWER_LABEL,
                    .tooltipText = {},
                    .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedPower.watts, powerAxisUpper),
                    .color = theme.scheme().chartCpu,
                });
            }

            if (snap.power.hasBattery)
            {
                // The battery's status -- on AC, charging, time left -- is its strip entry's text
                // ("Battery: <plug> <battery> 94% (not charging)"), on the heading's line like every
                // chart's value strip; it was a separate right-aligned status there.
                bars.push_back({.valueText = UI::Format::formatPercent(m_SmoothedPower.batteryChargePercent),
                                .label = batteryLabel,
                                .tooltipText = UI::Widgets::tooltipRowText(batteryLabel, Detail::batteryHeaderStatus(snap.power)),
                                // Scaled to the battery axis's top, headroom included, so the bar meets the line.
                                .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedPower.batteryChargePercent,
                                                                                UI::Widgets::PERCENT_AXIS_UPPER_WITH_HEADROOM),
                                .color = theme.scheme().chartMemory});
            }

            auto plot = [&]()
            {
                // Primary Y-axis: Power (Watts), pinned to 0 at the bottom, with Battery on Y2. Without
                // power, Battery is the only series and takes the primary axis as a percentage, so no
                // Watts axis is left labelling nothing.
                const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                    UI::Widgets::withHeight(
                        hasProcessPower ? UI::Widgets::rateHistoryConfigWithUpper(
                                              "##PowerBatteryHistory", axis.xMin, axis.xMax, formatAxisWatts, powerAxisUpper)
                                        : UI::Widgets::percentHistoryConfigWithHeadroom("##PowerBatteryHistory", axis.xMin, axis.xMax),
                        plotHeight),
                    m_ChartDataGeneration));
                if (chart.active())
                {
                    // Secondary Y-axis: Battery % (0-100), labelled like any second axis (#1206). Its
                    // ticks were hidden to keep the time axis aligned with the charts above, which left
                    // the battery line reading against a Watts axis; the stack is aligned by
                    // AlignedChartStack now instead. Drawn a little past 100 % with ticks up to 100, so a full
                    // battery's line sits below the top edge instead of on it (#1300).
                    if (hasProcessPower && snap.power.hasBattery && !batteryHist.empty())
                    {
                        UI::Widgets::setupSecondaryRateAxis(UI::Widgets::PERCENT_AXIS_UPPER_WITH_HEADROOM,
                                                            UI::Widgets::formatAxisPercent,
                                                            theme.scheme().chartMemory,
                                                            100.0);
                    }
                    // After all axis setup: the hint reads the plot's geometry, which locks setup (#1013).
                    UI::Widgets::drawCollectingHint(alignedCount);

                    // Plot power on primary Y-axis
                    if (!powerHist.empty())
                    {
                        plotSeries(POWER_LABEL,
                                   powerTimeData.data(),
                                   powerHist.data(),
                                   UI::Format::checkedCount(powerHist.size()),
                                   theme.scheme().chartCpu,
                                   theme.scheme().chartCpuFill,
                                   seriesStyle(SeriesRole::Primary));
                    }

                    // Plot battery charge on secondary Y-axis
                    if (snap.power.hasBattery && !batteryHist.empty())
                    {
                        ImPlot::SetAxes(ImAxis_X1, hasProcessPower ? ImAxis_Y2 : ImAxis_Y1);
                        plotSeries(batteryLabel,
                                   batteryTimeData.data(),
                                   batteryHist.data(),
                                   UI::Format::checkedCount(batteryHist.size()),
                                   theme.scheme().chartMemory,
                                   theme.scheme().chartMemoryFill,
                                   // Alone (no power) it is the chart's primary series, and filled.
                                   hasProcessPower ? seriesStyle(SeriesRole::Secondary, 0) : seriesStyle(SeriesRole::Primary));
                        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1); // Reset to primary
                    }

                    // Tooltip
                    if (ImPlot::IsPlotHovered())
                    {
                        // Each series has its own time axis, so each is looked up on its own: the
                        // nearest power sample and the nearest battery sample to the pointer.
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        const auto powerIdx = hoveredIndexFromPlotX(powerTimeData, mouse.x);
                        const auto batteryIdx =
                            snap.power.hasBattery ? hoveredIndexFromPlotX(batteryTimeData, mouse.x) : std::optional<size_t>{};
                        if (powerIdx || batteryIdx)
                        {
                            std::vector<UI::Widgets::TooltipRow> rows;
                            if (powerIdx)
                            {
                                rows.push_back({.label = POWER_LABEL,
                                                .color = theme.scheme().chartCpu,
                                                .value = UI::Format::formatPowerOrZero(Domain::Numeric::toDouble(powerHist[*powerIdx]))});
                            }
                            if (batteryIdx)
                            {
                                const double batteryVal = Domain::Numeric::toDouble(batteryHist[*batteryIdx]);
                                rows.push_back({.label = batteryLabel,
                                                .color = theme.scheme().chartMemory,
                                                .value = UI::Widgets::formatSampleOrNA(
                                                    batteryVal, [](double v) { return UI::Format::formatPercent(v); })});
                            }
                            UI::Widgets::renderHistoryTooltip(powerIdx ? powerTimeData[*powerIdx] : batteryTimeData[*batteryIdx], rows);
                        }
                    }
                }
            };

            // Chart heading with the sample count; the battery's status is in its value-strip entry.
            std::string heading;
            if (!snap.power.hasBattery)
            {
                heading = std::format(ICON_FA_BOLT "  Power ({} samples)", alignedCount);
            }
            else if (hasProcessPower)
            {
                heading = std::format(ICON_FA_BOLT "  Power & Battery ({} samples)", alignedCount);
            }
            else
            {
                heading = std::format(ICON_FA_BOLT "  Battery ({} samples)", alignedCount);
            }
            ImGui::TextColored(theme.scheme().textPrimary, "%s", heading.c_str());

            // The heading's tooltip is shown after the chart: its value strip is placed beside the
            // heading, the item drawn just before it, so nothing else is submitted between the two.
            const bool headingHovered = ImGui::IsItemHovered();
            renderHistoryWithNowBars("PowerBatteryHistoryLayout", plotHeight, plot, bars, false, overviewNowBarColumns());
            // Tooltip with detailed info
            if (headingHovered)
            {
                ImGui::BeginTooltip();
                if (hasProcessPower)
                {
                    ImGui::TextUnformatted("Power: Aggregated CPU-proportional estimate from all processes.");
                }
                if (snap.power.hasBattery)
                {
                    ImGui::TextUnformatted("Battery: System battery charge percentage (0-100%).");
                    ImGui::Separator();
                    if (snap.power.healthPercent >= 0)
                    {
                        ImGui::Text("Health: %s", UI::Format::formatPercent(snap.power.healthPercent).c_str());
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
            fill.addPlot();
            ImGui::Spacing();
        }
    }

    // Threads, Page Faults, and Handles/FDs combined (aggregated from processes)
    if (processModel != nullptr)
    {
        const auto& procTimestamps = m_ProcessHistoryTimestamps;
        const auto& pageFaultHist = m_ProcessPageFaultsHistory;
        const auto& threadHist = m_ProcessThreadCountHistory;
        const auto& handleHist = m_ProcessHandleCountHistory;
        const size_t alignedCount = std::min({procTimestamps.size(), pageFaultHist.size(), threadHist.size(), handleHist.size()});

        // Always use default axis config even with no data
        const auto axis = alignedCount > 0 ? makeTimeAxisConfig(procTimestamps, m_MaxHistorySeconds, m_HistoryScrollSeconds)
                                           : makeTimeAxisConfig({}, m_MaxHistorySeconds, m_HistoryScrollSeconds);

        // Views into the panel's history, plotted as doubles -- no per-frame float copies (#1018).
        std::span<const double> timeData;
        const auto faultData = UI::Widgets::tailAlignedSpan(pageFaultHist, alignedCount).values;
        const auto threadData = UI::Widgets::tailAlignedSpan(threadHist, alignedCount).values;
        const auto handleData = UI::Widgets::tailAlignedSpan(handleHist, alignedCount).values;

        if (alignedCount > 0)
        {
            timeData = frameTimeAxis(procTimestamps, alignedCount, nowSeconds);

            // Update smoothed values
            const auto targetThreads = static_cast<double>(threadData.back());
            const auto targetFaults = static_cast<double>(faultData.back());
            const auto targetHandles = static_cast<double>(handleData.back());
            updateSmoothedResources(targetThreads, targetFaults, targetHandles, m_LastDeltaSeconds);
        }

        // Threads and handles are counts on the left axis; page faults are a rate, on their own
        // right-hand axis, so a fault spike no longer flattens the count lines (#1024). Each bar is
        // scaled to its series' axis, so a bar and its line show a value at the same height (#1003).
        // Both are sized to the samples in the window, not ones left of it (#1145), and to their bars'
        // smoothed values, which can still be easing down from a peak that has just left it.
        const double countAxisUpper = UI::Widgets::easedRateAxisUpperBound(
            "##ResourcesHistory",
            UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axis.xMin, threadData, handleData),
                                           {m_SmoothedResources.threads, m_SmoothedResources.handles}),
            UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);
        const double faultAxisUpper = UI::Widgets::easedRateAxisUpperBound(
            "##ResourcesHistory/Y2",
            UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axis.xMin, faultData), {m_SmoothedResources.pageFaults}),
            UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);

#ifdef _WIN32
        constexpr const char* handleLabel = "Handles";
#else
        constexpr const char* handleLabel = "FDs";
#endif

        const NowBar threadsBar{.valueText = UI::Format::formatIntLocalized(std::llround(m_SmoothedResources.threads)),
                                .label = THREADS_LABEL,
                                // The fallback tooltip, "Threads: <value>", says it all (#1019)
                                .tooltipText = {},
                                .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedResources.threads, countAxisUpper),
                                .color = theme.scheme().chartCpu};
        const NowBar faultsBar{.valueText = UI::Format::formatCountPerSecond(m_SmoothedResources.pageFaults),
                               .label = FAULTS_LABEL,
                               .tooltipText = {},
                               .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedResources.pageFaults, faultAxisUpper),
                               .color = theme.accentColor(3)};
        const NowBar handlesBar{.valueText = UI::Format::formatIntLocalized(std::llround(m_SmoothedResources.handles)),
                                .label = handleLabel,
                                .tooltipText = {}, // The fallback, "<label>: <value>", says it all (#1019)
                                .value01 = UI::Widgets::normalizeToUnitInterval(m_SmoothedResources.handles, countAxisUpper),
                                .color = theme.scheme().chartMemory};

        auto plot = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                            "##ResourcesHistory", axis.xMin, axis.xMax, formatAxisLocalized, countAxisUpper),
                                        plotHeight),
                m_ChartDataGeneration));
            if (chart.active())
            {
                UI::Widgets::setupSecondaryRateAxis(faultAxisUpper, formatAxisLocalized, theme.accentColor(3));
                // After all axis setup: the hint reads the plot's geometry, which locks setup (#1013).
                UI::Widgets::drawCollectingHint(alignedCount);
                const int count = UI::Format::checkedCount(alignedCount);
                plotSeries(THREADS_LABEL,
                           timeData.data(),
                           threadData.data(),
                           count,
                           theme.scheme().chartCpu,
                           theme.scheme().chartCpuFill,
                           seriesStyle(SeriesRole::Primary));
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                plotSeries(FAULTS_LABEL,
                           timeData.data(),
                           faultData.data(),
                           count,
                           theme.accentColor(3),
                           std::nullopt,
                           seriesStyle(SeriesRole::Secondary, 0));
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                plotSeries(handleLabel,
                           timeData.data(),
                           handleData.data(),
                           count,
                           theme.scheme().chartMemory,
                           theme.scheme().chartMemoryFill,
                           seriesStyle(SeriesRole::Secondary, 1));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        if (*idxVal < alignedCount)
                        {
                            const std::array rows{
                                UI::Widgets::TooltipRow{.label = THREADS_LABEL,
                                                        .color = theme.scheme().chartCpu,
                                                        .value = UI::Format::formatIntLocalized(std::llround(threadData[*idxVal]))},
                                UI::Widgets::TooltipRow{.label = FAULTS_LABEL,
                                                        .color = theme.accentColor(3),
                                                        .value = UI::Format::formatCountPerSecond(static_cast<double>(faultData[*idxVal]))},
                                UI::Widgets::TooltipRow{.label = handleLabel,
                                                        .color = theme.scheme().chartMemory,
                                                        .value = UI::Format::formatIntLocalized(std::llround(handleData[*idxVal]))},
                            };
                            UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                        }
                    }
                }
            }
        };

        ImGui::TextColored(
            theme.scheme().textPrimary, ICON_FA_GEARS "  Threads, Page Faults & %s (%zu samples)", handleLabel, alignedCount);
        renderHistoryWithNowBars(
            "ResourcesHistoryLayout", plotHeight, plot, {threadsBar, faultsBar, handlesBar}, false, overviewNowBarColumns());
        fill.addPlot();
        ImGui::Spacing();
    }
}

void SystemMetricsPanel::updateSmoothedCpu(const Domain::SystemSnapshot& snap, float deltaTimeSeconds)
{
    using UI::Format::clampPercent;

    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);

    const double targetTotal = clampPercent(snap.cpuTotal.totalPercent);
    const double targetUser = clampPercent(snap.cpuTotal.userPercent);
    const double targetSystem = clampPercent(snap.cpuTotal.systemPercent);
    const double targetIowait = clampPercent(snap.cpuTotal.iowaitPercent);
    const double targetIdle = clampPercent(snap.cpuTotal.idlePercent);

    // initializeOrSmooth, the one smoothing idiom for every NowBar: starts at the target, then eases
    // toward it (#1021).
    const bool initialized = m_SmoothedCpu.initialized;
    m_SmoothedCpu.total = clampPercent(initializeOrSmooth(m_SmoothedCpu.total, targetTotal, alpha, initialized));
    m_SmoothedCpu.user = clampPercent(initializeOrSmooth(m_SmoothedCpu.user, targetUser, alpha, initialized));
    m_SmoothedCpu.system = clampPercent(initializeOrSmooth(m_SmoothedCpu.system, targetSystem, alpha, initialized));
    m_SmoothedCpu.iowait = clampPercent(initializeOrSmooth(m_SmoothedCpu.iowait, targetIowait, alpha, initialized));
    m_SmoothedCpu.idle = clampPercent(initializeOrSmooth(m_SmoothedCpu.idle, targetIdle, alpha, initialized));
    m_SmoothedCpu.initialized = true;
}

void SystemMetricsPanel::updateSmoothedMemory(const Domain::SystemSnapshot& snap, float deltaTimeSeconds)
{
    MemorySection::updateSmoothedMemory(m_SmoothedMemory, snap, deltaTimeSeconds, m_RefreshInterval);
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
    const auto targetW = static_cast<double>(targetWatts);
    const auto targetB = static_cast<double>(targetBatteryPercent);

    const bool initialized = m_SmoothedPower.initialized;
    m_SmoothedPower.watts = initializeOrSmooth(m_SmoothedPower.watts, targetW, alpha, initialized);
    m_SmoothedPower.batteryChargePercent = initializeOrSmooth(m_SmoothedPower.batteryChargePercent, targetB, alpha, initialized);
    m_SmoothedPower.initialized = true;
}

void SystemMetricsPanel::updateSmoothedResources(double targetThreads, double targetFaults, double targetHandles, float deltaTimeSeconds)
{
    const double alpha = computeAlpha(deltaTimeSeconds, m_RefreshInterval);

    const bool initialized = m_SmoothedResources.initialized;
    m_SmoothedResources.threads = initializeOrSmooth(m_SmoothedResources.threads, targetThreads, alpha, initialized);
    m_SmoothedResources.pageFaults = initializeOrSmooth(m_SmoothedResources.pageFaults, targetFaults, alpha, initialized);
    m_SmoothedResources.handles = initializeOrSmooth(m_SmoothedResources.handles, targetHandles, alpha, initialized);
    m_SmoothedResources.initialized = true;
}

} // namespace App
