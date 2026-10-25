#pragma once

#include "App/Panel.h"
#include "App/Panels/CpuDetailsBlock.h"
#include "App/Panels/CpuDetailsText.h"
#include "App/Panels/GpuSection.h"
#include "App/Panels/MemorySection.h"
#include "App/Panels/NetInterfaceUtils.h"
#include "App/Panels/NetworkSection.h"
#include "App/Panels/StorageSection.h"
#include "App/SelectOverride.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/GPUModel.h"
#include "Domain/ISamplable.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SharedHistory.h"
#include "Domain/StorageModel.h"
#include "Domain/StorageSnapshot.h"
#include "Domain/SystemModel.h"
#include "Domain/SystemSnapshot.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPowerProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/ProcessTypes.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"
#include "UI/Theme.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App
{

/// The System model's probes (#2731): the system counters and the power state, read together on the
/// system sampler. A null power probe is no battery, as the model already allows.
struct SystemModelProbes
{
    std::unique_ptr<Platform::ISystemProbe> system;
    std::unique_ptr<Platform::IPowerProbe> power;
};

/// The probe factories SystemMetricsPanel's three samplers build their models from (#1546, #2731), one per
/// data domain: each runs once, on its own sampler thread, from that sampler's start hook. A null disk or
/// GPU probe is no disk or GPU, as the models already allow, and so is a factory a test leaves empty. The
/// panel's default constructor uses the composition root's (App/PanelPlatforms) instead.
struct SystemMetricsProbeFactories
{
    std::function<SystemModelProbes()> system;
    std::function<std::unique_ptr<Platform::IDiskProbe>()> disk;
    std::function<std::unique_ptr<Platform::IGPUProbe>()> gpu;
};

/// All four probes at once: the one-factory test seam (#1546). The panel shares its result out to the
/// three samplers (whichever sampler thread reaches it first runs it, once).
struct SystemMetricsProbes
{
    std::unique_ptr<Platform::ISystemProbe> system;
    std::unique_ptr<Platform::IPowerProbe> power;
    std::unique_ptr<Platform::IDiskProbe> disk;
    std::unique_ptr<Platform::IGPUProbe> gpu;
};

/// Panel displaying system-wide metrics with ImPlot graphs.
/// Shows CPU, memory, swap, disk I/O, and GPU usage over time.
class SystemMetricsPanel : public Panel
{
  public:
    /// The models' probes come from the synthetic scenario when TASKSMACK_SYNTHETIC selects one, else
    /// the platform's (Synthetic::make*Probe()).
    SystemMetricsPanel();

    /// Test seam (#1546): the samplers take their probes from @p factories instead (tests: mocks), one
    /// factory per sampler thread, with no synthetic scenario. The App layer stays the only place the
    /// platform's probes are made.
    explicit SystemMetricsPanel(SystemMetricsProbeFactories factories);

    /// The one-factory test seam (#1546): @p makeProbes runs once, on whichever sampler thread reaches it
    /// first, and each sampler takes its own probes from the result, run and takes under one mutex (#2731).
    explicit SystemMetricsPanel(std::function<SystemMetricsProbes()> makeProbes);
    ~SystemMetricsPanel() override;

    SystemMetricsPanel(const SystemMetricsPanel&) = delete;
    SystemMetricsPanel& operator=(const SystemMetricsPanel&) = delete;
    SystemMetricsPanel(SystemMetricsPanel&&) = delete;
    SystemMetricsPanel& operator=(SystemMetricsPanel&&) = delete;

    /// Initialize the panel: makes the System, Storage and GPU models and starts a sampler for each
    /// (#2731): three threads, so a stalled disk rescan or GPU enumeration never delays the CPU and memory
    /// samples. Each sampler's thread builds its model's probe(s) and seeds it. Returns without a probe
    /// call or a read: the charts draw their empty state until onUpdate() adopts the seeds.
    void onAttach() override;

    /// Waits until every sampler thread has built its probes and seeded its model, then adopts their
    /// publications as onUpdate() would (#2731). For tests, which used to find the seed in place when
    /// onAttach() returned; the app never waits. False on timeout, or before onAttach().
    [[nodiscard]] bool waitForFirstSample(std::chrono::milliseconds timeout = std::chrono::seconds(10));

    /// Cleanup.
    void onDetach() override;

    /// Adopts the samplers' newest publications and runs the per-frame bookkeeping. It never sets the
    /// sampling cadence: that is the Settings value alone (#2731).
    void onUpdate(float deltaTime) override;

    /// Set the refresh interval all three samplers run at. It is the Settings value, whatever the UI
    /// state (#2731): no tab change, resize, move or minimize rescales or pauses it.
    void setSamplingInterval(std::chrono::milliseconds interval, bool forceSample = true);

    /// The interval the system sampler runs at: the refresh setting from Settings, in every UI state
    /// (#2731); before onAttach(), the setting itself. samplerIntervals() has all three.
    [[nodiscard]] std::chrono::milliseconds samplingInterval() const;

    /// The interval each of the three samplers runs at (#2731). All three are the refresh setting, in
    /// every UI state; before onAttach(), the setting itself. For the tests that check that.
    struct SamplerIntervals
    {
        std::chrono::milliseconds system;
        std::chrono::milliseconds storage;
        std::chrono::milliseconds gpu;
    };
    [[nodiscard]] SamplerIntervals samplerIntervals() const;

    /// Request an immediate refresh.
    void requestRefresh();

    /// Inject process model for aggregated system histories (read-only: ProcessesPanel owns it and
    /// sets its history length, #1078). Held as a weak_ptr, so once ProcessesPanel releases the
    /// model (its onDetach) this panel sees no model rather than a dangling one (#1176). Cleared on
    /// this panel's own detach.
    void setProcessModel(std::weak_ptr<Domain::ProcessModel> model)
    {
        m_ProcessModel = std::move(model);
    }

    /// Handle application events (history/refresh changes)
    void onEvent(Core::Event& event) override;

    /// Render content only (for embedding in tab, without window wrapper).
    void renderContent() override;

    /// Width of the Overview's NowBar column, including the cell padding that separates it from the
    /// plot, at the current font and style. For the window's content minimum (#1207); needs a frame.
    [[nodiscard]] float overviewNowBarColumnWidth() const;

    /// The most any of this panel's chart tabs spent around and above its first chart, plot aside,
    /// counting only measurements taken at this window width and font size
    /// (UI::Widgets::currentFirstPlotNonPlotHeight()): the minimum window height keeps room for it
    /// (#1370 review). 0 when no tab has a current measurement.
    [[nodiscard]] float firstChartNonPlotHeight(float windowWidth, float fontSize) const noexcept
    {
        return std::max({UI::Widgets::currentFirstPlotNonPlotHeight(m_OverviewFill.firstPlot, windowWidth, fontSize),
                         UI::Widgets::currentFirstPlotNonPlotHeight(m_GpuFill.firstPlot, windowWidth, fontSize),
                         UI::Widgets::currentFirstPlotNonPlotHeight(m_NpuFill.firstPlot, windowWidth, fontSize),
                         UI::Widgets::currentFirstPlotNonPlotHeight(m_NetworkFill.firstPlot, windowWidth, fontSize)});
    }

    /// NowBar columns every Overview chart reserves: the most bars any of them has, which depends on
    /// whether the platform reports I/O Wait (a fourth CPU bar).
    [[nodiscard]] std::size_t overviewNowBarColumns() const;

    /// Get the hostname (for tab/window title).
    [[nodiscard]] const std::string& hostname() const
    {
        return m_Hostname;
    }

    /// Access the underlying GPU model for sharing with other components.
    [[nodiscard]] std::shared_ptr<Domain::GPUModel> gpuModel() const
    {
        return m_GPUModel;
    }

    /// Brings @p tab forward, asked for on every frame the sub-tabs are drawn until ImGui shows it
    /// selected: the test hooks' TASKSMACK_MACHINE_TAB and TASKSMACK_DEMO's machine= (#2869). A request
    /// outlives frames on which the sub-tab is not drawn (another main tab shown, or the first sample
    /// not yet in), and is dropped, with one warning, after MAX_MACHINE_TAB_FRAMES frames drawn with a
    /// snapshot that never had it (npu without an accelerator, network without counters). A new request
    /// replaces one still waiting.
    void requestTab(SelectOverride::MachineTab tab) noexcept
    {
        m_RequestedTab = SelectOverride::PendingMachineTab(tab);
    }

    /// Whether a requestTab() request is still waiting for its sub-tab to show.
    [[nodiscard]] bool requestedTabPending() const noexcept
    {
        return m_RequestedTab.pending();
    }

  private:
    SystemMetricsProbeFactories m_Factories; ///< Set by the test seams only; empty: the composition root's
    void renderOverview();

    /// Which models their sampler's start hook has seeded (#2731). Shared with the hooks, so neither side
    /// depends on the other's lifetime; waitForFirstSample() polls it.
    struct Seeds
    {
        std::atomic<bool> system{false};
        std::atomic<bool> storage{false};
        std::atomic<bool> gpu{false};

        [[nodiscard]] bool all() const noexcept
        {
            return system.load(std::memory_order_acquire) && storage.load(std::memory_order_acquire) && gpu.load(std::memory_order_acquire);
        }
    };

    /// A sampler for one model on its own named thread (#2731), started: @p hook builds the model's
    /// probe(s) and seeds it there, before the first pass.
    [[nodiscard]] std::unique_ptr<Domain::BackgroundSampler> startSampler(std::string_view threadName,
                                                                          const std::shared_ptr<Domain::ISamplable>& model,
                                                                          std::string name,
                                                                          std::function<void()> hook) const;

    // One sampler per data domain (#2731): CPU, memory, network and power; the disks; the GPUs and NPUs.
    // A slow source holds up only its own thread, and each model publishes with its own timestamp.
    std::unique_ptr<Domain::BackgroundSampler> m_SystemSampler;
    std::unique_ptr<Domain::BackgroundSampler> m_StorageSampler;
    std::unique_ptr<Domain::BackgroundSampler> m_GpuSampler;
    std::shared_ptr<Seeds> m_Seeds;
    // shared_ptr (not unique_ptr): BackgroundSampler observes these models via weak_ptr rather
    // than raw pointers, so the sampler thread can never outlive-dereference them regardless of
    // destructor ordering.
    std::shared_ptr<Domain::SystemModel> m_Model;
    std::shared_ptr<Domain::StorageModel> m_StorageModel;
    std::shared_ptr<Domain::GPUModel> m_GPUModel;
    // Owned by ProcessesPanel; locked where used, so its lifetime never depends on panel detach
    // order (#1176).
    std::weak_ptr<Domain::ProcessModel> m_ProcessModel;
    std::shared_ptr<const Domain::SystemPublication> m_SystemPublication;
    std::shared_ptr<const Domain::StoragePublication> m_StoragePublication;
    std::shared_ptr<const Domain::GPUPublication> m_GPUPublication;
    // Taken (UI::Widgets::nextChartDataGeneration()) whenever any history this panel charts changes --
    // a system, storage or GPU publication adopted, or the process histories copied -- so the charts
    // keep their reduced points until then (HistoryChartConfig::dataGeneration, #1139).
    std::uint64_t m_ChartDataGeneration = 0;
    std::uint64_t m_ProcessHistoryVersion = 0;
    // Shared views of the model's histories, not copies (#2156).
    Domain::HistoryView<double> m_ProcessHistoryTimestamps;
    Domain::HistoryView<double> m_ProcessPowerHistory;
    Domain::HistoryView<double> m_ProcessPageFaultsHistory;
    Domain::HistoryView<double> m_ProcessThreadCountHistory;
    Domain::HistoryView<double> m_ProcessHandleCountHistory;
    // The process probe's capabilities, copied with the process histories (#1254).
    Platform::ProcessCapabilities m_ProcessCapabilities;

    double m_MaxHistorySeconds = Domain::Numeric::toDouble(Domain::Sampling::HISTORY_SECONDS_DEFAULT);

    // Render scratch buffers for stacked CPU breakdown chart (reused across frames to avoid per-frame heap allocation)
    // double, to match the double time axis ImPlot pairs them with (UI::Widgets::fillTimeAxis)
    UI::Widgets::UserSystemStack m_CpuStack; // The User and System bands, shared with Process Details (#1180)
    std::vector<double> m_CpuStackYIowait;
    // The stacked bands' reduced points (#1022), kept until the next publication (#1139)
    UI::Widgets::ReducedPointsCache m_CpuStackReduction;
    UI::Widgets::ReducedValuesStamp m_CpuStackValues; // Whether the band buffers hold its points' values (#843)
    std::vector<double> m_CpuStackYBusy;              // Bottom of the I/O Wait band: the busy total, 100 - idle - iowait

    // The CPU Details block's rows (#809), rebuilt only when what they show changes -- the system and
    // GPU publications, the process count or the process histories -- not every frame (#1171).
    struct CpuDetailsCache
    {
        bool valid = false;
        std::uint64_t systemVersion = 0;
        std::uint64_t gpuVersion = 0;
        std::uint64_t processHistoryVersion = 0;
        std::size_t processCount = 0;
        bool hasProcessModel = false;
        std::uint64_t generation = 0; // Bumped on every rebuild, for CpuDetailsBlock's measurements
        std::vector<CpuDetailsText::Row> rows;
        std::string collapsedSummary; // The heading's text while the block is collapsed
    } m_CpuDetails;
    CpuDetailsBlock::MeasuredRows m_CpuDetailsMeasured; // The rows' widths, kept between frames

    // The battery details rows beneath the Battery chart (#1523): rebuilt once per system sample,
    // and their generation bumped (so they are re-measured) only when the text actually changed.
    struct BatteryDetailsCache
    {
        std::uint64_t systemVersion = 0;
        std::uint64_t generation = 0;
        std::vector<CpuDetailsText::Row> rows;
    } m_BatteryDetails;
    CpuDetailsBlock::MeasuredRows m_BatteryDetailsMeasured;

    std::chrono::milliseconds m_RefreshInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    bool m_ForceRefresh = false;
    float m_LastDeltaSeconds = 0.0F;
    bool m_IsActiveTab = true; // System Overview is default tab
    // The sub-tab requestTab() asked for, until ImGui shows it selected (#2869).
    SelectOverride::PendingMachineTab m_RequestedTab;

    // Previous frame's Overview layout, feeding UI::Widgets::FillPlotLayout (#922)
    UI::Widgets::PlotFillState m_OverviewFill;
    // The GPU tab's, shared by every GPU's charts (#959)
    UI::Widgets::PlotFillState m_GpuFill;
    // The NPU tab's, shared by every NPU's and compute accelerator's charts (#1799)
    UI::Widgets::PlotFillState m_NpuFill;
    // The Network and I/O tab's (#959)
    UI::Widgets::PlotFillState m_NetworkFill;

    struct SmoothedCpu
    {
        double total = 0.0;
        double user = 0.0;
        double system = 0.0;
        double iowait = 0.0;
        double idle = 0.0;
        bool initialized = false;
    } m_SmoothedCpu;

    // Use MemorySection's SmoothedMemory type
    MemorySection::SmoothedMemory m_SmoothedMemory;

    struct SmoothedPower
    {
        double watts = 0.0;
        double batteryChargePercent = 0.0;
        bool initialized = false;
    } m_SmoothedPower;

    struct SmoothedResources
    {
        double threads = 0.0;
        double pageFaults = 0.0;
        double handles = 0.0;
        bool initialized = false;
    } m_SmoothedResources;

    struct SmoothedSystemIO
    {
        double readBytesPerSec = 0.0;
        double writeBytesPerSec = 0.0;
        bool initialized = false;
    } m_SmoothedSystemIO;

    // Per-disk NowBar values for the Network and I/O tab's disk grid (#1012)
    std::unordered_map<std::string, StorageSection::SmoothedDiskRates> m_SmoothedPerDisk;

    struct SmoothedNetwork
    {
        double sentBytesPerSec = 0.0;
        double recvBytesPerSec = 0.0;
        bool initialized = false;
    } m_SmoothedNetwork;

    // Name of the selected network interface; empty means "Total" / all interfaces combined
    std::string m_SelectedNetworkInterface;

    // Interface Status table (#1211): "Show all" for this session only, and the interfaces seen moving
    // traffic, which stay listed while down.
    bool m_ShowAllInterfaces = false;
    NetInterfaceUtils::InterfaceNameSet m_InterfacesWithTraffic;
    // The Network tab's strings and lists, kept until the next publication (#1171)
    NetworkSection::FrameCache m_NetworkFrameCache;

    // GPU smoothed values (uses type from GpuSection)
    std::unordered_map<std::string, GpuSection::SmoothedGPU> m_SmoothedGPUs;
    // The GPU tab's per-frame storage, kept so drawing it allocates nothing once warmed up (#1171).
    GpuSection::FrameCache m_GpuFrameCache;
    // The NPU tab's own (#1799): its smoothed values and per-frame storage, as the GPU tab's above.
    std::unordered_map<std::string, GpuSection::SmoothedGPU> m_SmoothedAccelerators;
    GpuSection::FrameCache m_NpuFrameCache;

    std::vector<double> m_SmoothedPerCore;

    // Cached hostname for UI. The snapshot and timestamps are read from m_SystemPublication (#1180).
    std::string m_Hostname = "System";

    /// Holds the model's latest publication and takes a new chart data generation for it (onAttach()
    /// and onUpdate(), #1180).
    void adoptSystemPublication();
    /// Adopts whichever of the three models' publications is newer than the one held, and the hostname
    /// with the system one: onUpdate()'s step, also run by waitForFirstSample() (#2731).
    void adoptPublications();
    /// The held publication's snapshot, or an empty one before the first publication.
    [[nodiscard]] const Domain::SystemSnapshot& systemSnapshot() const;
    void updateSmoothedCpu(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedMemory(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedPower(float targetWatts, float targetBatteryPercent, float deltaTimeSeconds);
    void updateSmoothedResources(double targetThreads, double targetFaults, double targetHandles, float deltaTimeSeconds);
};

} // namespace App
