#pragma once

#include "App/KeyboardShortcuts.h"
#include "App/Panel.h"
#include "App/SelectOverride.h"
#include "Core/Event.h"
#include "Domain/CrashHistory.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/ServiceModel.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IProcessThreads.h"
#include "Platform/ProcessTypes.h"
#include "ProcessActionsView.h"
#include "ProcessConnectionsView.h"
#include "ProcessCrashHistoryView.h"
#include "ProcessDetailsCharts.h"
#include "ProcessDetailsHistory.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"
#include "ProcessEnvironmentView.h"
#include "ProcessModulesView.h"
#include "ProcessOpenFilesView.h"
#include "ProcessPriorityView.h"
#include "ProcessSecurityView.h"
#include "ProcessSmoothedUsage.h"
#include "ProcessThreadsView.h"
#include "UI/FillPlotLayout.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace App
{

/// Process Details' actions and per-process readers (made by PanelPlatforms.h's makeProcessDetailsReaders(), #2137).
struct ProcessDetailsReaders
{
    std::unique_ptr<Platform::IProcessActions> actions;
    std::unique_ptr<Platform::IProcessEnvironmentReader> environment;
    std::unique_ptr<Platform::IProcessConnectionsReader> connections;
    std::unique_ptr<Platform::IProcessModulesReader> modules;
    std::unique_ptr<Platform::IProcessSecurityReader> security;
    std::unique_ptr<Platform::IProcessOpenFilesReader> openFiles;
    std::unique_ptr<Platform::IProcessThreadsReader> threads; ///< #3069
};

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

    /// Construct with injected IProcessActions and IProcessEnvironmentReader implementations (tests: mocks).
    /// A null reader, like the one-argument constructor's, hides the Environment section.
    ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions,
                        std::unique_ptr<Platform::IProcessEnvironmentReader> environmentReader);

    /// Construct with injected IProcessActions, IProcessEnvironmentReader and IProcessConnectionsReader
    /// implementations (tests: mocks). A null connections reader, like the shorter constructors', hides
    /// the Connections section.
    ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions,
                        std::unique_ptr<Platform::IProcessEnvironmentReader> environmentReader,
                        std::unique_ptr<Platform::IProcessConnectionsReader> connectionsReader);

    /// As above, with an injected IProcessModulesReader too (tests: mocks). A null modules reader, like
    /// the shorter constructors', hides the Modules section (#802).
    ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions,
                        std::unique_ptr<Platform::IProcessEnvironmentReader> environmentReader,
                        std::unique_ptr<Platform::IProcessConnectionsReader> connectionsReader,
                        std::unique_ptr<Platform::IProcessModulesReader> modulesReader);

    /// As above, with an injected IProcessSecurityReader and IProcessOpenFilesReader too (tests: mocks). A
    /// null reader, like the shorter constructors', hides its section (Security #1526, Open files #183).
    ProcessDetailsPanel(std::unique_ptr<Platform::IProcessActions> processActions,
                        std::unique_ptr<Platform::IProcessEnvironmentReader> environmentReader,
                        std::unique_ptr<Platform::IProcessConnectionsReader> connectionsReader,
                        std::unique_ptr<Platform::IProcessModulesReader> modulesReader,
                        std::unique_ptr<Platform::IProcessSecurityReader> securityReader,
                        std::unique_ptr<Platform::IProcessOpenFilesReader> openFilesReader = nullptr);

    ~ProcessDetailsPanel() override = default;

    /// The Threads section's reader (#3069); null, the default, hides the section. Call before the first
    /// update; the default constructor sets the platform's.
    void setThreadsReader(std::unique_ptr<Platform::IProcessThreadsReader> reader)
    {
        m_ThreadsReader = std::move(reader);
        m_HasThreads = m_ThreadsReader != nullptr && m_ThreadsReader->hasThreads();
    }

    /// With @p readers (PanelPlatforms.h): the default constructor's, made for the active scenario (#2137).
    explicit ProcessDetailsPanel(ProcessDetailsReaders readers);

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

    /// Take the selected process's new samples into its history, and nothing else: the part of
    /// updateWithSamples() that can't wait for a frame. Called while the window is hidden and draws
    /// no frames (#3156, ShellPanels::updateWhileHidden()), so a long minimize loses no generation
    /// from the charts when the model's ring of them would wrap; smoothing and the on-demand reads
    /// wait for the next frame.
    void takeSamples(std::span<const Domain::ProcessSample> samples);

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

    /// The tab's text while no process is selected; the main tab bar's minimum width is measured
    /// with it (#1939).
    static constexpr const char* NO_SELECTION_TAB_TEXT = "Select a process";

    /// Get a label for this panel (process name or NO_SELECTION_TAB_TEXT); with several processes
    /// selected in the Processes table, "name (PID n)", so it says which of them is shown (#1926).
    /// Returned by reference so a caller can compare it against a cached copy every frame without
    /// allocating. The reference is valid until the next tabLabel(), updateWithSamples() or selection
    /// change.
    [[nodiscard]] const std::string& tabLabel() const;

    /// How many processes are selected in the Processes table (#804), each frame from the composition
    /// root: with more than one, tabLabel() names the shown process's PID too (#1926).
    void setSelectionCount(std::size_t count) noexcept
    {
        m_SelectionCount = count;
    }

    /// Handle application events (process selection, active tab, refresh interval, history window)
    void onEvent(Core::Event& event) override;

    /// Set the process to display.
    /// @param startTimeTicks The process's raw start time (Domain::ProcessSnapshot::startTimeTicks).
    ///        With it, a later process that reuses the PID is not mistaken for the selected one (#1503).
    void setSelectedPid(std::int32_t pid, std::uint64_t startTimeTicks);

    /// What the process probe can report, so series it never fills are not drawn (#1028, #1035).
    /// Set once by ShellLayer at attach; the default (all false) hides those optional series.
    void setProcessCapabilities(const Platform::ProcessCapabilities& capabilities);

    /// The shared crash cache the Overview's Recent crashes line reads (#1675), from the composition root
    /// (ShellLayer); null (the default) hides the line.
    void setCrashHistory(std::shared_ptr<Domain::CrashHistory> crashHistory) noexcept
    {
        m_CrashHistoryView.setHistory(std::move(crashHistory));
    }

    /// Whether the machine has an NPU or compute accelerator: the NPU tab is shown beside the GPU tab
    /// only then, on the machine view's NPU tab's condition (GpuSection::showAcceleratorTab(), #1869).
    /// Set each frame by ShellLayer; false (the default) hides it.
    void setHasAccelerator(bool hasAccelerator) noexcept
    {
        m_HasAccelerator = hasAccelerator;
    }

    /// Brings @p tab forward the next time the tabs are drawn: the test hook's TASKSMACK_DETAILS_TAB (#1559).
    void requestTab(SelectOverride::DetailsTab tab)
    {
        m_RequestedTab = tab;
    }

    /// Draws @p section of the Overview open the next time the Overview is drawn, through ImGui's own
    /// open state, as a click on its header does (TASKSMACK_DEMO's section=, #2827). Kept until then; a
    /// section this platform hides, or (Services) this process has none of, is left alone. Open files'
    /// scan is started with it, as its Scan button does, since the section lists nothing until then.
    void requestSection(SelectOverride::DetailsSection section)
    {
        m_RequestedSection = section;
    }

    /// Get currently displayed PID.
    [[nodiscard]] std::int32_t selectedPid() const
    {
        return m_SelectedPid;
    }

    /// Whether the pane shows a running process: one is selected, a sample of it has arrived, and it
    /// has not exited. False while it shows the empty, "Process not found" or "Process exited" state.
    [[nodiscard]] bool showsRunningProcess() const noexcept
    {
        return m_SelectedPid != -1 && m_HasSnapshot && !m_ProcessExited;
    }

    /// Whether the selected process has left the process list since it was sampled, so the pane shows
    /// "Process exited" (#927) until another process is selected.
    [[nodiscard]] bool selectedProcessExited() const noexcept
    {
        return m_ProcessExited;
    }

    /// What process actions the underlying IProcessActions supports, read at construction and again
    /// every renderContent() (some, like the Realtime I/O class, follow the privileged helper, #2407).
    /// Exposed publicly so tests constructing this panel with an injected mock can assert the
    /// capability flags were actually pulled from it.
    [[nodiscard]] const Platform::ProcessActionCapabilities& actionCapabilities() const
    {
        return m_ActionCapabilities;
    }

    /// F9 (#170): on this frame's render, opens the Actions block's Kill confirm for the process shown,
    /// through ProcessActionsView::requestKillShortcut() (target captured then, never a direct kill),
    /// and brings the Overview tab, which holds that block (#1493), forward. Does nothing while this pane is not
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

    /// The service list the Overview's Services section takes display names and states from (#1880), from
    /// the composition root (ShellLayer: the Services panel's model). Null (the default): names alone.
    void setServiceModel(std::shared_ptr<const Domain::ServiceModel> serviceModel) noexcept
    {
        m_ServiceModel = std::move(serviceModel);
    }

    /// Whether the Overview's Services section was drawn open on the last frame, as of the last
    /// updateWithSamples() (#1880): ShellLayer then keeps the service list sampled.
    [[nodiscard]] bool servicesSectionOpen() const noexcept
    {
        return m_ServicesSectionOpen;
    }

    /// Test seam (#1886): events the panel raises (Show in Services) go to @p raise instead of
    /// Core::Application.
    void setEventSinkForTesting(std::function<void(Core::Event&)> raise)
    {
        m_RaiseEvent = std::move(raise);
    }

    /// Test seam (#2117): the Actions block's view, with the dump's state (ProcessActionsView::dumpView()).
    [[nodiscard]] ProcessActionsView& actionsViewForTesting() noexcept
    {
        return m_ActionsView;
    }

    /// Test seam (#2407): the Priority card's view, with the I/O priority control's state.
    [[nodiscard]] ProcessPriorityView& priorityViewForTesting() noexcept
    {
        return m_PriorityView;
    }

  private:
    /// The pane renderContent() draws before the dialogs: the empty or exited state, or the tabs. Takes
    /// F9's request (@p killRequested) only once the tabs are drawn.
    void renderPane(bool killRequested);
    void renderBasicInfo(const Domain::ProcessSnapshot& proc);
    /// The Overview's collapsed Services section (#1886): each service the process runs, with a button
    /// that shows it in the Services tab. Not drawn for a process that runs none.
    void renderServicesSection(const Domain::ProcessSnapshot& proc);
    /// What the chart tabs draw this frame: the history, the smoothed now-bar values and the displayed
    /// snapshot, by pointer for this frame's calls only (ProcessDetailsCharts, #1179).
    [[nodiscard]] ProcessChartContext chartContext() const;
    /// The selected process as an action target: its PID and, once a snapshot has confirmed it,
    /// its start time, so a reuse of the PID is refused rather than acted on (#973).
    [[nodiscard]] Platform::ProcessTarget selectedTarget() const;
    /// updateWithSamples()'s on-demand section reads (Environment, Connections, Modules, Threads,
    /// Security, Open files, I/O priority, Recent crashes), each started only when due.
    void updateOnDemandReads(float deltaTime);

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
    std::uint64_t m_SelectedStartTicks = 0; // With m_SelectedPid, the selected process's exact identity (#1503)
    Detail::SampleIntake m_SampleIntake;    // Where history recording is in the watched process's samples (#1098)
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
        std::string state;    ///< The State with its Status: "Sleeping · Suspended" (#1878)
        std::string affinity; ///< The CPUs it may run on, as the Affinity column writes them (#1878)
    } m_BasicInfoText;
    // The Overview's state for the shared chart-height rule (#959); the Network and GPU tabs' are
    // m_Charts'.
    UI::Widgets::PlotFillState m_OverviewFill;

    std::function<void(Core::Event&)> m_RaiseEvent; // Set by the test seam only
    // The Services section's display names and states (#1880), and whether it was drawn open: set by
    // renderServicesSection(), taken into m_ServicesSectionOpen by updateWithSamples().
    std::shared_ptr<const Domain::ServiceModel> m_ServiceModel;
    bool m_ServicesDrawnOpen = false;
    bool m_ServicesSectionOpen = false;
    bool m_HasSnapshot = false;
    bool m_ProcessExited = false; // Had a snapshot of the selected process, and it has gone missing (#927)

    // Process actions
    std::unique_ptr<Platform::IProcessActions> m_ProcessActions;
    Platform::ProcessActionCapabilities m_ActionCapabilities;
    Platform::ProcessCapabilities m_ProcessCapabilities;
    bool m_HasAccelerator = false;    // Shows the NPU tab (setHasAccelerator(), #1869)
    std::size_t m_SelectionCount = 0; // Rows selected in the Processes table (setSelectionCount(), #1926)
    // tabLabel()'s "name (PID n)" while several processes are selected (#1926): rebuilt in place on each
    // call, its capacity kept, so a steady label allocates nothing.
    mutable std::string m_TabLabelWithPid;

    // The Overview's Actions block (#1493): the buttons, confirm dialog and result line, and the
    // priority control beside them (#1179). Both act through m_ProcessActions, which the panel keeps
    // owning.
    ProcessActionsView m_ActionsView;
    KeyboardShortcuts::FrameRequest m_KillShortcut; // F9, taken by renderContent() this frame
    // The tab to bring forward when the tabs are next drawn: the Overview when F9 asked for the Kill
    // confirm, which shows over the Actions block there; or the test hook's tab (#1559).
    std::optional<SelectOverride::DetailsTab> m_RequestedTab;
    // The Overview section to draw open when the Overview is next drawn (requestSection(), #2827).
    std::optional<SelectOverride::DetailsSection> m_RequestedSection;
    ProcessPriorityView m_PriorityView;
    // The Actions and Priority cards' content heights as last drawn (0 until they have been): the row
    // they are on takes the tallest card's height (ProcessDetailsLayout::computeRowCardHeight(), #1789).
    float m_ActionsCardHeight = 0.0F;
    float m_PriorityCardHeight = 0.0F;

    // The Overview's Environment section (#179). The reader is created by the composition root (the
    // default constructor) and called only from updateWithSamples(), through the view, while the
    // section is open: never from render(), and never for any process but the selected one.
    std::unique_ptr<Platform::IProcessEnvironmentReader> m_EnvironmentReader;
    bool m_HasEnvironment = false; // m_EnvironmentReader can read environments here (Linux)
    ProcessEnvironmentView m_EnvironmentView;

    // The Overview's Connections section (#799), on the same terms as the Environment section: the
    // reader comes from the composition root and is called only from updateWithSamples(), through the
    // view, while the section is open, and only for the selected process. The view runs each read on a
    // worker and its destructor waits for one in flight, so the reader is declared before the view:
    // members are destroyed in reverse order, and the reader must outlive any read using it.
    std::unique_ptr<Platform::IProcessConnectionsReader> m_ConnectionsReader;
    bool m_HasConnections = false; // m_ConnectionsReader can list sockets here (Linux)
    ProcessConnectionsView m_ConnectionsView;

    // The Overview's Modules section (#802), on the same terms as the Connections section, and for the
    // same reason its reader is declared before its view.
    std::unique_ptr<Platform::IProcessModulesReader> m_ModulesReader;
    bool m_HasModules = false; // m_ModulesReader can list modules here (Windows, Linux; not synthetic runs)
    ProcessModulesView m_ModulesView;
    std::unique_ptr<Platform::IProcessThreadsReader> m_ThreadsReader; // the Threads section's (#3069)
    bool m_HasThreads = false; // m_ThreadsReader can list threads here (Linux; not synthetic runs, nor Windows yet)
    ProcessThreadsView m_ThreadsView{static_cast<std::size_t>(std::thread::hardware_concurrency())};

    // The Overview's Security section (#1526), on the same terms, its reader declared before its view.
    std::unique_ptr<Platform::IProcessSecurityReader> m_SecurityReader;
    bool m_HasSecurity = false; // m_SecurityReader can read security contexts here (Linux; not Windows yet, not synthetic runs)
    ProcessSecurityView m_SecurityView;

    // The Overview's Open files section (#183), on the same terms; its reader declared before its view.
    std::unique_ptr<Platform::IProcessOpenFilesReader> m_OpenFilesReader;
    bool m_HasOpenFiles = false; // m_OpenFilesReader can list open files here (Windows, Linux; not synthetic runs)
    ProcessOpenFilesView m_OpenFilesView;

    // The Overview's Recent crashes line (#1675): the selected executable's crashes and hangs from the
    // shared Domain::CrashHistory, read on a worker at most once a refresh interval while the line is
    // drawn, never per frame or per selection change. Its destructor waits for a read in flight, which
    // holds its own reference to the history.
    ProcessCrashHistoryView m_CrashHistoryView;

    // The smoothed NowBar values, eased toward each shown sample (#1179).
    Detail::ProcessSmoothedUsage m_SmoothedUsage;

    // The Overview's charts and the GPU, NPU and Network and I/O tabs (#1179, #1869). They read m_History,
    // m_SmoothedUsage and the snapshot through chartContext(), passed in each frame.
    ProcessDetailsCharts m_Charts;
};

} // namespace App
