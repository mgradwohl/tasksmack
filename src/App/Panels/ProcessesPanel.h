#pragma once

#include "App/Panel.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/BackgroundSampler.h"
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ImFont; // Forward declaration for TextSizeCache

namespace App
{

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
    void renderContent();
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

    /// Get the current process snapshots.
    [[nodiscard]] std::vector<Domain::ProcessSnapshot> snapshots() const;

    /// Find a single snapshot by PID without copying the full vector.
    /// Searches the cached render snapshots; returns std::nullopt if not found.
    [[nodiscard]] std::optional<Domain::ProcessSnapshot> findSnapshot(std::int32_t pid) const;

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
    std::unique_ptr<Domain::ProcessModel> m_ProcessModel;
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
    };

    TextSizeCache m_TextSizeCache;

    /// Cache of pre-formatted strings per process row, keyed by uniqueKey.
    /// Rebuilt once when snapshot data changes (~1Hz), not per frame (60fps).
    /// Eliminates heap allocations for slow-changing formatted columns in renderProcessRow.
    struct RowFormatCache
    {
        std::string ppid;       // formatId(parentPid)          — immutable
        std::string startTime;  // formatEpochDateTimeShort      — immutable
        std::string cpuTime;    // formatCpuTimeCompact          — changes at 1Hz
        std::string cpuPercent; // pre-formatted to avoid per-frame decimal alignment work
        std::string memPercent; // pre-formatted to avoid per-frame decimal alignment work
        std::string virtualMem; // pre-formatted to avoid per-frame decimal alignment work
        std::string resident;   // pre-formatted to avoid per-frame decimal alignment work
        std::string peakRss;    // pre-formatted to avoid per-frame decimal alignment work
        std::string shared;     // pre-formatted to avoid per-frame decimal alignment work
        std::string ioRead;     // pre-formatted to avoid per-frame decimal alignment work
        std::string ioWrite;    // pre-formatted to avoid per-frame decimal alignment work
        std::string netSent;    // pre-formatted to avoid per-frame decimal alignment work
        std::string netRecv;    // pre-formatted to avoid per-frame decimal alignment work
        std::string power;      // pre-formatted to avoid per-frame decimal alignment work
        std::string gpuPercent; // pre-formatted to avoid per-frame decimal alignment work
        std::string gpuMemory;  // pre-formatted to avoid per-frame decimal alignment work
        std::string threads;    // formatOrDash/formatIntLocalized(threadCount)
        std::string handles;    // formatOrDash/formatIntLocalized(handleCount)
        std::string pageFaults; // formatOrDash/formatIntLocalized(pageFaults)
        std::string affinity;   // formatCpuAffinityMask         — rarely changes
        std::string gdiObjects; // formatIntLocalized(*gdiObjectCount) or "-"
    };

    std::unordered_map<std::uint64_t, RowFormatCache> m_RowFormatCache;
    std::uint64_t m_RowFormatCacheVersion = std::numeric_limits<std::uint64_t>::max();

    /// Ensure text size cache is populated for current font
    void ensureTextSizeCacheValid();

    /// Get the number of visible columns
    [[nodiscard]] int visibleColumnCount() const;

    /// Render process rows in tree view mode
    /// @param snapshots The full list of process snapshots.
    /// @param filteredIndices Indices into snapshots for processes matching the current filter.
    void renderTreeView(const std::vector<Domain::ProcessSnapshot>& snapshots, const std::vector<std::size_t>& filteredIndices);

    /// Render a single process and its children iteratively
    /// @param snapshots The full list of process snapshots.
    /// @param filteredSet Set of filtered indices for O(1) membership checks.
    /// @param procIdx Index of current process to render.
    /// @param depth Current depth in the tree hierarchy.
    void renderProcessTreeNode(const std::vector<Domain::ProcessSnapshot>& snapshots,
                               const std::unordered_set<std::size_t>& filteredSet,
                               std::size_t procIdx,
                               int depth);

    /// Render a single process row
    /// @param proc The process to render.
    /// @param depth Indentation depth in the tree.
    /// @param hasChildren Whether the process has children.
    /// @param isExpanded Whether the children are visible.
    void renderProcessRow(const Domain::ProcessSnapshot& proc, int depth, bool hasChildren, bool isExpanded);
};

} // namespace App
