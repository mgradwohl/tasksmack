#include "SystemMetricsPanel.h"

#include "App/Panel.h"
#include "App/PanelPlatforms.h"
#include "App/Panels/BatteryDetailsText.h"
#include "App/Panels/CpuCoreGridIds.h"
#include "App/Panels/CpuCoresSection.h"
#include "App/Panels/CpuDetailsBlock.h"
#include "App/Panels/CpuDetailsText.h"
#include "App/Panels/GpuSection.h"
#include "App/Panels/MemorySection.h"
#include "App/Panels/NetworkSection.h"
#include "App/SelectOverride.h"
#include "App/ShellMetrics.h"
#include "App/SyntheticScenario.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/DeferredProbe.h"
#include "Domain/GPUModel.h"
#include "Domain/ISamplable.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/ThreadName.h"
#include "PowerStatusText.h"
#include "UI/Card.h"
#include "UI/ChartWidgets.h"
#include "UI/ChromeLayout.h"
#include "UI/ChromeWidgets.h"
#include "UI/EmptyState.h"
#include "UI/FillPlotLayout.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RateAxis.h"
#include "UI/TabContent.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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
        {.label = CPU_TOTAL_LABEL, .color = scheme.chartCpuTotal, .value = UI::Format::formatPercent(totalPercent)},
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

namespace
{

/// The one-factory test seam shared out to the three samplers (#2731): @p makeProbes runs once, on
/// whichever sampler thread reaches it first, and each sampler's factory then moves its own probes out
/// of the one result. One mutex covers the run and every take, so the run happens-before each take, and
/// a probe's first use on its taker's thread (the move itself, then GPUModel::setProbe()'s virtual
/// calls) comes after a lock that thread acquired. It was std::call_once, the same order in the
/// language, but libc++'s call_once completes inside libc++.so, which ThreadSanitizer doesn't
/// instrument, so TSan reported the moves and the first virtual calls on probes another thread built as
/// data races (the Sanitizers leg went red on main after #2877). A mutex is a synchronization it sees.
[[nodiscard]] SystemMetricsProbeFactories splitProbeFactory(std::function<SystemMetricsProbes()> makeProbes)
{
    struct Shared
    {
        std::mutex mutex;
        std::function<SystemMetricsProbes()> makeProbes; ///< guarded by mutex; empty once it has run
        SystemMetricsProbes probes;                      ///< guarded by mutex; each member emptied by its taker
    };
    const auto shared = std::make_shared<Shared>();
    shared->makeProbes = std::move(makeProbes);
    // Runs the factory if no taker has yet (one that throws stays, as under call_once, for the next taker
    // to retry; the sampler's start hook logs the failure), then hands @p take the result, still locked.
    const auto made = [shared](auto take)
    {
        const std::scoped_lock lock(shared->mutex);
        if (shared->makeProbes)
        {
            shared->probes = shared->makeProbes();
            shared->makeProbes = nullptr;
        }
        return take(shared->probes);
    };
    return {.system =
                [made]
            {
                return made([](SystemMetricsProbes& probes)
                            { return SystemModelProbes{.system = std::move(probes.system), .power = std::move(probes.power)}; });
            },
            .disk = [made] { return made([](SystemMetricsProbes& probes) { return std::move(probes.disk); }); },
            .gpu = [made] { return made([](SystemMetricsProbes& probes) { return std::move(probes.gpu); }); }};
}

} // namespace

SystemMetricsPanel::SystemMetricsPanel() : Panel("System")
{}

SystemMetricsPanel::SystemMetricsPanel(SystemMetricsProbeFactories factories) : Panel("System"), m_Factories(std::move(factories))
{
    // A factory the test left out is no such device (a null probe), never the platform's probe.
    if (!m_Factories.system)
    {
        m_Factories.system = []
        {
            return SystemModelProbes{};
        };
    }
    if (!m_Factories.disk)
    {
        m_Factories.disk = []
        {
            return std::unique_ptr<Platform::IDiskProbe>{};
        };
    }
    if (!m_Factories.gpu)
    {
        m_Factories.gpu = []
        {
            return std::unique_ptr<Platform::IGPUProbe>{};
        };
    }
}

SystemMetricsPanel::SystemMetricsPanel(std::function<SystemMetricsProbes()> makeProbes)
    : Panel("System"), m_Factories(splitProbeFactory(std::move(makeProbes)))
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
    // weak_ptr, so destruction order among these members isn't itself a safety requirement
    // today, but keeping the destructor in sync with onDetach() means that invariant doesn't
    // have to be re-verified by hand if a future member's safety ever does depend on order.
    m_GpuSampler.reset();
    m_StorageSampler.reset();
    m_SystemSampler.reset();
    m_Seeds.reset();
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
    m_ForceRefresh = true;

    // The synthetic scenario's probes when TASKSMACK_SYNTHETIC selects one (#1413), else the
    // platform's. A scenario that preloads history starts at its window, which ShellLayer's startup
    // event then confirms rather than trims.
    // Injected probe factories (tests, #1546; all three set, or none) replace both, with no scenario.
    const Synthetic::Scenario* scenario = m_Factories.system ? nullptr : Synthetic::activeScenario();
    m_MaxHistorySeconds = static_cast<double>(Synthetic::startupHistorySeconds(scenario, static_cast<int>(m_MaxHistorySeconds)));

    // The models exist from here, so the shell can wire them (ShellPanels::connectModels()), but
    // without probes (#2731): each sampler thread builds its own in its start hook below and seeds its
    // model there, so this thread never opens PDH, enumerates DXGI/NVML or waits for a first read, and
    // what the probes create belongs to the sampler's apartment, not this thread's STA. Until the seed
    // is adopted (onUpdate()) the publications are the empty generation and the charts draw no data.
    m_Model = std::make_shared<Domain::SystemModel>(Domain::DEFERRED_PROBE);
    m_Model->setMaxHistorySeconds(m_MaxHistorySeconds);
    // Config-file only (not in Settings), so applied once here, before the first refresh (#1291).
    m_Model->setMaxSaneNetworkRate(UserConfig::get().settings().maxSaneRateBps);

    m_StorageModel = std::make_shared<Domain::StorageModel>(Domain::DEFERRED_PROBE);
    m_StorageModel->setMaxHistorySeconds(m_MaxHistorySeconds);

    m_GPUModel = std::make_shared<Domain::GPUModel>(Domain::DEFERRED_PROBE);
    m_GPUModel->setMaxHistorySeconds(m_MaxHistorySeconds);

    m_Seeds = std::make_shared<Seeds>();

    // Three samplers, one per data domain (#2731): a disk rescan (PdhEnumObjects) or a DXGI/NVML
    // enumeration that stalls on a driver or PnP event holds up its own thread only, never the CPU and
    // memory samples, and each model publishes with its own timestamp, so the charts need no lockstep.
    // The start hooks run on their sampler's thread, before its first pass. Everything they touch is
    // captured by value or shared: the models outlive the samplers (onDetach() stops those first), and
    // the scenario is the process's. The factories are still the composition root's (PanelPlatforms) or
    // the test's. Each seed is the model's first publication, and the baseline the next pass's deltas
    // are taken against, one interval from now. The synthetic scenario fills the whole window before the
    // seed continues it; a no-op without one.
    m_SystemSampler = startSampler(Platform::SYSTEM_SAMPLER_THREAD_NAME,
                                   m_Model,
                                   "system",
                                   [makeProbes = m_Factories.system, scenario, model = m_Model, seeds = m_Seeds]
                                   {
                                       SystemModelProbes probes = makeProbes ? makeProbes() : makeSystemModelProbes(scenario);
                                       model->setProbes(std::move(probes.system), std::move(probes.power));
                                       if (scenario != nullptr)
                                       {
                                           Synthetic::preloadSystemHistory(scenario, *model, std::chrono::steady_clock::now());
                                       }
                                       model->refresh();
                                       seeds->system.store(true, std::memory_order_release);
                                       spdlog::info("SystemMetricsPanel: system and power probes built and seeded on {}",
                                                    Platform::SYSTEM_SAMPLER_THREAD_NAME);
                                   });
    m_StorageSampler =
        startSampler(Platform::STORAGE_SAMPLER_THREAD_NAME,
                     m_StorageModel,
                     "storage",
                     [makeProbe = m_Factories.disk, scenario, storage = m_StorageModel, seeds = m_Seeds]
                     {
                         storage->setProbe(makeProbe ? makeProbe() : makeStorageModelProbe(scenario));
                         if (scenario != nullptr)
                         {
                             Synthetic::preloadStorageHistory(scenario, *storage, std::chrono::steady_clock::now());
                         }
                         storage->sample();
                         seeds->storage.store(true, std::memory_order_release);
                         spdlog::info("SystemMetricsPanel: disk probe built and seeded on {}", Platform::STORAGE_SAMPLER_THREAD_NAME);
                     });
    m_GpuSampler = startSampler(Platform::GPU_SAMPLER_THREAD_NAME,
                                m_GPUModel,
                                "gpu",
                                [makeProbe = m_Factories.gpu, scenario, gpu = m_GPUModel, seeds = m_Seeds]
                                {
                                    gpu->setProbe(makeProbe ? makeProbe() : makeGPUModelProbe(scenario));
                                    gpu->refresh();
                                    seeds->gpu.store(true, std::memory_order_release);
                                    spdlog::info("SystemMetricsPanel: GPU probe built and seeded on {}", Platform::GPU_SAMPLER_THREAD_NAME);
                                });

    // The empty generation (version 0) until onUpdate() adopts the seeds; m_Hostname stays "System"
    // until then too, and the shell relabels the tab when it arrives.
    adoptPublications();
    m_ForceRefresh = false;
}

std::unique_ptr<Domain::BackgroundSampler> SystemMetricsPanel::startSampler(std::string_view threadName,
                                                                            const std::shared_ptr<Domain::ISamplable>& model,
                                                                            std::string name,
                                                                            std::function<void()> hook) const
{
    Domain::SamplerConfig samplerCfg;
    samplerCfg.interval = m_RefreshInterval;
    samplerCfg.firstSampleAfterInterval = true; // seeded by the start hook (#1102)
    samplerCfg.threadName = std::string(threadName);
    samplerCfg.multiThreadedApartment = true; // PDH's providers and DXGI live with the sampler, not the UI's STA (#2731)
    samplerCfg.onThreadStart = std::move(hook);
    auto sampler = std::make_unique<Domain::BackgroundSampler>(std::move(samplerCfg));
    sampler->addSamplable(model, std::move(name));
    sampler->start();
    return sampler;
}

bool SystemMetricsPanel::waitForFirstSample(std::chrono::milliseconds timeout)
{
    if (!m_Seeds || !m_Model)
    {
        return false; // not attached
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!m_Seeds->all())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    adoptPublications();
    return true;
}

void SystemMetricsPanel::adoptPublications()
{
    if (!m_SystemPublication || m_Model->publicationVersion() != m_SystemPublication->version)
    {
        adoptSystemPublication();
        // NOTE: m_Hostname intentionally stores the raw hostname without any icon prefix.
        // UI code (e.g., tab labels) is responsible for adding icons when rendering.
        if (const std::string& hostname = m_SystemPublication->snapshot.hostname; !hostname.empty())
        {
            m_Hostname = hostname;
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
}

void SystemMetricsPanel::adoptSystemPublication()
{
    // Held, not copied: the publication is immutable and shared with the model, so its snapshot and
    // timestamps are read in place (#1180) rather than copied into panel members on every adoption.
    m_SystemPublication = m_Model->publication();
    m_ChartDataGeneration = UI::Widgets::nextChartDataGeneration();
}

const Domain::SystemSnapshot& SystemMetricsPanel::systemSnapshot() const
{
    static const Domain::SystemSnapshot empty{};
    return m_SystemPublication ? m_SystemPublication->snapshot : empty;
}

void SystemMetricsPanel::onDetach()
{
    // Each waits for its start hook if that is still building its probes (#2731).
    m_GpuSampler.reset();
    m_StorageSampler.reset();
    m_SystemSampler.reset();
    m_Seeds.reset();
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
    // All three samplers (#2731): every domain is sampled at the Settings interval.
    for (Domain::BackgroundSampler* sampler : {m_SystemSampler.get(), m_StorageSampler.get(), m_GpuSampler.get()})
    {
        if (sampler != nullptr)
        {
            sampler->setInterval(interval);
        }
    }
    if (forceSample)
    {
        m_ForceRefresh = true;
        requestRefresh(); // Consume flag semantics for older calls
    }
}

std::chrono::milliseconds SystemMetricsPanel::samplingInterval() const
{
    return m_SystemSampler ? m_SystemSampler->interval() : m_RefreshInterval;
}

SystemMetricsPanel::SamplerIntervals SystemMetricsPanel::samplerIntervals() const
{
    return {.system = m_SystemSampler ? m_SystemSampler->interval() : m_RefreshInterval,
            .storage = m_StorageSampler ? m_StorageSampler->interval() : m_RefreshInterval,
            .gpu = m_GpuSampler ? m_GpuSampler->interval() : m_RefreshInterval};
}

void SystemMetricsPanel::requestRefresh()
{
    for (Domain::BackgroundSampler* sampler : {m_SystemSampler.get(), m_StorageSampler.get(), m_GpuSampler.get()})
    {
        if (sampler != nullptr)
        {
            sampler->requestRefresh();
        }
    }
    m_ForceRefresh = false; // Consumed since the samplers will refresh
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

    // The sampler's interval is not touched here: it is the refresh setting, set by setSamplingInterval()
    // alone, whether or not this tab is the one shown and whatever the window is doing (#2731). Sampling
    // and drawing are separate.
    if (m_ForceRefresh)
    {
        requestRefresh();
    }

    adoptPublications();
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

void SystemMetricsPanel::renderContent()
{
    if (!m_Model)
    {
        UI::Widgets::renderEmptyState(ICON_FA_TRIANGLE_EXCLAMATION "  System data unavailable",
                                      "The system model is not initialized, so there are no system metrics to show.");
        return;
    }

    // Skip rendering when tab is inactive (data collection continues in onUpdate)
    if (!m_IsActiveTab)
    {
        return;
    }

    const auto& theme = UI::Theme::get();

    // A reference, not a copy: m_SystemPublication is only reassigned in onUpdate(), never while
    // rendering, and a copy duplicated every per-core and per-interface vector each frame (#1017).
    const auto& snap = systemSnapshot();

    // Add padding inside tabs for better spacing, scaled like the style it overrides (#971)
    const float tabPaddingScale = theme.styleScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ShellMetrics::TAB_PADDING_X * tabPaddingScale, ShellMetrics::SUB_TAB_PADDING_Y * tabPaddingScale));

    if (ImGui::BeginTabBar("SystemTabs", ImGuiTabBarFlags_DrawSelectedOverline))
    {
        // A sub-tab asked for (requestTab(): TASKSMACK_MACHINE_TAB, TASKSMACK_DEMO's machine=, #2869) is
        // submitted with ImGuiTabItemFlags_SetSelected on every frame until BeginTabItem() reports it
        // selected, as the shell's tab bar does for TASKSMACK_TAB (SelectOverride::PendingMainTab).
        using SelectOverride::MachineTab;
        const auto tabFlags = [this](const MachineTab tab) -> ImGuiTabItemFlags
        {
            return m_RequestedTab.wantsSelected(tab) ? ImGuiTabItemFlags_SetSelected : 0;
        };
        const auto beginTab = [this, &tabFlags](const char* label, const MachineTab tab)
        {
            const bool selected = ImGui::BeginTabItem(label, nullptr, tabFlags(tab));
            m_RequestedTab.onTabSubmitted(tab, selected);
            return selected;
        };

        // Each tab's body scrolls in its own child, so the tab bar itself stays in view (#968).
        if (beginTab(ICON_FA_GAUGE_HIGH "  Overview", MachineTab::Overview))
        {
            {
                const UI::Widgets::TabContentScope content("##OverviewContent");
                renderOverview();
            }
            ImGui::EndTabItem();
        }

        if (CpuCoresSection::showCpuCoresTab(snap.seenCoreIds, static_cast<std::size_t>(snap.coreCount)))
        {
            if (beginTab(ICON_FA_MICROCHIP "  CPU Cores", MachineTab::Cores))
            {
                // Build context for CpuCoresSection render function
                CpuCoresSection::RenderContext cpuCtx{
                    .publication = m_SystemPublication.get(),
                    .chartDataGeneration = m_ChartDataGeneration,
                    .maxHistorySeconds = m_MaxHistorySeconds,
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
            if (beginTab(ICON_FA_MICROCHIP "  GPU", MachineTab::Gpu))
            {
                // Build context for GpuSection render function
                GpuSection::RenderContext gpuCtx{
                    .publication = m_GPUPublication.get(),
                    .chartDataGeneration = m_ChartDataGeneration,
                    .maxHistorySeconds = m_MaxHistorySeconds,
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

        // NPU tab, beside the GPU tab: only once an NPU or other compute accelerator is listed (#1799).
        // The GPU tab's code draws it, over the publication's accelerators.
        if (m_GPUModel && GpuSection::showAcceleratorTab(m_GPUPublication.get()))
        {
            if (beginTab(ICON_FA_BRAIN "  NPU", MachineTab::Npu))
            {
                GpuSection::RenderContext npuCtx{
                    .publication = m_GPUPublication.get(),
                    .chartDataGeneration = m_ChartDataGeneration,
                    .maxHistorySeconds = m_MaxHistorySeconds,
                    .lastDeltaSeconds = m_LastDeltaSeconds,
                    .refreshInterval = m_RefreshInterval,
                    .smoothedGPUs = &m_SmoothedAccelerators,
                    .cache = &m_NpuFrameCache,
                    .devices = GpuSection::DeviceSet::Accelerators,
                };
                {
                    const UI::Widgets::TabContentScope content("##NpuContent");
                    UI::Widgets::FillPlotLayout fill(m_NpuFill);
                    npuCtx.fill = &fill;
                    GpuSection::renderGpuSection(npuCtx);
                }
                ImGui::EndTabItem();
            }
        }

        // Network and I/O tab - show if network counters are available
        if (m_Model != nullptr && m_Model->capabilities().hasNetworkCounters)
        {
            if (beginTab(ICON_FA_NETWORK_WIRED "  Network and I/O", MachineTab::Network))
            {
                // Build context for NetworkSection render functions
                NetworkSection::RenderContext netCtx{
                    .systemPublication = m_SystemPublication.get(),
                    .storagePublication = m_StoragePublication.get(),
                    .chartDataGeneration = m_ChartDataGeneration,
                    .hasNetworkCounters = m_Model != nullptr && m_Model->capabilities().hasNetworkCounters,
                    .maxHistorySeconds = m_MaxHistorySeconds,
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

        // The request's frames are counted only once the first sample is in, when every sub-tab this
        // machine draws is drawn; a request still waiting then names a sub-tab it never has.
        if (m_SystemPublication)
        {
            if (const std::optional<MachineTab> dropped = m_RequestedTab.onFrameEnd(); dropped.has_value())
            {
                spdlog::warn("Requested machine sub-tab '{}' was not drawn after {} frames; ignored",
                             SelectOverride::machineTabName(*dropped),
                             SelectOverride::MAX_MACHINE_TAB_FRAMES);
            }
        }
    }

    ImGui::PopStyleVar(); // FramePadding
}

void SystemMetricsPanel::renderOverview()
{
    const auto& snap = systemSnapshot(); // See renderContent() (#1017)
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

    // The CPU Details block (#809), in place of the header line that listed some of these facts. Its
    // rows come from the publications, the process count and the process histories, so they are
    // rebuilt only when one of those changes (#1171).
    const std::size_t processCount = (processModel != nullptr) ? processModel->processCount() : 0;
    const std::uint64_t systemVersion = m_SystemPublication ? m_SystemPublication->version : 0;
    const std::uint64_t gpuVersion = m_GPUPublication ? m_GPUPublication->version : 0;
    if (!m_CpuDetails.valid || m_CpuDetails.systemVersion != systemVersion || m_CpuDetails.gpuVersion != gpuVersion ||
        m_CpuDetails.processHistoryVersion != m_ProcessHistoryVersion || m_CpuDetails.processCount != processCount ||
        m_CpuDetails.hasProcessModel != (processModel != nullptr))
    {
        // Built in a fresh cache and moved in whole, validity last: a render exception is caught and
        // the app carries on, so a rebuild that throws part-way must leave the cache stale.
        CpuDetailsCache fresh;
        const Platform::SystemCapabilities caps = (m_Model != nullptr) ? m_Model->capabilities() : Platform::SystemCapabilities{};
        const auto lastOf = [](std::span<const double> history)
        {
            return history.empty() ? std::optional<double>{} : std::optional<double>{history.back()};
        };
        const bool hasProcesses = (processModel != nullptr);
        const CpuDetailsText::Inputs inputs{
            .snapshot = &snap,
            .hasCpuFreq = caps.hasCpuFreq,
            .hasUptime = caps.hasUptime,
            .hasVirtualizationInfo = caps.hasVirtualizationInfo,
            .hasVirtualizationSecurityInfo = caps.hasVirtualizationSecurityInfo,
            .processCount = hasProcesses ? std::optional<std::size_t>{processCount} : std::nullopt,
            .threadCount = (hasProcesses && m_ProcessCapabilities.hasThreadCount) ? lastOf(m_ProcessThreadCountHistory) : std::nullopt,
            .handleCount = (hasProcesses && m_ProcessCapabilities.hasHandleCount) ? lastOf(m_ProcessHandleCountHistory) : std::nullopt,
#ifdef _WIN32
            .handlesAreFileDescriptors = false,
#else
            .handlesAreFileDescriptors = true,
#endif
            // Total dedicated VRAM: discrete GPUs only, an integrated GPU's "memory" being system RAM (#1114).
            .totalVramBytes = m_GPUPublication ? GpuSection::totalDedicatedVramBytes(m_GPUPublication->snapshots) : 0,
        };
        fresh.rows = CpuDetailsText::buildRows(inputs);
        fresh.collapsedSummary = CpuDetailsText::collapsedSummary(inputs);
        fresh.systemVersion = systemVersion;
        fresh.gpuVersion = gpuVersion;
        fresh.processHistoryVersion = m_ProcessHistoryVersion;
        fresh.processCount = processCount;
        fresh.hasProcessModel = hasProcesses;
        fresh.generation = m_CpuDetails.generation + 1;
        fresh.valid = true;
        m_CpuDetails = std::move(fresh);
    }
    // Expanded or collapsed, as the user left it (UserConfig, #809). The block renders inside the fill
    // layout's scope, so the charts share whatever height it leaves, at either size.
    // Each Overview section is a card, as the CPU Cores grid draws its cells (#1586). The chart cards
    // are sized to their chart explicitly (UI::Widgets::beginChartCard()); this one, with no chart, to
    // the rows it lays out this frame (CpuDetailsBlock::beginCard(), #1919).
    auto& userSettings = UserConfig::get().settings();
    const CpuDetailsBlock::Content cpuDetailsContent{.cpuModel = snap.cpuModel,
                                                     .collapsedSummary = m_CpuDetails.collapsedSummary,
                                                     .rows = m_CpuDetails.rows,
                                                     .rowsGeneration = m_CpuDetails.generation,
                                                     .expanded = userSettings.cpuDetailsExpanded};
    const bool cpuDetailsVisible = CpuDetailsBlock::beginCard("##CpuDetailsCard", cpuDetailsContent, m_CpuDetailsMeasured);
    if (cpuDetailsVisible && CpuDetailsBlock::render(cpuDetailsContent, m_CpuDetailsMeasured))
    {
        userSettings.cpuDetailsExpanded = !userSettings.cpuDetailsExpanded;
        UserConfig::get().save();
    }
    UI::Widgets::endCard();

    // Get theme for colored progress bars
    auto& theme = UI::Theme::get();

    const auto& cpuHist = m_SystemPublication->cpuHistory;
    const auto& cpuUserHist = m_SystemPublication->cpuUserHistory;
    const auto& cpuSystemHist = m_SystemPublication->cpuSystemHistory;
    const auto& cpuIowaitHist = m_SystemPublication->cpuIowaitHistory;
    const auto& cpuIdleHist = m_SystemPublication->cpuIdleHistory;
    const std::span<const double> timestamps = m_SystemPublication->timestamps;
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)
    const auto axisConfig = makeTimeAxisConfig(m_MaxHistorySeconds);

    const size_t cpuCount = std::min(cpuHist.size(), timestamps.size());
    const auto cpuData = UI::Widgets::tailAlignedSpan(cpuHist, cpuCount).values;
    // CPU history with vertical now bars (total + breakdown)
    const bool cpuCardVisible = UI::Widgets::beginChartCard("##CpuUsageCard", plotHeight);
    (void) UI::Widgets::sectionHeader(ICON_FA_MICROCHIP, "CPU Usage", {}, cpuCount);

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
                // The band edges below are cumulative tops: "Copy data as CSV" takes each series'
                // own values instead (#808).
                UI::Widgets::exportSeries(CPU_USER_LABEL, breakdownTimeData, cpuUserData);
                UI::Widgets::exportSeries(CPU_SYSTEM_LABEL, breakdownTimeData, cpuSystemData);
                if (showIowait)
                {
                    UI::Widgets::exportSeries(CPU_IOWAIT_LABEL, breakdownTimeData, cpuIowaitData);
                }

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

                // The User and System bands' edges (shared with Process Details, #1180). A gap point is
                // NaN in every band (see UI::Widgets::reduceAlignedSeries).
                auto& stack = m_CpuStack;
                UI::Widgets::buildUserSystemStack<float>(points, breakdownTimeData, cpuUserData, cpuSystemData, stack);
                auto& yIowaitTop = m_CpuStackYIowait;
                auto& yBusyTop = m_CpuStackYBusy;
                yIowaitTop.resize(points.size());
                yBusyTop.resize(points.size());
                for (std::size_t k = 0; k < points.size(); ++k)
                {
                    if (points[k].gap)
                    {
                        yIowaitTop[k] = yBusyTop[k] = std::numeric_limits<double>::quiet_NaN();
                        continue;
                    }
                    // I/O Wait is idle time, not busy (#1157): its band sits on the busy total
                    // (100 - idle - iowait, which the Total line follows) rather than on System, so
                    // the Total line runs along its bottom edge instead of through it.
                    const auto i = static_cast<std::size_t>(points[k].index);
                    yIowaitTop[k] = 100.0 - static_cast<double>(cpuIdleData[i]);
                    yBusyTop[k] = yIowaitTop[k] - static_cast<double>(cpuIowaitData[i]);
                }

                // The bands reach "now" like every plotLineWithFill series: the last sample held to
                // x = 0 (#1016), unless it is too old to pass for current (#1147).
                UI::Widgets::holdLastValuesToNow(stack.x,
                                                 {&stack.base, &stack.userTop, &stack.systemTop, &yIowaitTop, &yBusyTop},
                                                 UI::Widgets::maxHoldSecondsForAxis(breakdownTimeData));
                const int stackCount = UI::Format::checkedCount(stack.x.size());

                // Each band is filled only where both of its edges have a reading (#1149).
                const auto shadeBand =
                    [&](const char* label, const std::vector<double>& lower, const std::vector<double>& upper, const ImVec4& fillColor)
                {
                    UI::Widgets::plotShadedBand(label, stack.x.data(), lower.data(), upper.data(), stackCount, fillColor);
                };
                shadeBand(CPU_USER_LABEL, stack.base, stack.userTop, theme.scheme().cpuUserFill);
                shadeBand(CPU_SYSTEM_LABEL, stack.userTop, stack.systemTop, theme.scheme().cpuSystemFill);
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
                    UI::Widgets::plotStyledLine(
                        label, stack.x.data(), top.data(), stackCount, color, seriesStyle(SeriesRole::Secondary, slot));
                };
                bandEdge(CPU_USER_LABEL, stack.userTop, theme.scheme().cpuUser, 0);
                bandEdge(CPU_SYSTEM_LABEL, stack.systemTop, theme.scheme().cpuSystem, 1);
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
                                     theme.scheme().chartCpuTotal,
                                     std::nullopt,
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
                       .color = theme.scheme().chartCpuTotal}); // The Total line's colour (#1192)
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

    if (cpuCardVisible)
    {
        renderHistoryWithNowBars("OverviewCPUHistoryLayout", plotHeight, cpuPlot, cpuBars, {.minBarColumns = overviewNowBarColumns()});
    }
    UI::Widgets::endChartCard("##CpuUsageCard", plotHeight);
    fill.addPlot(); // After the card, in the panel's coordinates: the fill layout counts its padding as non-chart height

    // Memory & Swap history section
    {
        MemorySection::RenderContext memCtx{
            .publication = m_SystemPublication.get(),
            .chartDataGeneration = m_ChartDataGeneration,
            .maxHistorySeconds = m_MaxHistorySeconds,
            .lastDeltaSeconds = m_LastDeltaSeconds,
            .refreshInterval = m_RefreshInterval,
            .smoothedMemory = &m_SmoothedMemory,
            .plotHeight = plotHeight,
        };
        if (UI::Widgets::beginChartCard("##MemoryCard", plotHeight))
        {
            MemorySection::renderMemorySection(memCtx, timestamps, nowSeconds, static_cast<int>(overviewNowBarColumns()));
        }
        UI::Widgets::endChartCard("##MemoryCard", plotHeight);
        fill.addPlot();
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

            // A view into the battery history, too: the model already stores "no reading" as NaN, a
            // gap in the line rather than a dive to 0 %, so it is plotted as is (#2159).
            const auto batteryHist = UI::Widgets::tailAlignedSpan(batteryHistFloat, batteryCount).values;

            const auto powerTimeData = frameTimeAxis(m_ProcessHistoryTimestamps, powerCount, nowSeconds);
            const auto batteryTimeData = frameTimeAxis(timestamps, batteryCount, nowSeconds);
            const auto axis = makeTimeAxisConfig(m_MaxHistorySeconds);
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
                    .color = theme.scheme().chartPower,
                });
            }

            if (snap.power.hasBattery)
            {
                // The battery's status -- on AC, charging, time left -- is its strip entry's text
                // ("Battery: <plug> <battery> 94% (not charging)"), on the heading's line like every
                // chart's value strip; it was a separate right-aligned status there.
                // A dash and a hatched bar, not a stale or 0% figure, while the battery reports no
                // charge (#2408, #2529).
                bars.push_back(
                    Detail::batteryNowBar(snap.power, m_SmoothedPower.batteryChargePercent, batteryLabel, theme.scheme().chartBattery));
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
                                                            theme.scheme().chartBattery,
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
                                   theme.scheme().chartPower,
                                   theme.scheme().chartPowerFill,
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
                                   theme.scheme().chartBattery,
                                   theme.scheme().chartBatteryFill,
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
                                                .color = theme.scheme().chartPower,
                                                .value = UI::Format::formatPowerOrZero(Domain::Numeric::toDouble(powerHist[*powerIdx]))});
                            }
                            if (batteryIdx)
                            {
                                const double batteryVal = Domain::Numeric::toDouble(batteryHist[*batteryIdx]);
                                rows.push_back({.label = batteryLabel,
                                                .color = theme.scheme().chartBattery,
                                                .value = UI::Widgets::formatSampleOrNA(
                                                    batteryVal, [](double v) { return UI::Format::formatPercent(v); })});
                            }
                            UI::Widgets::renderHistoryTooltip(powerIdx ? powerTimeData[*powerIdx] : batteryTimeData[*batteryIdx], rows);
                        }
                    }
                }
            };

            // Chart heading; the battery's status is in its value-strip entry, and the sample count in
            // the heading's own tooltip below (#1200).
            const char* headingTitle = "Battery";
            if (!snap.power.hasBattery)
            {
                headingTitle = "Power";
            }
            else if (hasProcessPower)
            {
                headingTitle = "Power & Battery";
            }

            // The heading's tooltip is shown after the chart: its value strip is placed beside the
            // heading, the item drawn just before it, so nothing else is submitted between the two.
            const bool powerCardVisible = UI::Widgets::beginChartCard("##PowerCard", plotHeight);
            const bool headingHovered = powerCardVisible && UI::Widgets::sectionHeader(ICON_FA_BOLT, headingTitle);
            if (powerCardVisible)
            {
                renderHistoryWithNowBars("PowerBatteryHistoryLayout", plotHeight, plot, bars, {.minBarColumns = overviewNowBarColumns()});
            }
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
                }
                std::array<char, 64> samplesText{};
                const std::string_view samples = UI::ChromeLayout::formatSampleCount(samplesText, alignedCount);
                ImGui::TextUnformatted(samples.data(), samples.data() + samples.size());
                ImGui::EndTooltip();
            }
            if (powerCardVisible && snap.power.hasBattery)
            {
                // Battery details beneath the chart, in its card (#1523): inside the fill scope, so the
                // charts' shared height accounts for them.
                const std::uint64_t version = m_SystemPublication->version;
                if (m_BatteryDetails.generation == 0 || m_BatteryDetails.systemVersion != version)
                {
                    std::vector<CpuDetailsText::Row> rows = BatteryDetailsText::buildRows(snap.power);
                    if (m_BatteryDetails.generation == 0 || rows != m_BatteryDetails.rows)
                    {
                        m_BatteryDetails.rows = std::move(rows);
                        ++m_BatteryDetails.generation;
                    }
                    m_BatteryDetails.systemVersion = version;
                }
                CpuDetailsBlock::renderRows(
                    "##BatteryDetails", m_BatteryDetails.rows, m_BatteryDetails.generation, m_BatteryDetailsMeasured);
            }
            UI::Widgets::endChartCard("##PowerCard", plotHeight);
            fill.addPlot();
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
        const auto axis = makeTimeAxisConfig(m_MaxHistorySeconds);

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

        // Three scales: threads on the left axis; page faults, a rate, on the right-hand Y2, so a fault
        // spike doesn't flatten the count lines (#1024); and handles/FDs on a right-hand Y3 outside it.
        // A machine holds tens to hundreds of times more handles than threads, and while the two
        // shared the left axis the Threads line and bar lay flat at zero (#1916). Each bar is scaled
        // to its series' axis, so a bar and its line show a value at the same height (#1003). All are
        // sized to the samples in the window, not ones left of it (#1145), and to their bars' smoothed
        // values, which can still be easing down from a peak that has just left it.
        const double threadAxisUpper = UI::Widgets::easedRateAxisUpperBound(
            "##ResourcesHistory",
            UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axis.xMin, threadData), {m_SmoothedResources.threads}),
            UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);
        const double faultAxisUpper = UI::Widgets::easedRateAxisUpperBound(
            "##ResourcesHistory/Y2",
            UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axis.xMin, faultData), {m_SmoothedResources.pageFaults}),
            UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);
        const double handleAxisUpper = UI::Widgets::easedRateAxisUpperBound(
            "##ResourcesHistory/Y3",
            UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, axis.xMin, handleData), {m_SmoothedResources.handles}),
            UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);

#ifdef _WIN32
        constexpr const char* handleLabel = "Handles →"; // The outer right-hand axis (#1916)
        constexpr const char* resourcesTitle = "Threads, Page Faults & Handles";
#else
        constexpr const char* handleLabel = "FDs →";
        constexpr const char* resourcesTitle = "Threads, Page Faults & FDs";
#endif

        // A total the process probe can't count at all is N/A, drawn hatched rather than as a measured 0
        // (#1935). Its capabilities arrive with its first samples; until then the bars are empty as before.
        const bool capabilitiesKnown = alignedCount > 0;
        const bool threadsAvailable = !capabilitiesKnown || m_ProcessCapabilities.hasThreadCount;
        const bool faultsAvailable = !capabilitiesKnown || m_ProcessCapabilities.hasPageFaults;
        const bool handlesAvailable = !capabilitiesKnown || m_ProcessCapabilities.hasHandleCount;
        const NowBar threadsBar{
            .valueText = threadsAvailable ? UI::Format::formatIntLocalized(std::llround(m_SmoothedResources.threads)) : std::string("N/A"),
            .label = THREADS_LABEL,
            // The fallback tooltip, "Threads: <value>", says it all (#1019)
            .tooltipText = {},
            .value01 = threadsAvailable ? UI::Widgets::normalizeToUnitInterval(m_SmoothedResources.threads, threadAxisUpper) : 0.0,
            .color = theme.scheme().chartThreads,
            .marker = ImPlotMarker_None,
            .available = threadsAvailable};
        const NowBar faultsBar{
            .valueText = faultsAvailable ? UI::Format::formatCountPerSecond(m_SmoothedResources.pageFaults) : std::string("N/A"),
            .label = FAULTS_LABEL,
            .tooltipText = {},
            .value01 = faultsAvailable ? UI::Widgets::normalizeToUnitInterval(m_SmoothedResources.pageFaults, faultAxisUpper) : 0.0,
            .color = theme.scheme().chartPageFaults,
            .marker = ImPlotMarker_None,
            .available = faultsAvailable};
        const NowBar handlesBar{
            .valueText = handlesAvailable ? UI::Format::formatIntLocalized(std::llround(m_SmoothedResources.handles)) : std::string("N/A"),
            .label = handleLabel,
            .tooltipText = {}, // The fallback, "<label>: <value>", says it all (#1019)
            .value01 = handlesAvailable ? UI::Widgets::normalizeToUnitInterval(m_SmoothedResources.handles, handleAxisUpper) : 0.0,
            .color = theme.scheme().chartHandles,
            .marker = ImPlotMarker_None,
            .available = handlesAvailable};

        auto plot = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                            "##ResourcesHistory", axis.xMin, axis.xMax, formatAxisLocalized, threadAxisUpper),
                                        plotHeight),
                m_ChartDataGeneration));
            if (chart.active())
            {
                UI::Widgets::setupSecondaryRateAxis(faultAxisUpper, formatAxisLocalized, theme.scheme().chartPageFaults);
                UI::Widgets::setupRightRateAxis(ImAxis_Y3, handleAxisUpper, formatAxisLocalized, theme.scheme().chartHandles);
                // After all axis setup: the hint reads the plot's geometry, which locks setup (#1013).
                UI::Widgets::drawCollectingHint(alignedCount);
                const int count = UI::Format::checkedCount(alignedCount);
                // Each line fills its own axis, so a steady thread count and a steady handle count sit at
                // the same height: Threads, the chart's primary series, is drawn last, on top (#1916).
                // A CSV copy keeps the key's column order: a series' first export is the one kept.
                UI::Widgets::exportSeries(THREADS_LABEL, timeData, threadData);
                UI::Widgets::exportSeries(FAULTS_LABEL, timeData, faultData);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y3);
                plotSeries(handleLabel,
                           timeData.data(),
                           handleData.data(),
                           count,
                           theme.scheme().chartHandles,
                           theme.scheme().chartHandlesFill,
                           seriesStyle(SeriesRole::Secondary, 1));
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                plotSeries(FAULTS_LABEL,
                           timeData.data(),
                           faultData.data(),
                           count,
                           theme.scheme().chartPageFaults,
                           std::nullopt,
                           seriesStyle(SeriesRole::Secondary, 0));
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                plotSeries(THREADS_LABEL,
                           timeData.data(),
                           threadData.data(),
                           count,
                           theme.scheme().chartThreads,
                           theme.scheme().chartThreadsFill,
                           seriesStyle(SeriesRole::Primary));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        if (*idxVal < alignedCount)
                        {
                            const std::array rows{
                                UI::Widgets::TooltipRow{.label = THREADS_LABEL,
                                                        .color = theme.scheme().chartThreads,
                                                        .value = UI::Format::formatIntLocalized(std::llround(threadData[*idxVal]))},
                                UI::Widgets::TooltipRow{.label = FAULTS_LABEL,
                                                        .color = theme.scheme().chartPageFaults,
                                                        .value = UI::Format::formatCountPerSecond(static_cast<double>(faultData[*idxVal]))},
                                UI::Widgets::TooltipRow{.label = handleLabel,
                                                        .color = theme.scheme().chartHandles,
                                                        .value = UI::Format::formatIntLocalized(std::llround(handleData[*idxVal]))},
                            };
                            UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                        }
                    }
                }
            }
        };

        if (UI::Widgets::beginChartCard("##ResourcesCard", plotHeight))
        {
            (void) UI::Widgets::sectionHeader(ICON_FA_GEARS, resourcesTitle, {}, alignedCount);
            renderHistoryWithNowBars("ResourcesHistoryLayout",
                                     plotHeight,
                                     plot,
                                     {threadsBar, faultsBar, handlesBar},
                                     {.minBarColumns = overviewNowBarColumns()});
        }
        UI::Widgets::endChartCard("##ResourcesCard", plotHeight);
        fill.addPlot();
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
