#pragma once

#include "App/Panel.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/IProcessActions.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

// Forward declaration for ImGui draw list
struct ImDrawList;

namespace App
{

/// Panel displaying detailed information for a selected process.
/// Includes resource usage, I/O stats, and process actions.
class ProcessDetailsPanel : public Panel
{
  public:
    ProcessDetailsPanel();
    ~ProcessDetailsPanel() override = default;

    ProcessDetailsPanel(const ProcessDetailsPanel&) = delete;
    ProcessDetailsPanel& operator=(const ProcessDetailsPanel&) = delete;
    ProcessDetailsPanel(ProcessDetailsPanel&&) noexcept = default;
    ProcessDetailsPanel& operator=(ProcessDetailsPanel&&) noexcept = default;

    /// Update with current process data.
    /// Call each frame with the snapshot for the selected process (or nullptr if none).
    void updateWithSnapshot(const Domain::ProcessSnapshot* snapshot, float deltaTime);

    /// Render the panel (with ImGui window wrapper).
    /// @param open Pointer to visibility flag (for window close button).
    void render(bool* open) override;

    /// Render content only (for embedding in tab, without window wrapper).
    void renderContent();

    /// Get a label for this panel (process name or "Select a process").
    [[nodiscard]] std::string tabLabel() const;

    /// Set the process to display.
    void setSelectedPid(std::int32_t pid);

    /// Get currently displayed PID.
    [[nodiscard]] std::int32_t selectedPid() const
    {
        return m_SelectedPid;
    }

  private:
    static void renderBasicInfo(const Domain::ProcessSnapshot& proc);
    void renderResourceUsage(const Domain::ProcessSnapshot& proc);
    void renderThreadAndFaultHistory(const Domain::ProcessSnapshot& proc);
    void renderIoStats(const Domain::ProcessSnapshot& proc);
    void renderNetworkStats(const Domain::ProcessSnapshot& proc);
    void renderPowerUsage(const Domain::ProcessSnapshot& proc);
    void renderGpuUsage(const Domain::ProcessSnapshot& proc);
    void renderActions();
    void trimHistory(double nowSeconds);

    // Priority slider helper methods (extracted for testability and clarity)
    struct PrioritySliderContext;
    static void drawPriorityBadge(ImDrawList* drawList, const PrioritySliderContext& ctx);
    static void drawPriorityGradient(ImDrawList* drawList, const PrioritySliderContext& ctx);
    static void drawPriorityThumb(ImDrawList* drawList, const PrioritySliderContext& ctx);
    void handlePrioritySliderInput(const PrioritySliderContext& ctx);
    static void drawPriorityScaleLabels(const PrioritySliderContext& ctx);
    void updateSmoothedUsage(const Domain::ProcessSnapshot& snapshot, float deltaTimeSeconds);

    std::int32_t m_SelectedPid = -1;
    float m_HistoryTimer = 0.0F;
    float m_LastDeltaSeconds = 0.0F;

    // History buffers (trimmed by time window)
    static constexpr float HISTORY_SAMPLE_INTERVAL = 1.0F;
    std::deque<double> m_CpuHistory;       // CPU% total history (avoid narrowing)
    std::deque<double> m_CpuUserHistory;   // CPU% user history (avoid narrowing)
    std::deque<double> m_CpuSystemHistory; // CPU% system history (avoid narrowing)
    std::deque<double> m_MemoryHistory;    // Used memory percent (RSS)
    std::deque<double> m_SharedHistory;    // Shared memory percent (best effort)
    std::deque<double> m_VirtualHistory;   // Virtual memory percent (best effort)
    std::deque<double> m_ThreadHistory;    // Thread count history
    std::deque<double> m_HandleHistory;    // Handle/FD count history
    std::deque<double> m_PageFaultHistory; // Page faults per second history
    std::deque<double> m_IoReadHistory;    // Disk read rate (bytes/sec)
    std::deque<double> m_IoWriteHistory;   // Disk write rate (bytes/sec)
    std::deque<double> m_NetSentHistory;   // Network send rate (bytes/sec)
    std::deque<double> m_NetRecvHistory;   // Network receive rate (bytes/sec)
    std::deque<double> m_PowerHistory;     // Power usage history (watts)
    std::deque<double> m_GpuUtilHistory;   // GPU utilization % history
    std::deque<double> m_GpuMemHistory;    // GPU memory bytes history
    std::deque<double> m_Timestamps;
    double m_MaxHistorySeconds = 300.0;
    double m_PeakMemoryPercent = 0.0; // Peak working set (never decreases)

    // Cached snapshot for rendering
    Domain::ProcessSnapshot m_CachedSnapshot;
    bool m_HasSnapshot = false;

    // Process actions
    std::unique_ptr<Platform::IProcessActions> m_ProcessActions;
    Platform::ProcessActionCapabilities m_ActionCapabilities;

    // Confirmation dialog state
    bool m_ShowConfirmDialog = false;
    std::string m_ConfirmAction;
    std::string m_LastActionResult;
    float m_ActionResultTimer = 0.0F;

    // Priority adjustment state
    int32_t m_PriorityNiceValue = 0;
    bool m_PriorityChanged = false;
    std::string m_PriorityError; // Persistent error message for priority changes

    struct SmoothedUsage
    {
        double cpuPercent = 0.0;
        double cpuUserPercent = 0.0;
        double cpuSystemPercent = 0.0;
        double residentBytes = 0.0;
        double virtualBytes = 0.0;
        double threadCount = 0.0;
        double handleCount = 0.0;
        double pageFaultsPerSec = 0.0;
        double ioReadBytesPerSec = 0.0;
        double ioWriteBytesPerSec = 0.0;
        double netSentBytesPerSec = 0.0;
        double netRecvBytesPerSec = 0.0;
        double powerWatts = 0.0;
        double gpuUtilPercent = 0.0;
        double gpuMemoryBytes = 0.0;
        bool initialized = false;
    } m_SmoothedUsage;
};

} // namespace App
