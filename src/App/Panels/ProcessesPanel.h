#pragma once

#include "App/Panel.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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

  private:
    std::unique_ptr<Domain::ProcessModel> m_ProcessModel;
    std::int32_t m_SelectedPid = -1;

    std::chrono::milliseconds m_RefreshInterval{1000};
    float m_RefreshAccumulatorSec = 0.0F;
    bool m_ForceRefresh = false;

    // Column visibility
    ProcessColumnSettings m_ColumnSettings;

    // Search/filter state - using std::string for dynamic sizing
    std::string m_SearchBuffer;

    // Tree view state
    bool m_TreeViewEnabled = false;
    std::unordered_set<std::uint64_t> m_CollapsedKeys; // uniqueKeys that are collapsed in tree view

    // Cached tree structure (rebuilt on refresh timer in onUpdate)
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> m_CachedTree;

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

    mutable TextSizeCache m_TextSizeCache;

    /// Ensure text size cache is populated for current font
    void ensureTextSizeCacheValid() const;

    /// Get the number of visible columns
    [[nodiscard]] int visibleColumnCount() const;

    /// Build parent-child process tree structure
    /// @param snapshots The full list of process snapshots.
    /// @return Map of parent uniqueKey to vector of child indices
    [[nodiscard]] static std::unordered_map<std::uint64_t, std::vector<std::size_t>>
    buildProcessTree(const std::vector<Domain::ProcessSnapshot>& snapshots);

    /// Render process rows in tree view mode
    /// @param snapshots The full list of process snapshots.
    /// @param filteredIndices Indices into snapshots for processes matching the current filter.
    /// @param tree Mapping from parent uniqueKey to child process indices within snapshots.
    void renderTreeView(const std::vector<Domain::ProcessSnapshot>& snapshots,
                        const std::vector<std::size_t>& filteredIndices,
                        const std::unordered_map<std::uint64_t, std::vector<std::size_t>>& tree);

    /// Render a single process and its children iteratively
    /// @param snapshots The full list of process snapshots.
    /// @param tree Mapping from parent uniqueKey to child process indices within snapshots.
    /// @param filteredSet Set of filtered indices for O(1) membership checks.
    /// @param procIdx Index of current process to render.
    /// @param depth Current depth in the tree hierarchy.
    void renderProcessTreeNode(const std::vector<Domain::ProcessSnapshot>& snapshots,
                               const std::unordered_map<std::uint64_t, std::vector<std::size_t>>& tree,
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
