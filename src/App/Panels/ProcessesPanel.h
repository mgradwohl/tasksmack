#pragma once

#include "App/KeyboardShortcuts.h"
#include "App/Panel.h"
#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessBatchPriorityDialog.h"
#include "App/Panels/ProcessColumnAvailability.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDisplayFreeze.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessRowMeterView.h"
#include "App/Panels/ProcessSelection.h"
#include "App/Panels/ProcessTableNavigation.h"
#include "App/Panels/ProcessTreeFlatten.h"
#include "App/ProcessColumnConfig.h"
#include "App/SelectOverride.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/ProcessTypes.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ImFont; // Forward declaration for TextSizeCache

namespace Domain
{
class GPUModel;
} // namespace Domain

namespace App
{

/// Pulled in from ProcessRowFormat.h so existing bare references throughout this header and
/// ProcessesPanel.cpp keep compiling unchanged after the row-formatting logic moved to a
/// standalone, ImGui-free, unit-testable header (see ProcessRowFormat.h's doc comment).
using ProcessRowFormat::AlignedCellText;
using ProcessRowFormat::RowFormatCache;

/// Domain::Priority::getProcessPriorityLabel()'s complete fixed set of possible return values, taken
/// from Domain rather than duplicated here -- a hand-duplicated copy would silently drift (and make
/// getPriorityLabelWidth() fall back to a wrong width of 0, misplacing the cell) if Domain ever
/// renamed a label. Namespace-scope (not nested in ProcessesPanel) for the same reason as
/// AlignedCellText: both ProcessesPanel::TextSizeCache (header) and the free helper functions in
/// ProcessesPanel.cpp's anonymous namespace need to see it, and it must be visible wherever
/// TextSizeCache::priorityLabelWidths is sized.
inline constexpr auto PRIORITY_LABELS = Domain::Priority::PROCESS_PRIORITY_LABELS;

/// The font-measured widths the Processes table's per-column cell renderers read (#1382): each
/// decimal-aligned column's unit slot, the widest unit it can show as its cells print it (#1201),
/// and PRIORITY_LABELS' widths. Measured by ProcessesPanel::TextSizeCache::populate().
/// Namespace-scope, like PRIORITY_LABELS, so the renderers in ProcessesPanel.cpp's anonymous
/// namespace can take it.
struct ProcessCellWidths
{
    float unitBytes = 0.0F;       // " MiB", " GiB", etc.
    float unitBytesPerSec = 0.0F; // " MiB/s", " GiB/s", etc.
    float unitPower = 0.0F;       // " W", " mW", " µW"
    float unavailableText = 0.0F; // ProcessRowFormat::UNAVAILABLE_CELL_TEXT, for free-text cells (#1210)

    // Widths for PRIORITY_LABELS (Domain::Priority::getProcessPriorityLabel()'s fixed label set), in the
    // same order. That column isn't backed by RowFormatCache (it's a live std::string_view lookup,
    // not a per-row formatted string), so its width can't ride along with RowFormatCache's per-row
    // AlignedCellText widths -- cached here instead, alongside the other small fixed-string widths.
    std::array<float, PRIORITY_LABELS.size()> priorityLabels{};
    float widestPriorityLabel = 0.0F; // The Priority column's default width fits it (#1280)

    /// The cached width of one of Domain::Priority::getProcessPriorityLabel()'s fixed labels. Returns 0
    /// for any other string (getProcessPriorityLabel never returns anything else).
    [[nodiscard]] float priorityLabelWidth(std::string_view label) const noexcept
    {
        for (std::size_t i = 0; i < PRIORITY_LABELS.size(); ++i)
        {
            if (PRIORITY_LABELS[i] == label)
            {
                return priorityLabels[i];
            }
        }
        return 0.0F; // Unreachable in practice: getProcessPriorityLabel() only returns PRIORITY_LABELS entries.
    }
};

/// Panel for displaying and managing the process list.
/// Refresh cadence is driven by the main loop via onUpdate().
class ProcessesPanel : public Panel
{
  public:
    ProcessesPanel();
    ~ProcessesPanel() override;

    ProcessesPanel(const ProcessesPanel&) = delete;
    ProcessesPanel& operator=(const ProcessesPanel&) = delete;
    ProcessesPanel(ProcessesPanel&&) = delete;
    ProcessesPanel& operator=(ProcessesPanel&&) = delete;

    /// Initialize the panel (creates ProcessModel and starts sampler).
    void onAttach() override;

    /// Cleanup (stops sampler).
    void onDetach() override;

    /// Update logic (no longer needed for refresh, kept for interface compatibility).
    void onUpdate(float deltaTime) override;

    /// Render content only (for embedding in tab, without window wrapper).
    void renderContent() override;
    /// Handle application events (theme/font changes)
    void onEvent(Core::Event& event) override;

    /// The primary selected process's PID: the row last clicked or moved to, which Process Details
    /// shows. With several rows selected (#804) it is one of them; selectionCount() counts them all.
    /// @return Selected PID, or -1 if none selected.
    [[nodiscard]] std::int32_t selectedPid() const
    {
        return m_SelectedPid;
    }

    /// How many processes are selected in the table (#804): 0, 1, or more after a Ctrl/Shift+click or
    /// Ctrl+A. Selected processes that exit drop out.
    [[nodiscard]] std::size_t selectionCount() const noexcept
    {
        return m_Selection.size();
    }

    /// Get the process count.
    [[nodiscard]] size_t processCount() const;

    /// The process count of the snapshot generation this panel last adopted (in onUpdate()), for a
    /// per-frame reader such as the status bar (#1200): the adopted vector is immutable and owned
    /// here, so this takes no lock, unlike processCount(), which locks the model's mutex.
    [[nodiscard]] std::size_t adoptedProcessCount() const noexcept
    {
        return m_CachedRenderSnapshots->size();
    }

    /// Find a single snapshot by PID without copying the full snapshot vector.
    /// Always reflects the latest published data; returns std::nullopt if not found.
    [[nodiscard]] std::optional<Domain::ProcessSnapshot> findSnapshot(std::int32_t pid) const;

    /// Have the process model keep a sample of process @p pid from every generation it publishes
    /// (Domain::ProcessModel::watchProcess()). pid <= 0 stops watching.
    void watchProcess(std::int32_t pid);

    /// The test hook's startup selection (#1559, App/SelectOverride.h): the first snapshot that holds
    /// @p target's process selects it like a click and, with @p showDetails, opens Process Details.
    /// Set once by ShellLayer.
    void requestStartupSelection(std::optional<SelectOverride::Target> target, bool showDetails);

    /// The Process Details tab the startup selection asks for, once, after it fired; never when it
    /// gave up (TASKSMACK_DETAILS_TAB, #1559).
    [[nodiscard]] std::optional<SelectOverride::DetailsTab> takeStartupDetailsTab() noexcept
    {
        return m_StartupSelection.takeFiredTab();
    }

    /// The watched process's samples newer than @p lastSeenVersion, oldest first, appended to
    /// @p outSamples; see Domain::ProcessModel::watchedSamplesSince(). Like findSnapshot(), this
    /// bypasses the render cache, so it follows every publish whichever tab is showing.
    [[nodiscard]] bool watchedSamplesSince(std::uint64_t lastSeenVersion, std::vector<Domain::ProcessSample>& outSamples) const;

    /// Get column settings (for persistence)
    [[nodiscard]] const ProcessColumnSettings& columnSettings() const
    {
        return m_ColumnSettings;
    }

    /// Set column settings (from loaded config)
    void setColumnSettings(const ProcessColumnSettings& settings)
    {
        m_ColumnSettings = settings;
        m_ColumnSettings.keepUnhideableColumnsVisible(); // PID and Name cannot be hidden (#1209)
    }

    /// Set the refresh interval (applied by onUpdate cadence checks).
    void setSamplingInterval(std::chrono::milliseconds interval, bool forceSample = true);

    /// Request an immediate refresh.
    void requestRefresh();

    /// Hands a saved column layout (widths, order, sort) back to ImGui. Call before the table is
    /// first rendered; the text is filtered first, so anything that is not a table layout is
    /// ignored. See ProcessTableSettings.h (#952).
    static void restoreTableLayout(std::string_view stored);

    /// The table's current column layout as text for the config, or empty if the table has not been
    /// rendered this session (in which case the caller should keep whatever it already has).
    [[nodiscard]] std::string captureTableLayout() const;

    /// The process model this panel owns, null before onAttach() and after onDetach(). Other panels
    /// should keep it as a weak_ptr, so they never outlive it (#1176).
    [[nodiscard]] std::shared_ptr<Domain::ProcessModel> processModel() const
    {
        return m_ProcessModel;
    }

    /// Returns true if the process probe reports reduced privileges, as of the latest snapshot
    /// generation this panel has fetched (#1254). Convenience accessor so ShellLayer does not need to
    /// include Domain/ProcessModel.h. UI thread; takes no lock once a generation is cached.
    [[nodiscard]] bool hasReducedPrivileges() const;

    /// Narrowest the toolbar row (filter, clear button, the paused indicator's icon-only form (#928),
    /// process count or a row action's result, the Columns button and the List | Tree control) can be
    /// without overlapping, at the current font and style, for the window's content minimum (#1207).
    /// Measured with a worst-case process count so it does not change as processes come and go.
    /// Needs a frame.
    [[nodiscard]] static float measureToolbarMinimumWidth();

    /// What the process probe can report (all false without a model), as of the latest snapshot
    /// generation this panel has fetched: a probe can withdraw a capability after the first sample
    /// (#1254). UI thread; takes no lock once a generation is cached.
    [[nodiscard]] Platform::ProcessCapabilities processCapabilities() const;

    /// The GPU model whose probe decides whether per-process GPU usage can be observed (#1210). Set
    /// by ShellLayer, which shares it with the process model; kept weakly.
    void setGpuModel(const std::shared_ptr<const Domain::GPUModel>& gpuModel);

    /// Whether per-process GPU usage can be observed on this system: a GPU model is set and its probe
    /// has not been found to lack per-process metrics (ProcessColumnAvailability::perProcessGpuSupported()).
    [[nodiscard]] bool hasPerProcessGpuMetrics() const;

    /// What per-process GPU data can be observed: metrics at all, and utilization among them (#1210).
    [[nodiscard]] ProcessColumnAvailability::GpuSupport gpuSupport() const;

    /// Whether the table shows the process tree rather than the flat list.
    [[nodiscard]] bool treeViewEnabled() const noexcept
    {
        return m_TreeViewEnabled;
    }

    /// Switches between list and tree view, as the toolbar's List | Tree control does (F5, #170).
    void toggleTreeView()
    {
        setTreeView(!m_TreeViewEnabled);
    }

    /// F9 (#170): on this frame's render, ask to kill the selected process through the same confirm
    /// dialog as the row menu's Kill, its target captured then. Nothing happens when no process is
    /// selected, the selection is not a visible row, or the platform cannot kill. With several rows
    /// selected (#804) it asks to kill all of them, in the batch confirm. Never kills directly.
    /// Lives for this frame only: expireFrameRequests() drops it if the table was not drawn.
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
    // shared_ptr (not unique_ptr): BackgroundSampler observes this model via a weak_ptr rather
    // than a raw pointer, so the sampler thread can never outlive-dereference it regardless of
    // destructor ordering.
    std::shared_ptr<Domain::ProcessModel> m_ProcessModel;
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler;
    // ImGui's ID for the process table, recorded when it is rendered; 0 until then. Needed to pick
    // this table's section out of ImGui's settings text (#952).
    std::uint32_t m_TableId = 0;

    // The table's layout as captured on last entering tree view. Its sort is restored onto any
    // later capture that has none, which tree view causes and which persists after leaving it.
    // Empty until tree view is first entered.
    std::string m_SortBackupLayout;

    // The primary process: last clicked or moved to, shown by Process Details.
    std::int32_t m_SelectedPid = -1;
    // With m_SelectedPid, the primary process's exact identity: a PID can be reused, and the uniqueKey
    // hash could collide (#1503).
    std::uint64_t m_SelectedStartTicks = 0;
    // Every selected row, by PID and start time (#804, #1503): the rows drawn highlighted, and what a
    // batch action is for. A plain click or keyboard move makes it the primary alone.
    ProcessSelection::Selection m_Selection;
    // A row click, applied after the rows are drawn (#804): a Shift+click's range needs the visible
    // order, which must not be rebuilt while the tree rows are iterated.
    struct PendingClick
    {
        ProcessSelection::Identity id;
        ProcessSelection::ClickKind kind = ProcessSelection::ClickKind::Replace;
    };
    std::optional<PendingClick> m_PendingClick;
    // TaskSmack's own PID (Platform::currentProcessId(), at attach): a batch naming it says so and
    // acts on it last (#804).
    std::int32_t m_OwnPid = 0;

    std::chrono::milliseconds m_RefreshInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    std::chrono::milliseconds m_AppliedSamplerInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    bool m_ForceRefresh = false;
    bool m_IsActiveTab = false;
    // Whether the active main tab shows any process data, which sets the sampling rate (#1097). True
    // at start: the default System tab shows process-derived charts.
    bool m_ProcessDataShown = true;
    float m_InteractionHoldSeconds = 0.0F;

    // Column visibility
    ProcessColumnSettings m_ColumnSettings;
    // Visibility asked for in the toolbar's Columns menu (#1209), handed to ImGui inside the table on
    // the same frame; the menu shows it until then.
    std::optional<ProcessColumnSettings> m_RequestedColumns;
    bool m_ResetColumnOrderRequested = false; // "Reset columns" also restores the default order
    bool m_TableHasDefaultOrder = true;       // As of the last frame, for enabling "Reset columns"
    // The capabilities the columns' defaults were last applied for: when the probe's change, the
    // columns whose visibility was not chosen follow them (#1210).
    Platform::ProcessCapabilities m_ColumnDefaultsCapabilities;
    ProcessColumnAvailability::GpuSupport m_ColumnDefaultsGpuSupport; // gpuSupport() when they were last applied
    // gpuSupport(), read once a frame for the GPU columns (#1210)
    ProcessColumnAvailability::GpuSupport m_GpuSupport;
    std::weak_ptr<const Domain::GPUModel> m_GpuModel;
    // Whether the table has been drawn with m_ColumnSettings: from then on, a column ImGui shows or
    // hides differently is the user's toggle in its header menu; before, it is a restored layout.
    bool m_TableShowsColumnSettings = false;

    // Inline meters (#1528): which columns draw one (Columns > Show meter, saved in config.toml), the
    // absolute columns' maxima for the adopted generation (rebuilt once per generation, not per frame),
    // and this frame's heat colours. m_MetersShown is false on a frame with no meter on, so the rows
    // skip the per-cell check entirely.
    ProcessRowMeter::Settings m_MeterSettings;
    ProcessRowMeter::ColumnMaxima m_MeterMaxima;
    std::uint64_t m_MeterMaximaVersion = std::numeric_limits<std::uint64_t>::max();
    ProcessRowMeter::Colors m_MeterColors;
    bool m_MetersShown = false;

    // Tree view gives the Name column room (#1209): adjusted once when the view mode changes. The
    // width it had before, and the width tree view set (0 when it left it alone), so leaving tree
    // view can restore a width the user did not change in the meantime.
    bool m_NameWidthSyncPending = false;
    float m_NameWidthBeforeTree = 0.0F;
    float m_NameWidthSetForTree = 0.0F;

    // Process actions from the row menu (#1209), confirmed in the same dialog as Process Details'
    // Actions block. Created at attach, like that panel's: this panel is part of the composition root.
    std::unique_ptr<Platform::IProcessActions> m_ProcessActions;
    Platform::ProcessActionCapabilities m_ActionCapabilities;
    // The action awaiting confirmation: one target from a row (or F9), several from the selection
    // (#804). Each target is a PID and start time, so a reused PID is refused (#973). The dialog's
    // text is built once, here, when the action is requested. A batch priority change (#1484) is
    // confirmed in its own dialog (m_BatchPriorityDialog, #1539), not here.
    struct RowAction
    {
        Detail::ProcessAction action = Detail::ProcessAction::None;
        std::vector<ProcessBatch::BatchTarget> targets;
        std::string title;
        std::string question;

        /// Whether an action is awaiting confirmation.
        [[nodiscard]] bool pending() const noexcept
        {
            return action != Detail::ProcessAction::None;
        }
    } m_RowAction;
    bool m_ShowRowActionConfirm = false;
    // The row menu's "Set priority for N processes..." (#1484): picks the value, then the batch
    // confirmation above sets it.
    ProcessBatchPriorityDialog m_BatchPriorityDialog;
    Detail::ActionResultMessage m_RowActionResult; // Shown in the toolbar for a few seconds
    float m_RowActionResultSeconds = 0.0F;
    // The process the row menu was opened on, copied on the same right-press that selected it: the
    // table re-sorts every sample, so by the button's release another row can be under the pointer
    // and the menu would act on a process other than the highlighted one (#1365).
    std::optional<Domain::ProcessSnapshot> m_RowMenuTarget;
    unsigned int m_RowMenuPopupId = 0; // ImGuiID of ROW_MENU_POPUP_ID at the panel's ID stack

    // Keyboard navigation (#160) and F9 (#170). A move scrolls the newly selected row into view on the
    // frame it is drawn; the clipper is told to draw that row even when it is off-screen.
    KeyboardShortcuts::FrameRequest m_KillShortcut;
    bool m_ScrollSelectedIntoView = false;
    std::size_t m_ScrollTargetRow = 0; // Index of the selected row in the visible order

    // Search/filter state - using std::string for dynamic sizing
    std::string m_SearchBuffer;

    // Tree view state
    bool m_TreeViewEnabled = false;
    // m_CachedSortedIndices is in natural order and the list view must re-sort it on its next frame,
    // even though ImGui's SpecsDirty is not set (e.g. the rows were reset while in tree view) (#1174).
    bool m_SortPending = true;

    // Previous frame's table layout, feeding ProcessTableLayout::computeInnerWidth() (#924)
    float m_OtherColumnsWidth = 0.0F;                  // Everything but the Command column's own content
    float m_TableVisibleWidth = 0.0F;                  // Visible width of the table's scrolling area
    std::unordered_set<std::uint64_t> m_CollapsedKeys; // uniqueKeys that are collapsed in tree view
    std::uint64_t m_CollapseGeneration = 0;            // Advanced whenever m_CollapsedKeys changes (#1138)

    // Snapshot cache: only re-fetch from ProcessModel when version changes (data updates at 1Hz,
    // but render runs at 60fps). A shared_ptr to ProcessModel's immutable published vector, not
    // an owned copy: tryCopySnapshotsIfNewer() hands out the same vector every reader shares,
    // so re-fetching is an O(1) refcount bump rather than a deep copy of 50-100+
    // ProcessSnapshot objects (see #843 Phase 3b). Default-constructed to an empty (never null)
    // vector so callers can dereference it before the first successful fetch.
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> m_CachedRenderSnapshots =
        std::make_shared<const std::vector<Domain::ProcessSnapshot>>();
    std::uint64_t m_CachedSnapshotVersion = std::numeric_limits<std::uint64_t>::max();
    SelectOverride::Pending m_StartupSelection; // TASKSMACK_SELECT_PID/_NAME (#1559); idle once fired
    bool m_StartupSelectionShowsDetails = true; // false when TASKSMACK_TAB picks the tab instead
    // The probe's capabilities published with m_CachedRenderSnapshots' generation (#1254).
    Platform::ProcessCapabilities m_CachedCapabilities;
    // The GPU support m_CachedRenderSnapshots' GPU fields were read under (#1210), copied with them:
    // the cells are formatted by it, not by the GPU model's current state (m_GpuSupport), which can
    // differ while GPU merges are throttled. Column defaults and headers follow m_GpuSupport.
    Domain::ProcessModel::GpuSupport m_CachedGpuSupport;
    // Hold Ctrl to freeze the pane (#928): while frozen, adoptNewerSnapshots() keeps the generation
    // above, so the rows, their values and their order stay put. Evaluated once per frame in
    // renderContent(); onUpdate() sees the previous frame's state.
    ProcessDisplayFreeze::Tracker m_DisplayFreeze;

    // Per-frame filter cache: filtered indices, running count, and summary string are rebuilt
    // only when the snapshot version or search term changes (O(1) skip in 59/60 frames).
    // m_CachedFilteredIndices is always in natural (snapshot) order; list view uses
    // m_CachedSortedIndices so tree view never sees a sorted ordering.
    std::vector<std::size_t> m_CachedFilteredIndices;
    std::vector<std::size_t> m_CachedSortedIndices;
    std::size_t m_CachedRunningCount = 0;
    std::uint64_t m_CachedFilterVersion = std::numeric_limits<std::uint64_t>::max();
    std::string m_CachedSearchTerm;
    std::string m_CachedSummaryStr;
    std::uint64_t m_FilterGeneration = 0; // Advanced whenever m_CachedFilteredIndices is rebuilt (#1138)

    // The tree view's flattened rows, rebuilt only when the snapshot version, m_FilterGeneration or
    // m_CollapseGeneration changes rather than every frame (#1138).
    ProcessTreeFlatten::ProcessTreeRowsCache m_TreeRowsCache;

    /// Cache for text size measurements to avoid repeated ImGui::CalcTextSize calls.
    /// Invalidated when the font changes: a different ImFont pointer, or a rebuilt font atlas
    /// (UI::Theme::fontGeneration()), which can reuse the old pointer (#943).
    struct TextSizeCache
    {
        // Column header widths (indexed by ProcessColumn enum)
        std::array<float, processColumnCount()> columnHeaderWidths{};

        // The unit slots and priority label widths the table's cell renderers read (#1201, #1382)
        ProcessCellWidths cells;

        // Static label widths
        float treeViewLabelWidth = 0.0F;
        float listViewLabelWidth = 0.0F;
        float columnsLabelWidth = 0.0F;
        float caretRightWidth = 0.0F; // Tree expanders (#1209)
        float caretDownWidth = 0.0F;

        // Font pointer and font-atlas generation used when cache was populated (for invalidation)
        const ImFont* fontPtr = nullptr;
        std::uint64_t fontGeneration = 0;

        // Changes on every populate(): the identity RowFormatCache entries are stamped with, so a
        // repopulate for any reason rebuilds their widths, even if the font kept its address.
        std::uintptr_t stamp = 0;

        /// Check if cache is valid for current font
        [[nodiscard]] bool isValid() const noexcept;

        /// Populate cache with current font measurements
        void populate();

        /// Get column header width (returns 0 if cache invalid)
        [[nodiscard]] float getHeaderWidth(ProcessColumn col) const noexcept
        {
            return columnHeaderWidths[toIndex(col)];
        }
    };

    TextSizeCache m_TextSizeCache;

    /// Cache of pre-formatted strings per process row, keyed by uniqueKey. Unlike a version-only
    /// cache, each entry is built lazily (only when that specific row is actually rendered) and
    /// stamps its own `generation`/`fontPtr` (see ProcessRowFormat.h's RowFormatCache doc
    /// comment) so renderProcessRow() can detect a stale entry (new snapshot data, or a font/
    /// size/DPI change) and rebuild just that one row on demand -- cost scales with visible
    /// rows, not total process count (perf-plan #843).
    std::unordered_map<std::uint64_t, RowFormatCache> m_RowFormatCache;
    // Generation m_RowFormatCache was last pruned at (entries for processes no longer present
    // are erased once per new snapshot generation, not per frame, to bound the map's size
    // without adding per-frame cost).
    std::uint64_t m_RowFormatCachePrunedVersion = std::numeric_limits<std::uint64_t>::max();

    /// Ensure text size cache is populated for current font
    void ensureTextSizeCacheValid();

    /// Adopts the model's latest snapshot generation and its capabilities into the render cache if
    /// it is newer than the cached one (onAttach(), onUpdate() and renderContent(), #1180). Does
    /// nothing while the pane is frozen by a held Ctrl (#928), except to load the first generation.
    void adoptNewerSnapshots();

    /// Offers the adopted generation to m_StartupSelection; selects its process when it appears.
    void applyStartupSelection();

    /// Feeds this frame's keyboard and window state to m_DisplayFreeze (#928). Must be called inside
    /// the window the pane renders into, since it asks ImGui whether that window is hovered/focused.
    void updateDisplayFreeze();

    /// Render process rows in tree view mode. Takes the filtered/expanded tree in render order
    /// from m_TreeRowsCache, which flattens it (ProcessTreeFlatten::buildProcessTreeRows(), a pure,
    /// separately-tested traversal) only when its inputs change (#1138), then applies
    /// ImGuiListClipper to that flat list so only visible rows reach the expensive part,
    /// renderProcessRow() -- see perf-plan #843's tree-view virtualization item.
    /// @param snapshots The full list of process snapshots.
    /// @param filteredIndices Indices into snapshots for processes matching the current filter.
    void renderTreeView(const std::vector<Domain::ProcessSnapshot>& snapshots, const std::vector<std::size_t>& filteredIndices);

    /// The toolbar's Columns menu: a check per column and "Reset columns" (#1209).
    void renderColumnsMenu();

    /// The Columns menu's "Show meter" submenu (#1528): a check per meter column.
    void renderMeterMenu();

    /// Readies this frame's meters (#1528): the maxima for a newly adopted generation, the theme's
    /// colours. Before the rows are drawn.
    void prepareRowMeters(const std::vector<Domain::ProcessSnapshot>& snapshots);

    /// Hands the Columns menu's requests to ImGui. Inside the table, after its columns are set up and
    /// before the first row. Returns true when column visibility was changed this frame.
    bool applyColumnRequests();

    /// Gives the Name column tree view's width, or back the list's, after a view-mode change (#1209).
    /// Inside the table, after its columns are set up and before the first row.
    void syncNameWidthForViewMode();

    /// Switches between list and tree view.
    void setTreeView(bool enabled);

    /// Applies this frame's navigation key and a pending F9 to the visible rows (#160, #170): moves the
    /// selection, collapses or expands in tree view, or opens the Kill confirm for the selected row.
    /// Inside the table, after sorting and before the rows are drawn.
    /// Ctrl+A (#804) selects every visible row.
    void applyKeyboardInput(const std::vector<Domain::ProcessSnapshot>& snapshots,
                            ProcessTableNavigation::NavCommand command,
                            bool killRequested,
                            bool selectAllRequested);

    /// The visible rows in drawn order, as indices into `snapshots`: the sorted list, or the flattened
    /// tree. With `shapes`, also each tree row's shape (left empty in list view, or if any index had to
    /// be dropped). Not while the tree rows are being iterated: it can rebuild them.
    void collectVisibleRows(const std::vector<Domain::ProcessSnapshot>& snapshots,
                            std::vector<std::size_t>& visible,
                            std::vector<ProcessTableNavigation::TreeRowShape>* shapes);

    /// The identities (PID and start time) of the visible rows, in drawn order.
    [[nodiscard]] std::vector<ProcessSelection::Identity> visibleIdentities(const std::vector<Domain::ProcessSnapshot>& snapshots);

    /// The primary process's identity (m_SelectedPid, m_SelectedStartTicks).
    [[nodiscard]] ProcessSelection::Identity primaryIdentity() const noexcept
    {
        return {.pid = m_SelectedPid, .startTimeTicks = m_SelectedStartTicks};
    }

    /// Applies this frame's row click (m_PendingClick) to the selection, after the rows are drawn.
    void applyPendingClick(const std::vector<Domain::ProcessSnapshot>& snapshots);

    /// Makes `proc` the primary process -- what Process Details shows -- and tells the other panels.
    /// Leaves the multi-selection alone.
    void selectProcess(const Domain::ProcessSnapshot& proc);
    void selectProcess(const ProcessSelection::Identity& id);

    /// The right-click menu of a process row (#1209): Details, Copy, and the actions the platform has.
    void renderRowContextMenu(const Domain::ProcessSnapshot& proc);

    /// Asks to run `action` on `proc`, through the confirmation dialog.
    void requestRowAction(Detail::ProcessAction action, const Domain::ProcessSnapshot& proc);

    /// Asks to run `action` on every selected process still listed, through the batch confirmation
    /// (#804). One process left: the single-process dialog. None: nothing happens.
    void requestSelectionAction(Detail::ProcessAction action);

    /// Opens the batch priority dialog for every selected process still listed (#1484). None left:
    /// nothing happens.
    void requestSelectionPriority();

    /// The batch priority dialog (#1484), which lists the processes and is its own confirmation
    /// (#1539); on Apply, sets the picked value on each of them by identity.
    void renderBatchPriorityDialog();

    /// The confirmation dialog for a row-menu action, and the action once confirmed.
    void renderRowActionConfirm();

    /// Render a single process row
    /// @param proc The process to render.
    /// @param depth Indentation depth in the tree.
    /// @param hasChildren Whether the process has children.
    /// @param isExpanded Whether the children are visible.
    void renderProcessRow(const Domain::ProcessSnapshot& proc, int depth, bool hasChildren, bool isExpanded);

    /// The PID cell: the row's selectable (entered even when the column is scrolled out of view, so
    /// the row stays clickable, #962) and, when `columnVisible`, the right-aligned PID.
    void renderPidCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, bool columnVisible);

    /// The Name cell: the tree indent and expand/collapse control in tree view, then the name (#906).
    void renderNameCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, int depth, bool hasChildren, bool isExpanded);
};

} // namespace App
