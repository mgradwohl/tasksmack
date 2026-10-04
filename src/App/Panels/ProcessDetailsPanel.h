#pragma once

#include "App/Panel.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/ProcessTypes.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/FillPlotLayout.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

    /// Construct with an injected IProcessActions implementation instead of the real
    /// platform one. Intended for tests (a mock IProcessActions) - production code should
    /// use the default constructor, which is the composition-root call to
    /// Platform::makeProcessActions().
    explicit ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions);

    ~ProcessDetailsPanel() override = default;

    ProcessDetailsPanel(const ProcessDetailsPanel&) = delete;
    ProcessDetailsPanel& operator=(const ProcessDetailsPanel&) = delete;
    ProcessDetailsPanel(ProcessDetailsPanel&&) noexcept = default;
    ProcessDetailsPanel& operator=(ProcessDetailsPanel&&) noexcept = default;

    /// Update with current process data.
    /// Call each frame with the snapshot for the selected process (or nullptr if none).
    void updateWithSnapshot(const Domain::ProcessSnapshot* snapshot, std::uint64_t snapshotVersion, float deltaTime);

    /// Render the panel (with ImGui window wrapper).
    /// @param open Pointer to visibility flag (for window close button).
    void render(bool* open) override;

    /// Render content only (for embedding in tab, without window wrapper).
    void renderContent() override;

    /// Get a label for this panel (process name or "Select a process").
    /// Returned by reference so a caller can compare it against a cached copy every frame without
    /// allocating. The reference is valid until the next updateWithSnapshot() or selection change.
    [[nodiscard]] const std::string& tabLabel() const;

    /// Handle application events (process selection, active tab, refresh interval, history window)
    void onEvent(Core::Event& event) override;

    /// Set the process to display.
    /// @param uniqueKey Identity of the process (hash of PID and start time), or 0 if not known.
    ///        With it, a later process that reuses the PID is not mistaken for the selected one.
    void setSelectedPid(std::int32_t pid, std::uint64_t uniqueKey = 0);

    /// What the process probe can report, so series it never fills are not drawn (#1028, #1035).
    /// Set once by ShellLayer at attach; the default (all false) hides those optional series.
    void setProcessCapabilities(const Platform::ProcessCapabilities& capabilities);

    /// Get currently displayed PID.
    [[nodiscard]] std::int32_t selectedPid() const
    {
        return m_SelectedPid;
    }

    /// What process actions the underlying IProcessActions supports. Exposed publicly so
    /// tests constructing this panel with an injected mock can assert the capability flags
    /// were actually pulled from it.
    [[nodiscard]] const Platform::ProcessActionCapabilities& actionCapabilities() const
    {
        return m_ActionCapabilities;
    }

  private:
    /// Action pending confirmation in the "Confirm Action" popup - see
    /// ProcessDetailsPanel_ActionHelpers.h for why this is a type alias rather than a
    /// member enum.
    using ProcessAction = Detail::ProcessAction;

    static void renderBasicInfo(const Domain::ProcessSnapshot& proc);
    void renderResourceUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill);
    void renderCpuUsageSection(UI::Widgets::FillPlotLayout& fill);
    void renderMemoryUsageSection(UI::Widgets::FillPlotLayout& fill);
    void renderThreadAndFaultHistory(UI::Widgets::FillPlotLayout& fill);
    void renderIoStats(UI::Widgets::FillPlotLayout& fill);
    void renderNetworkStats(UI::Widgets::FillPlotLayout& fill);
    void renderPowerUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill);
    void renderGpuUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill);
    void renderGpuCurrentMetricsTable(const Domain::ProcessSnapshot& proc) const;
    static void renderPerGpuBreakdown(const Domain::ProcessSnapshot& proc);
    void renderGpuHistoryGraphs(UI::Widgets::FillPlotLayout& fill);
    void renderActions();
    void renderActionResultFeedback();
    void renderConfirmDialog();
    void dispatchConfirmedAction();
    /// The selected process as an action target: its PID and, once a snapshot has confirmed it,
    /// its start time, so a reuse of the PID is refused rather than acted on (#973).
    [[nodiscard]] Platform::ProcessTarget selectedTarget() const;
    void renderActionButtons();
    void renderPrioritySection();
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
    std::uint64_t m_SelectedUniqueKey = 0; // 0 = not known; adopted from the first snapshot
    std::uint64_t m_LastHistorySnapshotVersion = 0;
    float m_LastDeltaSeconds = 0.0F;
    bool m_IsActiveTab = false;

    // History buffers (trimmed by time window). Vectors, not deques: the charts plot the newest
    // samples in place through spans, where a deque had to be copied out every frame (#1018).
    // Trimming erases from the front, once per sample, not per frame.
    std::vector<double> m_CpuHistory;       // CPU% total history (avoid narrowing)
    std::vector<double> m_CpuUserHistory;   // CPU% user history (avoid narrowing)
    std::vector<double> m_CpuSystemHistory; // CPU% system history (avoid narrowing)
    std::vector<double> m_MemoryHistory;    // Used memory percent (RSS)
    std::vector<double> m_SharedHistory;    // Shared memory percent (best effort)
    std::vector<double> m_VirtualHistory;   // Virtual memory bytes (#992)
    std::vector<double> m_ThreadHistory;    // Thread count history
    std::vector<double> m_HandleHistory;    // Handle/FD count history
    std::vector<double> m_PageFaultHistory; // Page faults per second history
    std::vector<double> m_IoReadHistory;    // Disk read rate (bytes/sec)
    std::vector<double> m_IoWriteHistory;   // Disk write rate (bytes/sec)
    std::vector<double> m_NetSentHistory;   // Network send rate (bytes/sec)
    std::vector<double> m_NetRecvHistory;   // Network receive rate (bytes/sec)
    std::vector<double> m_PowerHistory;     // Power usage history (watts)
    std::vector<double> m_GpuUtilHistory;   // GPU utilization % history
    std::vector<double> m_GpuMemHistory;    // GPU memory bytes history
    std::vector<double> m_GdiHistory;       // GDI object count history (Windows-only)
    std::vector<double> m_Timestamps;
    // Refresh interval and history window start at the SamplingConfig defaults; ShellLayer raises the
    // configured values as events on its first update (#1079).
    double m_MaxHistorySeconds = Domain::Numeric::toDouble(Domain::Sampling::HISTORY_SECONDS_DEFAULT);
    // Sampling interval the NowBar smoothing is tuned to (#1072)
    std::chrono::milliseconds m_RefreshInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    double m_PeakMemoryPercent = 0.0; // Peak working set (never decreases)

    // Render scratch buffers for stacked CPU chart (reused across frames to avoid per-frame heap allocation)
    std::vector<double> m_CpuPlotX; // CPU chart points as drawn, held to now (#1016)
    std::vector<double> m_CpuPlotTotal;
    std::vector<double> m_CpuPlotUser;
    std::vector<double> m_CpuPlotSystem;
    std::vector<double> m_CpuStackY0;
    std::vector<double> m_CpuStackYUser;
    std::vector<double> m_CpuStackYSystem;

    // Cached snapshot for rendering
    Domain::ProcessSnapshot m_CachedSnapshot;
    // Per-tab state for the shared chart-height rule (#959)
    UI::Widgets::PlotFillState m_OverviewFill;
    UI::Widgets::PlotFillState m_NetworkFill;
    UI::Widgets::PlotFillState m_GpuFill;

    bool m_HasSnapshot = false;
    bool m_ProcessExited = false; // Had a snapshot of the selected process, and it has gone missing (#927)

    // Process actions
    std::unique_ptr<Platform::IProcessActions> m_ProcessActions;
    Platform::ProcessActionCapabilities m_ActionCapabilities;
    Platform::ProcessCapabilities m_ProcessCapabilities;

    // Confirmation dialog state
    bool m_ShowConfirmDialog = false;
    ProcessAction m_ConfirmAction = ProcessAction::None;
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
        double gdiObjectCount = 0.0;
        // Whether the latest sample had a GDI reading. A missing one leaves gdiObjectCount where it
        // was (not eased toward 0) and the NowBar shows N/A, as the line shows a gap (#1148).
        bool gdiInitialized = false;
        // Memory bars, as percents of system RAM like the Memory chart
        double memoryUsedPercent = 0.0;
        double memorySharedPercent = 0.0;
        bool initialized = false;
    } m_SmoothedUsage;

    // GPU logging throttle state (per-panel tracking)
    std::int32_t m_LastGpuLogPid = -1;
    std::uint64_t m_LastGpuLogMemoryBytes = std::numeric_limits<std::uint64_t>::max();
};

} // namespace App
