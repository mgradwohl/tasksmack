#pragma once

#include "App/Panel.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessTreeFlatten.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"

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

    /// Render the panel (with ImGui window wrapper).
    /// @param open Pointer to visibility flag (for window close button).
    void render(bool* open) override;

    /// Render content only (for embedding in tab, without window wrapper).
    void renderContent() override;
    /// Handle application events (theme/font changes)
    void onEvent(Core::Event& event) override;

    /// Get the currently selected process PID.
    /// @return Selected PID, or -1 if none selected.
    [[nodiscard]] std::int32_t selectedPid() const
    {
        return m_SelectedPid;
    }

    /// Get the process count.
    [[nodiscard]] size_t processCount() const;

    /// Find a single snapshot by PID without copying the full snapshot vector.
    /// Always reflects the latest published data; returns std::nullopt if not found.
    [[nodiscard]] std::optional<Domain::ProcessSnapshot> findSnapshot(std::int32_t pid) const;

    /// Have the process model keep a sample of process @p pid from every generation it publishes
    /// (Domain::ProcessModel::watchProcess()). pid <= 0 stops watching.
    void watchProcess(std::int32_t pid);

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

    /// Narrowest the toolbar row (filter, clear button, process count, tree-view toggle) can be
    /// without overlapping, at the current font and style, for the window's content minimum (#1207).
    /// Measured with a worst-case process count so it does not change as processes come and go.
    /// Needs a frame.
    [[nodiscard]] static float measureToolbarMinimumWidth();

    /// What the process probe can report (all false without a model), as of the latest snapshot
    /// generation this panel has fetched: a probe can withdraw a capability after the first sample
    /// (#1254). UI thread; takes no lock once a generation is cached.
    [[nodiscard]] Platform::ProcessCapabilities processCapabilities() const;

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

    std::int32_t m_SelectedPid = -1;
    std::uint64_t m_SelectedUniqueKey = 0; // Selected process's identity: a PID can be reused

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
    // The probe's capabilities published with m_CachedRenderSnapshots' generation (#1254).
    Platform::ProcessCapabilities m_CachedCapabilities;

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

        // Unit slot widths of the decimal-aligned columns (#1201): the widest unit each can show,
        // measured from the strings the cells print (renderUnitAlignedText())
        float unitBytesWidth = 0.0F;       // " MiB", " GiB", etc.
        float unitBytesPerSecWidth = 0.0F; // " MiB/s", " GiB/s", etc.
        float unitPowerWidth = 0.0F;       // " W", " mW", " µW"

        // Static label widths
        float treeViewLabelWidth = 0.0F;
        float listViewLabelWidth = 0.0F;

        // Widths for PRIORITY_LABELS (Domain::Priority::getProcessPriorityLabel()'s fixed label set).
        // That column isn't backed by RowFormatCache (it's a live std::string_view lookup, not
        // a per-row formatted string), so its width can't ride along with RowFormatCache's
        // per-row AlignedCellText widths -- cached here instead, alongside this panel's other
        // small fixed-string-set widths.
        std::array<float, PRIORITY_LABELS.size()> priorityLabelWidths{};
        float widestPriorityLabelWidth = 0.0F; // The Priority column's default width fits it (#1280)

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

        /// Get the cached width of one of Domain::Priority::getProcessPriorityLabel()'s fixed labels.
        /// Returns 0 for any other string (getProcessPriorityLabel never returns anything else).
        [[nodiscard]] float getPriorityLabelWidth(std::string_view label) const noexcept;
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

    /// Get the number of visible columns
    [[nodiscard]] int visibleColumnCount() const;

    /// Render process rows in tree view mode. Takes the filtered/expanded tree in render order
    /// from m_TreeRowsCache, which flattens it (ProcessTreeFlatten::buildProcessTreeRows(), a pure,
    /// separately-tested traversal) only when its inputs change (#1138), then applies
    /// ImGuiListClipper to that flat list so only visible rows reach the expensive part,
    /// renderProcessRow() -- see perf-plan #843's tree-view virtualization item.
    /// @param snapshots The full list of process snapshots.
    /// @param filteredIndices Indices into snapshots for processes matching the current filter.
    void renderTreeView(const std::vector<Domain::ProcessSnapshot>& snapshots, const std::vector<std::size_t>& filteredIndices);

    /// Render a single process row
    /// @param proc The process to render.
    /// @param depth Indentation depth in the tree.
    /// @param hasChildren Whether the process has children.
    /// @param isExpanded Whether the children are visible.
    void renderProcessRow(const Domain::ProcessSnapshot& proc, int depth, bool hasChildren, bool isExpanded);
};

} // namespace App
