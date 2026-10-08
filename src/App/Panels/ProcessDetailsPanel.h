#pragma once

#include "App/KeyboardShortcuts.h"
#include "App/Panel.h"
#include "Core/Event.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/ProcessTypes.h"
#include "ProcessActionsView.h"
#include "ProcessDetailsCharts.h"
#include "ProcessDetailsHistory.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"
#include "ProcessPriorityView.h"
#include "ProcessSmoothedUsage.h"
#include "UI/FillPlotLayout.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

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

    /// F9 (#170): on this frame's render, opens the Actions tab's Kill confirm for the process shown,
    /// through ProcessActionsView::requestKillShortcut() (target captured then, never a direct kill),
    /// and brings the Actions tab forward so the dialog is drawn. Does nothing while this pane is not
    /// showing a running process, or when the platform cannot kill. Lives for this frame only:
    /// expireFrameRequests() drops it if the tabs were not drawn.
    void requestKillSelected() noexcept
    {
        m_KillShortcut.request();
    }

    /// End of frame: drop a shortcut request this frame's render did not take (#170).
    void expireFrameRequests() noexcept
    {
        m_KillShortcut.expire();
    }

  private:
    void renderBasicInfo(const Domain::ProcessSnapshot& proc);
    void renderActions();
    /// What the chart tabs draw this frame: the history, the smoothed now-bar values and the displayed
    /// snapshot, by pointer for this frame's calls only (ProcessDetailsCharts, #1179).
    [[nodiscard]] ProcessChartContext chartContext() const;
    /// The selected process as an action target: its PID and, once a snapshot has confirmed it,
    /// its start time, so a reuse of the PID is refused rather than acted on (#973).
    [[nodiscard]] Platform::ProcessTarget selectedTarget() const;
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

    // The selected process as last sampled, shared with ProcessModel's sample rather than copied
    // every frame (#1172); null before the first sample.
    std::shared_ptr<const Domain::ProcessSnapshot> m_CachedSnapshot;
    // Which of m_CachedSnapshot's I/O and network rates are readings, by its own generation (#1210).
    Detail::SampleRateReadings m_CachedRateReadings;

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
    // The Overview's state for the shared chart-height rule (#959); the Network and GPU tabs' are
    // m_Charts'.
    UI::Widgets::PlotFillState m_OverviewFill;

    bool m_HasSnapshot = false;
    bool m_ProcessExited = false; // Had a snapshot of the selected process, and it has gone missing (#927)

    // Process actions
    std::unique_ptr<Platform::IProcessActions> m_ProcessActions;
    Platform::ProcessActionCapabilities m_ActionCapabilities;
    Platform::ProcessCapabilities m_ProcessCapabilities;

    // The Actions tab's buttons, confirm dialog and result line, and the priority control under them
    // (#1179). Both act through m_ProcessActions, which the panel keeps owning.
    ProcessActionsView m_ActionsView;
    KeyboardShortcuts::FrameRequest m_KillShortcut; // F9, taken by renderContent() this frame
    bool m_SelectActionsTab = false;                // F9 asked for the Kill confirm: show the Actions tab, which draws it
    ProcessPriorityView m_PriorityView;

    // The smoothed NowBar values, eased toward each shown sample (#1179).
    Detail::ProcessSmoothedUsage m_SmoothedUsage;

    // The Overview's charts and the GPU and Network and I/O tabs (#1179). They read m_History,
    // m_SmoothedUsage and the snapshot through chartContext(), passed in each frame.
    ProcessDetailsCharts m_Charts;
};

} // namespace App
