#pragma once

#include "App/Panel.h"
#include "App/TabLabel.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/ProcessTypes.h"
#include "ProcessActionsView.h"
#include "ProcessDetailsHistory.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
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
    /// Platform::makeProcessActions() (or, under TASKSMACK_SYNTHETIC, the synthetic scenario's
    /// refusing actions: App/SyntheticScenario.h).
    explicit ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions);

    ~ProcessDetailsPanel() override = default;

    ProcessDetailsPanel(const ProcessDetailsPanel&) = delete;
    ProcessDetailsPanel& operator=(const ProcessDetailsPanel&) = delete;
    ProcessDetailsPanel(ProcessDetailsPanel&&) noexcept = default;
    ProcessDetailsPanel& operator=(ProcessDetailsPanel&&) noexcept = default;

    /// Update with the selected process's new samples. Call each frame with what
    /// Domain::ProcessModel::watchedSamplesSince(lastSampleVersion()) returned for the watched
    /// selectedPid() -- oldest first, empty when nothing new was published. Each sample of the selected
    /// process becomes one history point, stamped with when it was sampled (#1098); the newest is
    /// shown, shared rather than copied (#1172).
    void updateWithSamples(std::span<const Domain::ProcessSample> samples, float deltaTime);

    /// The newest generation taken in since the selection (0 = none): what to pass to
    /// Domain::ProcessModel::watchedSamplesSince(). Reset to 0 when the selection changes.
    [[nodiscard]] std::uint64_t lastSampleVersion() const
    {
        return m_SampleIntake.lastVersion;
    }

    /// The selected process as last sampled, or nullptr when no sample of it has arrived since the
    /// selection. Valid until the next updateWithSamples() or selection change.
    [[nodiscard]] const Domain::ProcessSnapshot* displayedSnapshot() const
    {
        return m_HasSnapshot ? m_CachedSnapshot.get() : nullptr;
    }

    /// Render the panel (with ImGui window wrapper).
    /// @param open Pointer to visibility flag (for window close button).
    void render(bool* open) override;

    /// Render content only (for embedding in tab, without window wrapper).
    void renderContent() override;

    /// Get a label for this panel (process name or "Select a process").
    /// Returned by reference so a caller can compare it against a cached copy every frame without
    /// allocating. The reference is valid until the next updateWithSamples() or selection change.
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
    void renderBasicInfo(const Domain::ProcessSnapshot& proc);
    void renderResourceUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill);
    void renderCpuUsageSection(UI::Widgets::FillPlotLayout& fill);
    void renderMemoryUsageSection(UI::Widgets::FillPlotLayout& fill);
    void renderThreadAndFaultHistory(UI::Widgets::FillPlotLayout& fill);
    void renderIoStats(UI::Widgets::FillPlotLayout& fill);
    void renderNetworkStats(UI::Widgets::FillPlotLayout& fill);
    void renderPowerUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill);
    void renderGpuUsage(const Domain::ProcessSnapshot& proc, UI::Widgets::FillPlotLayout& fill);
    void renderGpuCurrentMetricsTable(const Domain::ProcessSnapshot& proc) const;
    void renderPerGpuBreakdown(const Domain::ProcessSnapshot& proc) const;
    void renderGpuHistoryGraphs(UI::Widgets::FillPlotLayout& fill);
    void renderActions();
    /// The selected process as an action target: its PID and, once a snapshot has confirmed it,
    /// its start time, so a reuse of the PID is refused rather than acted on (#973).
    [[nodiscard]] Platform::ProcessTarget selectedTarget() const;
    void renderPrioritySection();

    // Priority slider helper methods (extracted for testability and clarity)
    struct PrioritySliderContext;
    static void drawPriorityBadge(ImDrawList* drawList, const PrioritySliderContext& ctx);
    static void drawPriorityGradient(ImDrawList* drawList, const PrioritySliderContext& ctx);
    static void drawPriorityThumb(ImDrawList* drawList, const PrioritySliderContext& ctx);
    void handlePrioritySliderInput(const PrioritySliderContext& ctx);
    static void drawPriorityScaleLabels(const PrioritySliderContext& ctx);
    void updateSmoothedUsage(const Domain::ProcessSnapshot& snapshot, float deltaTimeSeconds);
    /// Appends one history point for @p snapshot at @p sampleTimeSeconds, after a gap point when
    /// @p gapBefore (see Detail::takeSamples()). @p rateReadings says which of its I/O and network rates
    /// are readings, by the sample's own generation (Detail::rateReadings()); the others are gaps.
    void recordHistoryPoint(const Domain::ProcessSnapshot& snapshot,
                            double sampleTimeSeconds,
                            bool gapBefore,
                            Detail::SampleRateReadings rateReadings);
    /// The displayed snapshot, or an empty one before the first: for code that draws it unconditionally.
    [[nodiscard]] const Domain::ProcessSnapshot& cachedSnapshot() const;

    std::int32_t m_SelectedPid = -1;
    std::uint64_t m_SelectedUniqueKey = 0; // 0 = not known; adopted from the first snapshot
    Detail::SampleIntake m_SampleIntake;   // Where history recording is in the watched process's samples (#1098)
    float m_LastDeltaSeconds = 0.0F;
    bool m_IsActiveTab = false;

    // The time axis and every per-process series, appended, trimmed to m_MaxHistorySeconds and cleared
    // together, so no series can fall out of step with the axis (#1179).
    Detail::ProcessDetailsHistory m_History;
    // Taken (UI::Widgets::nextChartDataGeneration()) whenever the histories above change -- a sample
    // recorded or trimmed, or the selection reset -- so the charts keep their reduced points until
    // then instead of reducing every history every frame (HistoryChartConfig::dataGeneration, #1139).
    std::uint64_t m_HistoryGeneration = 0;
    // Refresh interval and history window start at the SamplingConfig defaults; ShellLayer raises the
    // configured values as events on its first update (#1079).
    double m_MaxHistorySeconds = Domain::Numeric::toDouble(Domain::Sampling::HISTORY_SECONDS_DEFAULT);
    // Sampling interval the NowBar smoothing is tuned to (#1072)
    std::chrono::milliseconds m_RefreshInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    double m_PeakMemoryBytes = 0.0; // Peak working set (never decreases)

    // Render scratch buffers for stacked CPU chart (reused across frames to avoid per-frame heap allocation):
    // only the reduced points, at most LINE_PLOT_MAX_POINTS_DENSE, are built into them each frame.
    // The User and System bands, their x axis held to now (#1016), shared with the Overview (#1180);
    // the User line is the User band's top.
    UI::Widgets::UserSystemStack m_CpuStack;
    std::vector<double> m_CpuPlotTotal;
    std::vector<double> m_CpuPlotSystem;
    UI::Widgets::ReducedPointsCache m_CpuPlotReduction; // The CPU chart's reduced points (#1022), kept per m_HistoryGeneration (#1139)

    // The selected process as last sampled, shared with ProcessModel's sample rather than copied
    // every frame (#1172); null before the first sample.
    std::shared_ptr<const Domain::ProcessSnapshot> m_CachedSnapshot;
    // Which of m_CachedSnapshot's I/O and network rates are readings, by its own generation (#1210).
    Detail::SampleRateReadings m_CachedRateReadings;

    // render()'s window title, rebuilt only when the selected process's name changes rather than
    // every frame (#1326).
    TabLabel::CachedLabel m_WindowLabel;

    // The Overview's Identity/Runtime values formatted from one snapshot, kept until a different one
    // is shown, so the block is not reformatted every frame (#1171). keepAlive holds that snapshot,
    // so a later one cannot be allocated at the same address and pass for it.
    struct BasicInfoText
    {
        const Domain::ProcessSnapshot* key = nullptr;
        std::shared_ptr<const Domain::ProcessSnapshot> keepAlive;
        std::string pid;
        std::string parentPid;
        std::string started;
        std::string threads;
        std::string handles;
        std::string cpuTime;
        std::string priority;
    } m_BasicInfoText;
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
    // The GPU tab's "No GPU usage" explanation, naming the history window (#1210). Empty until built,
    // and cleared when the window changes so the next frame rebuilds it.
    std::string m_NoGpuUsageDetail;

    // The Actions tab's buttons, confirm dialog and result line (#1179). It dispatches through
    // m_ProcessActions, which the panel keeps owning; the priority control below uses it too.
    ProcessActionsView m_ActionsView;

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
        // Whether the latest sample's generation had these from the GPU probe (#1210): one it did not
        // leaves the value where it was and shows N/A, like the I/O and network readings above.
        bool gpuUtilAvailable = false;
        bool gpuMemoryAvailable = false;
        // Whether the latest sample had these readings (#1110): an unread one leaves its value where it
        // was and shows N/A, as its line shows a gap, like the GDI count below.
        bool handleCountAvailable = false;
        bool ioAvailable = false;
        bool networkAvailable = false;
        double gdiObjectCount = 0.0;
        // Whether the latest sample had a GDI reading. A missing one leaves gdiObjectCount where it
        // was (not eased toward 0) and the NowBar shows N/A, as the line shows a gap (#1148).
        bool gdiInitialized = false;
        // Memory bars, in bytes like the Memory chart (#1195); Used is residentBytes above. Their share
        // of system RAM is shown only in the hover text, via memoryPercentPerByte.
        double memorySharedBytes = 0.0;
        double memoryPercentPerByte = 0.0; ///< Latest, not smoothed: converts bytes to a share of RAM
        bool initialized = false;
    } m_SmoothedUsage;

    // GPU logging throttle state (per-panel tracking)
    std::int32_t m_LastGpuLogPid = -1;
    std::uint64_t m_LastGpuLogMemoryBytes = std::numeric_limits<std::uint64_t>::max();
};

} // namespace App
