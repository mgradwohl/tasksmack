#pragma once

#include "App/Panel.h"
#include "App/Panels/GpuSection.h"
#include "App/Panels/MemorySection.h"
#include "App/Panels/NetInterfaceUtils.h"
#include "App/Panels/NetworkSection.h"
#include "App/Panels/StorageSection.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/GPUModel.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/StorageModel.h"
#include "Domain/StorageSnapshot.h"
#include "Domain/SystemModel.h"
#include "Domain/SystemSnapshot.h"
#include "Platform/ProcessTypes.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"
#include "UI/Theme.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App
{

/// Panel displaying system-wide metrics with ImPlot graphs.
/// Shows CPU, memory, swap, disk I/O, and GPU usage over time.
class SystemMetricsPanel : public Panel
{
  public:
    SystemMetricsPanel();
    ~SystemMetricsPanel() override;

    SystemMetricsPanel(const SystemMetricsPanel&) = delete;
    SystemMetricsPanel& operator=(const SystemMetricsPanel&) = delete;
    SystemMetricsPanel(SystemMetricsPanel&&) = delete;
    SystemMetricsPanel& operator=(SystemMetricsPanel&&) = delete;

    /// Initialize the panel (creates SystemModel and StorageModel).
    void onAttach() override;

    /// Cleanup.
    void onDetach() override;

    /// Update logic (refresh cadence is driven by main loop).
    void onUpdate(float deltaTime) override;

    /// Set the refresh interval (applied by onUpdate cadence checks).
    void setSamplingInterval(std::chrono::milliseconds interval, bool forceSample = true);

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

    /// Render the panel (with ImGui window wrapper).
    void render(bool* open) override;
    /// Handle application events (history/refresh changes)
    void onEvent(Core::Event& event) override;

    /// Render content only (for embedding in tab, without window wrapper).
    void renderContent() override;

    /// Width of the Overview's NowBar column, including the cell padding that separates it from the
    /// plot, at the current font and style. For the window's content minimum (#1207); needs a frame.
    [[nodiscard]] float overviewNowBarColumnWidth() const;

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

  private:
    void renderOverview();

    std::unique_ptr<Domain::BackgroundSampler> m_Sampler;
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
    std::vector<double> m_ProcessHistoryTimestamps;
    std::vector<double> m_ProcessPowerHistory;
    std::vector<double> m_ProcessPageFaultsHistory;
    std::vector<double> m_ProcessThreadCountHistory;
    std::vector<double> m_ProcessHandleCountHistory;
    // The process probe's capabilities, copied with the process histories (#1254).
    Platform::ProcessCapabilities m_ProcessCapabilities;

    double m_MaxHistorySeconds = Domain::Numeric::toDouble(Domain::Sampling::HISTORY_SECONDS_DEFAULT);
    double m_HistoryScrollSeconds = 0.0;
    double m_CurrentNowSeconds = 0.0;
    std::vector<double> m_TimestampsCache;

    // Render scratch buffers for stacked CPU breakdown chart (reused across frames to avoid per-frame heap allocation)
    // double, to match the double time axis ImPlot pairs them with (UI::Widgets::fillTimeAxis)
    UI::Widgets::UserSystemStack m_CpuStack; // The User and System bands, shared with Process Details (#1180)
    std::vector<double> m_CpuStackYIowait;
    // The stacked bands' reduced points (#1022), kept until the next publication (#1139)
    UI::Widgets::ReducedPointsCache m_CpuStackReduction;
    std::vector<double> m_CpuStackYBusy; // Bottom of the I/O Wait band: the busy total, 100 - idle - iowait
    // The battery charge history as charted, "no reading" (-1) as NaN; rebuilt in place each frame (#1171)
    std::vector<float> m_BatteryChartHistory;

    // The Overview header's text, rebuilt only when what it shows changes -- the system and GPU
    // publications or the process count -- rather than formatted every frame (#1171).
    struct OverviewHeaderText
    {
        bool valid = false;
        std::uint64_t systemVersion = 0;
        std::uint64_t gpuVersion = 0;
        std::size_t processCount = 0;
        bool hasProcessModel = false;
        std::string uptime;
        std::string coreInfo;
        std::string processes;
        std::string memory;
    } m_OverviewHeader;

    std::chrono::milliseconds m_RefreshInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    bool m_ForceRefresh = false;
    float m_LastDeltaSeconds = 0.0F;
    bool m_IsActiveTab = true; // System Overview is default tab

    // Previous frame's Overview layout, feeding UI::Widgets::FillPlotLayout (#922)
    UI::Widgets::PlotFillState m_OverviewFill;
    // The GPU tab's, shared by every GPU's charts (#959)
    UI::Widgets::PlotFillState m_GpuFill;
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

    std::vector<double> m_SmoothedPerCore;

    // Cached layout values (recalculated one frame after font changes)
    UI::FontSize m_LastFontSize = UI::FontSize::Medium;
    float m_OverviewLabelWidth = 0.0F;
    float m_PerCoreLabelWidth = 0.0F;
    int m_LastCoreCount = 0;
    bool m_LayoutDirty = true; // Start dirty to calculate on first frame

    // Cached hostname and snapshot for UI
    std::string m_Hostname = "System";
    Domain::SystemSnapshot m_CachedSnapshot;

    void updateCachedLayout();
    void updateSmoothedCpu(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedMemory(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedPower(float targetWatts, float targetBatteryPercent, float deltaTimeSeconds);
    void updateSmoothedResources(double targetThreads, double targetFaults, double targetHandles, float deltaTimeSeconds);
};

} // namespace App
