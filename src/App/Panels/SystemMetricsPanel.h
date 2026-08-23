#pragma once

#include "App/Panel.h"
#include "App/Panels/GpuSection.h"
#include "App/Panels/MemorySection.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/GPUModel.h"
#include "Domain/ProcessModel.h"
#include "Domain/StorageModel.h"
#include "Domain/StorageSnapshot.h"
#include "Domain/SystemModel.h"
#include "Domain/SystemSnapshot.h"
#include "UI/Theme.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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
    void setSamplingInterval(std::chrono::milliseconds interval);

    /// Request an immediate refresh.
    void requestRefresh();

    /// Inject process model for aggregated system histories (non-owning).
    void setProcessModel(Domain::ProcessModel* model)
    {
        m_ProcessModel = model;
        if (m_ProcessModel != nullptr)
        {
            m_ProcessModel->setMaxHistorySeconds(m_MaxHistorySeconds);
        }
    }

    /// Render the panel (with ImGui window wrapper).
    void render(bool* open) override;
    /// Handle application events (history/refresh changes)
    void onEvent(Core::Event& event) override;

    /// Render content only (for embedding in tab, without window wrapper).
    void renderContent();

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
    void renderCpuSection();

    std::unique_ptr<Domain::BackgroundSampler> m_Sampler;
    std::unique_ptr<Domain::SystemModel> m_Model;
    std::unique_ptr<Domain::StorageModel> m_StorageModel;
    std::shared_ptr<Domain::GPUModel> m_GPUModel;
    Domain::ProcessModel* m_ProcessModel = nullptr; // non-owning
    std::shared_ptr<const Domain::SystemPublication> m_SystemPublication;
    std::shared_ptr<const Domain::StoragePublication> m_StoragePublication;
    std::shared_ptr<const Domain::GPUPublication> m_GPUPublication;
    std::uint64_t m_ProcessHistoryVersion = 0;
    std::vector<double> m_ProcessHistoryTimestamps;
    std::vector<double> m_ProcessPowerHistory;
    std::vector<double> m_ProcessPageFaultsHistory;
    std::vector<double> m_ProcessThreadCountHistory;
    std::vector<double> m_ProcessHandleCountHistory;

    double m_MaxHistorySeconds = 300.0;
    double m_HistoryScrollSeconds = 0.0;
    double m_CurrentNowSeconds = 0.0;
    std::vector<double> m_TimestampsCache;

    // Render scratch buffers for stacked CPU breakdown chart (reused across frames to avoid per-frame heap allocation)
    std::vector<float> m_CpuStackY0;
    std::vector<float> m_CpuStackYUser;
    std::vector<float> m_CpuStackYSystem;
    std::vector<float> m_CpuStackYIowait;

    std::chrono::milliseconds m_RefreshInterval{1000};
    bool m_ForceRefresh = false;
    float m_LastDeltaSeconds = 0.0F;
    bool m_IsActiveTab = true; // System Overview is default tab

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

    struct SmoothedDiskIO
    {
        double readMBps = 0.0;
        double writeMBps = 0.0;
        double avgUtilization = 0.0;
        bool initialized = false;
    } m_SmoothedDiskIO;

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

    struct SmoothedNetwork
    {
        double sentBytesPerSec = 0.0;
        double recvBytesPerSec = 0.0;
        bool initialized = false;
    } m_SmoothedNetwork;

    // Selected network interface (-1 means "Total" / all interfaces combined)
    int m_SelectedNetworkInterface = -1;

    // GPU smoothed values (uses type from GpuSection)
    std::unordered_map<std::string, GpuSection::SmoothedGPU> m_SmoothedGPUs;

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
    void updateSmoothedDiskIO(const Domain::StorageSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedPower(float targetWatts, float targetBatteryPercent, float deltaTimeSeconds);
    void updateSmoothedResources(double targetThreads, double targetFaults, double targetHandles, float deltaTimeSeconds);
};

} // namespace App
