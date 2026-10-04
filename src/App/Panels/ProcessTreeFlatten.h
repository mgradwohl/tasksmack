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
#include <ranges>
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

namespace Detail
{

struct StackFrame
{
    std::size_t procIdx;
    int depth;
};

/// The walk behind collectProcessTreeRows() and buildProcessTreeRows(), with filter membership as a
/// predicate (`isFiltered(index)`) and the walk's stack supplied by the caller so it can be reused.
// isFiltered is called once per child, so it is used as an lvalue rather than forwarded.
template<typename IsFiltered>
inline void walkProcessTree(const std::vector<Domain::ProcessSnapshot>& snapshots,
                            IsFiltered&& isFiltered, // NOLINT(cppcoreguidelines-missing-std-forward)
                            const std::unordered_set<std::uint64_t>& collapsedKeys,
                            std::size_t procIdx,
                            int depth,
                            std::vector<StackFrame>& stack,
                            std::vector<ProcessTreeRow>& outRows)
{
    stack.clear();
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
        const bool isExpanded = !collapsedKeys.contains(proc.uniqueKey);

        // Only children in the filtered set count. An expanded node's are pushed in reverse, so they
        // pop off the stack -- and are emitted -- in their original order; a collapsed node only
        // needs to know whether it has any. No per-node list of children is built.
        bool hasChildren = false;
        for (const std::size_t childIdx : std::views::reverse(proc.childrenIndices))
        {
            if (!isFiltered(childIdx))
            {
                continue;
            }
            hasChildren = true;
            if (!isExpanded)
            {
                break;
            }
            stack.push_back(StackFrame{.procIdx = childIdx, .depth = frame.depth + 1});
        }

        outRows.push_back(
            ProcessTreeRow{.procIdx = frame.procIdx, .depth = frame.depth, .hasChildren = hasChildren, .isExpanded = isExpanded});
    }
}

} // namespace Detail

/// Iteratively walks one root process and its descendants (pre-order, with children pushed in
/// reverse so they pop off the stack in original order), appending one ProcessTreeRow per node
/// to `outRows`. Does not clear `outRows` first, so a caller can accumulate rows from multiple
/// root processes into one flat list.
///
/// Takes every input explicitly (snapshots, filtered-set membership, collapsed-key set) instead
/// of reading any ProcessesPanel member state, so it's fully testable without a live ImGui
/// context. The only side effect is an spdlog::warn() on cycle/depth-limit detection.
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
    std::vector<Detail::StackFrame> stack;
    stack.reserve(32); // Reserve space for typical tree depth to avoid reallocations
    Detail::walkProcessTree(
        snapshots,
        [&filteredSet](std::size_t index) { return filteredSet.contains(index); },
        collapsedKeys,
        procIdx,
        depth,
        stack,
        outRows);
}

/// Buffers buildProcessTreeRows() reuses from one build to the next, so a rebuild allocates nothing
/// once they have grown to the process count.
struct TreeFlattenScratch
{
    std::vector<char> inFiltered;      // indexed by snapshot index: passes the filter
    std::vector<char> isFilteredChild; // indexed by snapshot index: a child of a process that passes it
    std::vector<Detail::StackFrame> stack;
};

/// The whole tree view in render order: every filtered process that is not the child of another
/// filtered process is a root, walked in `filteredIndices` order (collectProcessTreeRows()).
/// `outRows` is cleared first. Filter membership is a bitmap over the snapshot indices rather than
/// a hash set, and the buffers come from `scratch`, so building the rows of a few thousand processes
/// is a few linear passes with no allocation once warmed (#1138). A child index out of range of
/// `snapshots` counts as filtered out.
inline void buildProcessTreeRows(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                 const std::vector<std::size_t>& filteredIndices,
                                 const std::unordered_set<std::uint64_t>& collapsedKeys,
                                 TreeFlattenScratch& scratch,
                                 std::vector<ProcessTreeRow>& outRows)
{
    outRows.clear();
    outRows.reserve(filteredIndices.size());
    const std::size_t count = snapshots.size();
    scratch.inFiltered.assign(count, 0);
    scratch.isFilteredChild.assign(count, 0);
    for (const std::size_t idx : filteredIndices)
    {
        if (idx < count)
        {
            scratch.inFiltered[idx] = 1;
        }
    }
    const auto isFiltered = [&scratch, count](std::size_t index)
    {
        return index < count && scratch.inFiltered[index] != 0;
    };

    // A process listed as a child of another filtered process is rendered under it, so it is not a
    // root: descents start only from the rest, which avoids rendering anything twice.
    for (const std::size_t idx : filteredIndices)
    {
        if (idx >= count)
        {
            continue;
        }
        for (const std::size_t childIdx : snapshots[idx].childrenIndices)
        {
            if (isFiltered(childIdx))
            {
                scratch.isFilteredChild[childIdx] = 1;
            }
        }
    }

    for (const std::size_t idx : filteredIndices)
    {
        if (idx < count && scratch.isFilteredChild[idx] == 0)
        {
            Detail::walkProcessTree(snapshots, isFiltered, collapsedKeys, idx, 0, scratch.stack, outRows);
        }
    }
}

/// What the tree view's rows are built from: a snapshot generation, a filter result and a set of
/// collapsed nodes, each named by a number that changes whenever its input does.
struct ProcessTreeRowsKey
{
    std::uint64_t snapshotVersion = 0;    // the snapshots the indices refer to
    std::uint64_t filterGeneration = 0;   // advanced whenever the filtered indices are rebuilt
    std::uint64_t collapseGeneration = 0; // advanced whenever a node is collapsed or expanded

    [[nodiscard]] bool operator==(const ProcessTreeRowsKey&) const = default;
};

/// The tree view's flattened rows, rebuilt only when their inputs change (#1138). They used to be
/// rebuilt every frame -- two hash sets of the filtered indices plus a vector per node -- although
/// they change only when a sample arrives, the filter changes, or a node is expanded or collapsed.
/// Rendering the rows (ImGuiListClipper) still happens every frame; building them does not.
class ProcessTreeRowsCache
{
  public:
    /// The rows for `key`: the cached ones if they were built for exactly this key, otherwise
    /// rebuilt from the given inputs (buildProcessTreeRows()). The caller must advance the key
    /// whenever any of the inputs changes. Valid until the next call or invalidate().
    [[nodiscard]] const std::vector<ProcessTreeRow>& rows(const ProcessTreeRowsKey& key,
                                                          const std::vector<Domain::ProcessSnapshot>& snapshots,
                                                          const std::vector<std::size_t>& filteredIndices,
                                                          const std::unordered_set<std::uint64_t>& collapsedKeys)
    {
        if (!m_Valid || key != m_Key)
        {
            buildProcessTreeRows(snapshots, filteredIndices, collapsedKeys, m_Scratch, m_Rows);
            m_Key = key;
            m_Valid = true;
            ++m_BuildCount;
        }
        return m_Rows;
    }

    /// Forget the cached rows, so the next rows() call rebuilds whatever its key.
    void invalidate() noexcept
    {
        m_Valid = false;
    }

    /// How many times rows() has rebuilt.
    [[nodiscard]] std::uint64_t buildCount() const noexcept
    {
        return m_BuildCount;
    }

  private:
    std::vector<ProcessTreeRow> m_Rows;
    TreeFlattenScratch m_Scratch;
    ProcessTreeRowsKey m_Key;
    std::uint64_t m_BuildCount = 0;
    bool m_Valid = false;
};

} // namespace App::ProcessTreeFlatten
