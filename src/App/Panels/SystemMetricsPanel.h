#pragma once

#include "App/Panel.h"
#include "Domain/GPUModel.h"
#include "Domain/ProcessModel.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "UI/Theme.h"

#include <implot.h>

#include <chrono>
#include <memory>
#include <unordered_map>

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

    /// Render content only (for embedding in tab, without window wrapper).
    void renderContent();

    /// Get the hostname (for tab/window title).
    [[nodiscard]] const std::string& hostname() const
    {
        return m_Hostname;
    }

  private:
    void renderOverview();
    void renderCpuSection();
    void renderPerCoreSection();
    void renderGpuSection();

    std::unique_ptr<Domain::SystemModel> m_Model;
    std::unique_ptr<Domain::StorageModel> m_StorageModel;
    std::unique_ptr<Domain::GPUModel> m_GPUModel;
    Domain::ProcessModel* m_ProcessModel = nullptr; // non-owning

    double m_MaxHistorySeconds = 300.0;
    double m_HistoryScrollSeconds = 0.0;
    double m_CurrentNowSeconds = 0.0;
    std::vector<double> m_TimestampsCache;

    std::chrono::milliseconds m_RefreshInterval{1000};
    float m_RefreshAccumulatorSec = 0.0F;
    bool m_ForceRefresh = false;
    float m_LastDeltaSeconds = 0.0F;

    struct SmoothedCpu
    {
        double total = 0.0;
        double user = 0.0;
        double system = 0.0;
        double iowait = 0.0;
        double idle = 0.0;
        bool initialized = false;
    } m_SmoothedCpu;

    struct SmoothedMemory
    {
        double usedPercent = 0.0;
        double cachedPercent = 0.0;
        double swapPercent = 0.0;
        bool initialized = false;
    } m_SmoothedMemory;

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

    struct SmoothedThreadsFaults
    {
        double threads = 0.0;
        double pageFaults = 0.0;
        bool initialized = false;
    } m_SmoothedThreadsFaults;

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

    struct SmoothedGPU
    {
        double utilizationPercent = 0.0;
        double memoryPercent = 0.0;
        double temperatureC = 0.0;
        double powerWatts = 0.0;
        bool initialized = false;
    };
    std::unordered_map<std::string, SmoothedGPU> m_SmoothedGPUs;

    std::vector<double> m_SmoothedPerCore;

    // Cached layout values (recalculated one frame after font changes)
    UI::FontSize m_LastFontSize = UI::FontSize::Medium;
    float m_OverviewLabelWidth = 0.0F;
    float m_PerCoreLabelWidth = 0.0F;
    int m_LastCoreCount = 0;
    bool m_LayoutDirty = true; // Start dirty to calculate on first frame

    // Cached hostname for window title
    std::string m_Hostname = "System";

    void updateCachedLayout();
    void updateSmoothedCpu(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedMemory(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedPerCore(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedDiskIO(const Domain::StorageSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedPower(float targetWatts, float targetBatteryPercent, float deltaTimeSeconds);
    void updateSmoothedThreadsFaults(double targetThreads, double targetFaults, float deltaTimeSeconds);
    void updateSmoothedSystemIO(double targetRead, double targetWrite, float deltaTimeSeconds);
    void updateSmoothedNetwork(double targetSent, double targetRecv, float deltaTimeSeconds);
    void updateSmoothedGPU(const std::string& gpuId, const Domain::GPUSnapshot& snap, float deltaTimeSeconds);
};

} // namespace App
