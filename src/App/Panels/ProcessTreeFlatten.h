#pragma once

// Pure tree-flattening logic extracted from ProcessesPanel::renderTreeView() (perf-plan #843
// phase 1), so it can be unit-tested directly instead of only indirectly through a live ImGui
// context. See CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern
// (also used by AdaptiveIntervalUtils.h, TitleBarGeometry.h). Unlike most of ProcessesPanel.cpp,
// this traversal makes no ImGui calls at all -- it only walks process-snapshot indices and
// sets -- so there is no live-context blocker to extracting and testing it directly.

#include "Domain/ProcessSnapshot.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace App::ProcessTreeFlatten
{

/// Maximum tree depth to walk before assuming a cycle or malformed parent/child data and
/// abandoning that branch (logging a warning rather than looping forever).
inline constexpr int MAX_DEPTH = 1000;

/// One row of a flattened tree-view render order: a process snapshot index plus the tree
/// context (depth/hasChildren/isExpanded) renderProcessRow() needs. Building a flat list of
/// these lets ImGuiListClipper bound the expensive part of tree-view rendering (measuring/
/// drawing every column) to visible rows only, instead of every expanded row every frame.
struct ProcessTreeRow
{
    std::size_t procIdx;
    int depth;
    bool hasChildren;
    bool isExpanded;
};

/// Iteratively walks one root process and its descendants (pre-order, with children pushed in
/// reverse so they pop off the stack in original order), appending one ProcessTreeRow per node
/// to `outRows`. Does not clear `outRows` first, so a caller can accumulate rows from multiple
/// root processes into one flat list.
///
/// Takes every input explicitly (snapshots, filtered-set membership, collapsed-key set) instead
/// of reading any ProcessesPanel member state, so it's fully testable without a live ImGui
/// context. The only side effect is an spdlog::warn() on cycle/depth-limit detection, matching
/// the pre-extraction behavior exactly.
///
/// @param snapshots     The full list of process snapshots.
/// @param filteredSet   Indices into `snapshots` that pass the current filter, for O(1) membership checks.
/// @param collapsedKeys uniqueKeys of processes the user has collapsed (their children are skipped).
/// @param procIdx       Index of the root process to start this walk from.
/// @param depth         Depth to assign the root process (0 for a top-level root).
/// @param outRows       Appended to in render order; not cleared by this function.
inline void collectProcessTreeRows(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                   const std::unordered_set<std::size_t>& filteredSet,
                                   const std::unordered_set<std::uint64_t>& collapsedKeys,
                                   std::size_t procIdx,
                                   int depth,
                                   std::vector<ProcessTreeRow>& outRows)
{
    struct StackFrame
    {
        std::size_t procIdx;
        int depth;
    };

    std::vector<StackFrame> stack;
    stack.reserve(32); // Reserve space for typical tree depth to avoid reallocations
    stack.push_back(StackFrame{.procIdx = procIdx, .depth = depth});

    while (!stack.empty())
    {
        const StackFrame frame = stack.back();
        stack.pop_back();

        // Prevent excessive depth (may indicate cycles or malformed data)
        if (frame.depth >= MAX_DEPTH)
        {
            spdlog::warn("ProcessTreeFlatten: Maximum tree depth ({}) exceeded, possible cycle or malformed data", MAX_DEPTH);
            continue;
        }

        const auto& proc = snapshots[frame.procIdx];

        // Check if this process has children (in the filtered set)
        bool hasChildren = false;
        std::vector<std::size_t> filteredChildren;

        if (!proc.childrenIndices.empty())
        {
            filteredChildren.reserve(proc.childrenIndices.size());
            // Only count children that are in the filtered set
            for (const std::size_t childIdx : proc.childrenIndices)
            {
                if (filteredSet.contains(childIdx))
                {
                    filteredChildren.push_back(childIdx);
                }
            }
            hasChildren = !filteredChildren.empty();
        }

        const bool isExpanded = !collapsedKeys.contains(proc.uniqueKey);

        outRows.push_back(
            ProcessTreeRow{.procIdx = frame.procIdx, .depth = frame.depth, .hasChildren = hasChildren, .isExpanded = isExpanded});

        // Add children to stack if expanded (in reverse order for correct rendering)
        if (hasChildren && isExpanded)
        {
            for (auto it = filteredChildren.rbegin(); it != filteredChildren.rend(); ++it)
            {
                stack.push_back(StackFrame{.procIdx = *it, .depth = frame.depth + 1});
            }
        }
    }
}

} // namespace App::ProcessTreeFlatten
