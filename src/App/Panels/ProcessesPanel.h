#pragma once

#include "App/Panel.h"
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

/// A pre-formatted right-aligned cell's text plus its CalcTextSize width, measured lazily (on
/// first render, not at RowFormatCache population time) and cached from then on (perf-plan #843
/// phase 1). Populating widths eagerly for every process at cache-rebuild time -- before
/// ImGuiListClipper gets a chance to restrict work to visible rows -- would concentrate
/// thousands of CalcTextSize calls into a single snapshot-update frame on the app's "thousands
/// of processes" scenario, working directly against the frame-budget goal this cache exists to
/// serve. `width` is `mutable` so renderRightAlignedText() can fill it in through a `const
/// AlignedCellText&` the first time this specific cell is actually drawn; every later frame
/// (until the next cache rebuild resets it) reuses the cached value. Bundling text+width in one
/// type still means a caller can't use one without the other being kept in sync. Namespace-scope
/// (not nested in ProcessesPanel) so the free renderRightAlignedText()/makeAlignedCellText()
/// helpers in ProcessesPanel.cpp's anonymous namespace can use it without needing member/friend
/// access to a private nested type.
struct AlignedCellText
{
    /// Sentinel meaning "not measured yet". Real widths are never negative.
    static constexpr float UNMEASURED_WIDTH = -1.0F;

    std::string text;
    mutable float width = UNMEASURED_WIDTH;
};

/// Domain::Priority::getPriorityLabel()'s complete fixed set of possible return values, derived
/// by calling the real function at one representative nice value per threshold bucket instead
/// of duplicating its label strings here -- a hand-duplicated copy would silently drift (and
/// make getPriorityLabelWidth() fall back to a wrong width of 0, misplacing the cell) if Domain
/// ever renamed a label. Namespace-scope (not nested in ProcessesPanel) for the same reason as
/// AlignedCellText: both ProcessesPanel::TextSizeCache (header) and the free helper functions in
/// ProcessesPanel.cpp's anonymous namespace need to see it, and it must be visible wherever
/// TextSizeCache::priorityLabelWidths is sized.
inline constexpr std::array<std::string_view, 5> PRIORITY_LABELS = {
    Domain::Priority::getPriorityLabel(Domain::Priority::MIN_NICE),               // < HIGH_THRESHOLD           -> "High"
    Domain::Priority::getPriorityLabel(Domain::Priority::HIGH_THRESHOLD),         // < ABOVE_NORMAL_THRESHOLD   -> "Above Normal"
    Domain::Priority::getPriorityLabel(Domain::Priority::NORMAL_NICE),            // < BELOW_NORMAL_THRESHOLD   -> "Normal"
    Domain::Priority::getPriorityLabel(Domain::Priority::BELOW_NORMAL_THRESHOLD), // < IDLE_THRESHOLD          -> "Below Normal"
    Domain::Priority::getPriorityLabel(Domain::Priority::MAX_NICE),               // >= IDLE_THRESHOLD          -> "Idle"
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

    /// Same as findSnapshot(), but also returns the exact publication version the snapshot was
    /// read under, atomically. Prefer this over pairing findSnapshot() with a separate
    /// publication-version read (e.g. Domain::ProcessModel::snapshotVersion()) when the caller
    /// needs to gate behavior on "is this new data" -- see
    /// Domain::ProcessModel::findSnapshotWithVersion()'s doc comment.
    [[nodiscard]] std::optional<Domain::ProcessModel::SnapshotLookupResult> findSnapshotWithVersion(std::int32_t pid) const;

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
    void setSamplingInterval(std::chrono::milliseconds interval);

    /// Request an immediate refresh.
    void requestRefresh();

    /// Access the underlying process model (non-owning).
    [[nodiscard]] Domain::ProcessModel* processModel() const
    {
        return m_ProcessModel.get();
    }

    /// Returns true if the process probe reported reduced privileges at startup.
    /// Convenience accessor so ShellLayer does not need to include Domain/ProcessModel.h.
    [[nodiscard]] bool hasReducedPrivileges() const;

  private:
    // shared_ptr (not unique_ptr): BackgroundSampler observes this model via a weak_ptr rather
    // than a raw pointer, so the sampler thread can never outlive-dereference it regardless of
    // destructor ordering.
    std::shared_ptr<Domain::ProcessModel> m_ProcessModel;
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler;
    std::int32_t m_SelectedPid = -1;

    std::chrono::milliseconds m_RefreshInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    std::chrono::milliseconds m_AppliedSamplerInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    bool m_ForceRefresh = false;
    bool m_IsActiveTab = false;
    float m_InteractionHoldSeconds = 0.0F;

    // Column visibility
    ProcessColumnSettings m_ColumnSettings;

    // Search/filter state - using std::string for dynamic sizing
    std::string m_SearchBuffer;

    // Tree view state
    bool m_TreeViewEnabled = false;
    std::unordered_set<std::uint64_t> m_CollapsedKeys; // uniqueKeys that are collapsed in tree view

    // Snapshot copy cache: only re-copy from ProcessModel when version changes (data updates at 1Hz,
    // but render runs at 60fps — this avoids 59/60 redundant copies of 50-100 ProcessSnapshot objects)
    std::vector<Domain::ProcessSnapshot> m_CachedRenderSnapshots;
    std::uint64_t m_CachedSnapshotVersion = std::numeric_limits<std::uint64_t>::max();

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

    /// Cache for text size measurements to avoid repeated ImGui::CalcTextSize calls.
    /// Invalidated when font changes (detected by comparing ImFont pointer).
    struct TextSizeCache
    {
        // Column header widths (indexed by ProcessColumn enum)
        std::array<float, processColumnCount()> columnHeaderWidths{};

        // Unit string widths for decimal-aligned rendering
        // (measured from actual rendered unit strings for accurate alignment)
        float unitPercentWidth = 0.0F;     // "%"
        float unitBytesWidth = 0.0F;       // " MB", " GB", etc.
        float unitBytesPerSecWidth = 0.0F; // " MB/s", " GB/s", etc.
        float unitPowerWidth = 0.0F;       // " W", " mW", etc.
        float singleDigitWidth = 0.0F;     // "0" for decimal part

        // Static label widths
        float treeViewLabelWidth = 0.0F;
        float listViewLabelWidth = 0.0F;

        // Widths for PRIORITY_LABELS (Domain::Priority::getPriorityLabel()'s fixed label set).
        // That column isn't backed by RowFormatCache (it's a live std::string_view lookup, not
        // a per-row formatted string), so its width can't ride along with RowFormatCache's
        // per-row AlignedCellText widths -- cached here instead, alongside this panel's other
        // small fixed-string-set widths.
        std::array<float, PRIORITY_LABELS.size()> priorityLabelWidths{};

        // Font pointer used when cache was populated (for invalidation)
        const ImFont* fontPtr = nullptr;

        /// Check if cache is valid for current font
        [[nodiscard]] bool isValid() const noexcept;

        /// Populate cache with current font measurements
        void populate();

        /// Get column header width (returns 0 if cache invalid)
        [[nodiscard]] float getHeaderWidth(ProcessColumn col) const noexcept
        {
            return columnHeaderWidths[toIndex(col)];
        }

        /// Get the cached width of one of Domain::Priority::getPriorityLabel()'s fixed labels.
        /// Returns 0 for any other string (getPriorityLabel never returns anything else).
        [[nodiscard]] float getPriorityLabelWidth(std::string_view label) const noexcept;
    };

    TextSizeCache m_TextSizeCache;

    /// Cache of pre-formatted strings per process row, keyed by uniqueKey.
    /// Rebuilt once when snapshot data changes (~1Hz), not per frame (60fps).
    /// Eliminates heap allocations for slow-changing formatted columns in renderProcessRow.
    struct RowFormatCache
    {
        AlignedCellText ppid;       // formatId(parentPid)          — immutable
        AlignedCellText startTime;  // formatEpochDateTimeShort      — immutable
        AlignedCellText cpuTime;    // formatCpuTimeCompact          — changes at 1Hz
        AlignedCellText cpuPercent; // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText memPercent; // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText virtualMem; // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText resident;   // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText peakRss;    // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText shared;     // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText ioRead;     // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText ioWrite;    // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText netSent;    // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText netRecv;    // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText power;      // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText gpuPercent; // pre-formatted to avoid per-frame decimal alignment work
        AlignedCellText gpuMemory;  // pre-formatted to avoid per-frame decimal alignment work
        std::string gpuEngines;     // comma-joined engine list; avoids per-frame string joins; left-aligned, no width needed
        AlignedCellText threads;    // formatOrDash/formatIntLocalized(threadCount)
        AlignedCellText handles;    // formatOrDash/formatIntLocalized(handleCount)
        AlignedCellText pageFaults; // formatOrDash/formatIntLocalized(pageFaults)
        AlignedCellText affinity;   // formatCpuAffinityMask         — rarely changes
        AlignedCellText gdiObjects; // formatIntLocalized(*gdiObjectCount) or "-"
    };

    std::unordered_map<std::uint64_t, RowFormatCache> m_RowFormatCache;
    std::uint64_t m_RowFormatCacheVersion = std::numeric_limits<std::uint64_t>::max();
    // Font used the last time m_RowFormatCache was populated. A font/size/DPI change alone
    // (without a new data version) must still force a rebuild, or every AlignedCellText's width
    // goes stale for whatever's cached until the next ~1Hz data refresh happens to land.
    const ImFont* m_RowFormatCacheFontPtr = nullptr;

    /// Ensure text size cache is populated for current font
    void ensureTextSizeCacheValid();

    /// Get the number of visible columns
    [[nodiscard]] int visibleColumnCount() const;

    /// Render process rows in tree view mode. Flattens the filtered/expanded tree into render
    /// order via ProcessTreeFlatten::collectProcessTreeRows() (a pure, separately-tested
    /// traversal -- see ProcessTreeFlatten.h), then applies ImGuiListClipper to that flat list
    /// so only visible rows reach the expensive part, renderProcessRow() -- see perf-plan #843's
    /// tree-view virtualization item.
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
