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
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

    /// Render the panel.
    /// @param open Pointer to visibility flag (for window close button).
    void render(bool* open) override;

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

  private:
    std::unique_ptr<Domain::ProcessModel> m_ProcessModel;
    std::int32_t m_SelectedPid = -1;

    std::chrono::milliseconds m_RefreshInterval{1000};
    float m_RefreshAccumulatorSec = 0.0F;
    bool m_ForceRefresh = false;

    // Column visibility
    ProcessColumnSettings m_ColumnSettings;

    // Search/filter state
    std::array<char, 256> m_SearchBuffer{};

    // Tree view state
    bool m_TreeViewEnabled = false;
    std::unordered_set<std::int32_t> m_CollapsedPids; // PIDs that are collapsed in tree view

    /// Get the number of visible columns
    [[nodiscard]] int visibleColumnCount() const;

    /// Build parent-child process tree structure
    /// @return Map of parent PID to vector of child indices
    [[nodiscard]] std::unordered_map<std::int32_t, std::vector<std::size_t>>
    buildProcessTree(const std::vector<Domain::ProcessSnapshot>& snapshots) const;

    /// Render process rows in tree view mode
    void renderTreeView(const std::vector<Domain::ProcessSnapshot>& snapshots,
                        const std::vector<std::size_t>& filteredIndices,
                        const std::unordered_map<std::int32_t, std::vector<std::size_t>>& tree);

    /// Render a single process and its children recursively
    void renderProcessTreeNode(const std::vector<Domain::ProcessSnapshot>& snapshots,
                               const std::unordered_map<std::int32_t, std::vector<std::size_t>>& tree,
                               const std::unordered_set<std::size_t>& filteredSet,
                               std::size_t procIdx,
                               int depth);

    /// Render a single process row
    void renderProcessRow(const Domain::ProcessSnapshot& proc, int depth, bool hasChildren, bool isExpanded);
};

} // namespace App
