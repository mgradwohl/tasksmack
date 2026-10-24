#pragma once

// Pure tree-flattening logic extracted from ProcessesPanel::renderTreeView() (perf-plan #843
// phase 1), so it can be unit-tested directly instead of only indirectly through a live ImGui
// context. See CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern
// (also used by TitleBarGeometry.h). Unlike most of ProcessesPanel.cpp,
// this traversal makes no ImGui calls at all -- it only walks process-snapshot indices and
// sets -- so there is no live-context blocker to extracting and testing it directly.

#include "Domain/ProcessSnapshot.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    bool isContext; // shown only as an ancestor of a filter match, not a match itself (#838)
    // Children shown under it, expanded or not: the rows expanding it adds, its badge (#838).
    std::size_t childCount = 0;
};

namespace Detail
{

struct StackFrame
{
    std::size_t procIdx;
    int depth;
};

} // namespace Detail

/// Marks a process that is not in a build's `order`, or one with no parent among those that are.
inline constexpr std::size_t NOT_SHOWN = std::numeric_limits<std::size_t>::max();

/// What a filtered tree shows besides the matches (#838).
enum class FilterContext : std::uint8_t
{
    MatchesOnly,   ///< Only the processes in `order`: a match whose parent is filtered out is a root.
    WithAncestors, ///< Every ancestor of a match too, flagged ProcessTreeRow::isContext unless it matches.
};

/// Buffers buildProcessTreeRows() reuses from one build to the next, so a rebuild allocates nothing
/// once they have grown to the process count. All but `children`, `stack` and `order` are indexed by
/// snapshot index.
struct TreeFlattenScratch
{
    std::vector<std::size_t> rank;       // position in the build's `order`; NOT_SHOWN if filtered out
    std::vector<std::size_t> parent;     // the shown process this one is drawn under; NOT_SHOWN for a root
    std::vector<std::size_t> childStart; // count + 1 offsets: process i's children are children[childStart[i], childStart[i + 1])
    std::vector<std::size_t> childFill;  // next free slot in each process's group of `children` while it is filled
    std::vector<std::size_t> children;   // every shown process with a parent, grouped by parent, each group in `order`
    std::vector<Detail::StackFrame> stack;
    // FilterContext::WithAncestors only:
    std::vector<std::size_t> ancestor; // the process listing this one as a child, among all; NOT_SHOWN for none
    std::vector<std::uint8_t> state;   // Detail::MATCHED, WALKED and CONTEXT bits
    std::vector<std::size_t> order;    // the build's `order`, each match preceded by its ancestors not yet in it
};

namespace Detail
{

inline constexpr std::uint8_t MATCHED = 1U; // in the caller's `order`
inline constexpr std::uint8_t WALKED = 2U;  // reached from below; its own ancestors are in scratch.order
inline constexpr std::uint8_t CONTEXT = 4U; // reached from below without matching: drawn dimmed

/// Fills scratch.order with `order` plus every ancestor of a process in it. Each ancestor goes just
/// before the first match it leads to, so a parent's group of children still follows `order`: a
/// context ancestor sits among its siblings where its best-placed match would.
inline void addFilterAncestors(const std::vector<Domain::ProcessSnapshot>& snapshots,
                               const std::vector<std::size_t>& order,
                               TreeFlattenScratch& scratch)
{
    const std::size_t count = snapshots.size();
    scratch.ancestor.assign(count, NOT_SHOWN);
    for (std::size_t idx = 0; idx < count; ++idx)
    {
        for (const std::size_t childIdx : snapshots[idx].childrenIndices)
        {
            if (childIdx < count && childIdx != idx && scratch.ancestor[childIdx] == NOT_SHOWN)
            {
                scratch.ancestor[childIdx] = idx;
            }
        }
    }
    scratch.state.assign(count, 0U);
    for (const std::size_t idx : order)
    {
        if (idx < count)
        {
            scratch.state[idx] |= MATCHED;
        }
    }
    scratch.order.clear();
    scratch.order.reserve(order.size());
    for (const std::size_t idx : order)
    {
        // Up to the first ancestor already walked: everything above it is in scratch.order. Marking each
        // as it is passed also ends the climb on a cycle in a corrupt snapshot.
        for (std::size_t up = idx < count ? scratch.ancestor[idx] : NOT_SHOWN; up != NOT_SHOWN && (scratch.state[up] & WALKED) == 0U;
             up = scratch.ancestor[up])
        {
            scratch.state[up] |= WALKED;
            if ((scratch.state[up] & MATCHED) == 0U)
            {
                scratch.state[up] |= CONTEXT;
                scratch.order.push_back(up);
            }
        }
        scratch.order.push_back(idx);
    }
}

} // namespace Detail

/// The whole tree view in render order. `order` is the filtered processes in the order the table
/// shows them: snapshot order, or the table's sort (#838). A process whose parent is shown is drawn
/// under it; the rest are roots. Roots come out in `order`, and so does each parent's group of
/// children, so sorting `order` sorts every set of siblings while every child stays under its parent.
///
/// With FilterContext::WithAncestors, a match's ancestors are shown as well, flagged isContext unless
/// they match too, so a match keeps its place in the tree instead of becoming a root (#838). An
/// ancestor is ordered among its siblings by the first match under it in `order`.
///
/// Each parent's shown children are gathered once into one flat array (offsets per parent, then the
/// children grouped by parent), so the walk reads them instead of filtering the snapshot's own child
/// lists at every node. With the buffers from `scratch`, building the rows of a few thousand
/// processes is a few linear passes with no allocation once warmed (#1138). `outRows` is cleared first.
///
/// A process listed as a child by more than one shown process is drawn once, under the first of them
/// in snapshot order, and one listed as its own child is not. A child index out of range of
/// `snapshots`, and a repeat of an index already in `order`, are ignored.
inline void buildProcessTreeRows(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                 const std::vector<std::size_t>& matches,
                                 const std::unordered_set<std::uint64_t>& collapsedKeys,
                                 FilterContext context,
                                 TreeFlattenScratch& scratch,
                                 std::vector<ProcessTreeRow>& outRows)
{
    const bool withAncestors = context == FilterContext::WithAncestors;
    if (withAncestors)
    {
        Detail::addFilterAncestors(snapshots, matches, scratch);
    }
    const std::vector<std::size_t>& order = withAncestors ? scratch.order : matches;

    outRows.clear();
    outRows.reserve(order.size());
    const std::size_t count = snapshots.size();

    scratch.rank.assign(count, NOT_SHOWN);
    for (std::size_t pos = 0; pos < order.size(); ++pos)
    {
        if (const std::size_t idx = order[pos]; idx < count && scratch.rank[idx] == NOT_SHOWN)
        {
            scratch.rank[idx] = pos;
        }
    }
    const auto isShown = [&scratch, count](std::size_t index)
    {
        return index < count && scratch.rank[index] != NOT_SHOWN;
    };
    // Whether order[pos] is that process's first listing; a repeat is skipped, so nothing is drawn twice.
    const auto isFirstListing = [&scratch, &order, count](std::size_t pos)
    {
        return order[pos] < count && scratch.rank[order[pos]] == pos;
    };

    // Each shown process's shown parent, counted into the slot after the parent's in childStart.
    scratch.parent.assign(count, NOT_SHOWN);
    scratch.childStart.assign(count + 1, 0);
    for (std::size_t idx = 0; idx < count; ++idx)
    {
        if (!isShown(idx))
        {
            continue;
        }
        for (const std::size_t childIdx : snapshots[idx].childrenIndices)
        {
            if (childIdx != idx && isShown(childIdx) && scratch.parent[childIdx] == NOT_SHOWN)
            {
                scratch.parent[childIdx] = idx;
                ++scratch.childStart[idx + 1];
            }
        }
    }
    for (std::size_t idx = 0; idx < count; ++idx)
    {
        scratch.childStart[idx + 1] += scratch.childStart[idx];
    }

    // The children, filled in `order`, so each parent's group is in it too.
    scratch.childFill.assign(scratch.childStart.begin(), scratch.childStart.end() - 1);
    scratch.children.resize(scratch.childStart[count]);
    for (std::size_t pos = 0; pos < order.size(); ++pos)
    {
        if (isFirstListing(pos) && scratch.parent[order[pos]] != NOT_SHOWN)
        {
            scratch.children[scratch.childFill[scratch.parent[order[pos]]]++] = order[pos];
        }
    }

    // Pre-order from each root. Every shown process has at most one parent, so no walk comes back round
    // to a process it already drew; the depth limit only guards against a corrupt snapshot.
    for (std::size_t pos = 0; pos < order.size(); ++pos)
    {
        if (!isFirstListing(pos) || scratch.parent[order[pos]] != NOT_SHOWN)
        {
            continue;
        }
        scratch.stack.clear();
        scratch.stack.push_back(Detail::StackFrame{.procIdx = order[pos], .depth = 0});
        while (!scratch.stack.empty())
        {
            const Detail::StackFrame frame = scratch.stack.back();
            scratch.stack.pop_back();
            if (frame.depth >= MAX_DEPTH)
            {
                spdlog::warn("ProcessTreeFlatten: Maximum tree depth ({}) exceeded, possible cycle or malformed data", MAX_DEPTH);
                continue;
            }

            const std::size_t first = scratch.childStart[frame.procIdx];
            const std::size_t last = scratch.childStart[frame.procIdx + 1];
            const bool isExpanded = !collapsedKeys.contains(snapshots[frame.procIdx].uniqueKey);
            if (isExpanded)
            {
                // Pushed in reverse, so they pop off -- and are drawn -- in their group's order.
                for (std::size_t k = last; k > first; --k)
                {
                    scratch.stack.push_back(Detail::StackFrame{.procIdx = scratch.children[k - 1], .depth = frame.depth + 1});
                }
            }
            outRows.push_back(ProcessTreeRow{.procIdx = frame.procIdx,
                                             .depth = frame.depth,
                                             .hasChildren = last > first,
                                             .isExpanded = isExpanded,
                                             .isContext = withAncestors && (scratch.state[frame.procIdx] & Detail::CONTEXT) != 0U,
                                             .childCount = last - first});
        }
    }
}

/// As buildProcessTreeRows() above, showing only the processes in `order` (FilterContext::MatchesOnly).
inline void buildProcessTreeRows(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                 const std::vector<std::size_t>& order,
                                 const std::unordered_set<std::uint64_t>& collapsedKeys,
                                 TreeFlattenScratch& scratch,
                                 std::vector<ProcessTreeRow>& outRows)
{
    buildProcessTreeRows(snapshots, order, collapsedKeys, FilterContext::MatchesOnly, scratch, outRows);
}

/// Collapse All (#838): adds the uniqueKey of every process that lists a child to `collapsedKeys`, so
/// only the roots stay in view. Returns whether the set changed.
inline bool collapseAll(const std::vector<Domain::ProcessSnapshot>& snapshots, std::unordered_set<std::uint64_t>& collapsedKeys)
{
    bool changed = false;
    for (const Domain::ProcessSnapshot& proc : snapshots)
    {
        if (!proc.childrenIndices.empty())
        {
            changed |= collapsedKeys.insert(proc.uniqueKey).second;
        }
    }
    return changed;
}

/// Drops from `collapsedKeys` every key not in `liveKeys` -- processes that have exited -- so the set
/// cannot grow without bound over a long session (#838). Returns whether it removed any.
inline bool pruneCollapsedKeys(std::unordered_set<std::uint64_t>& collapsedKeys, const std::unordered_set<std::uint64_t>& liveKeys)
{
    return std::erase_if(collapsedKeys, [&liveKeys](std::uint64_t key) { return !liveKeys.contains(key); }) > 0;
}

/// What the tree view's rows are built from: a snapshot generation, a filter result, its order and a
/// set of collapsed nodes, each named by a number that changes whenever its input does.
struct ProcessTreeRowsKey
{
    std::uint64_t snapshotVersion = 0;                  // the snapshots the indices refer to
    std::uint64_t filterGeneration = 0;                 // advanced whenever the filtered indices are rebuilt
    std::uint64_t sortGeneration = 0;                   // advanced whenever they are sorted (#838)
    std::uint64_t collapseGeneration = 0;               // advanced whenever a node is collapsed or expanded
    FilterContext context = FilterContext::MatchesOnly; // whether the matches' ancestors are shown (#838)

    [[nodiscard]] bool operator==(const ProcessTreeRowsKey&) const = default;
};

/// The tree view's flattened rows, rebuilt only when their inputs change (#1138). They used to be
/// rebuilt every frame -- two hash sets of the filtered indices plus a vector per node -- although
/// they change only when a sample arrives, the filter or sort changes, or a node is expanded or
/// collapsed. Rendering the rows (ImGuiListClipper) still happens every frame; building them does not.
class ProcessTreeRowsCache
{
  public:
    /// The rows for `key`: the cached ones if they were built for exactly this key, otherwise
    /// rebuilt from the given inputs (buildProcessTreeRows(), with key.context). The caller must
    /// advance the key whenever any of the inputs changes. Valid until the next call or invalidate().
    [[nodiscard]] const std::vector<ProcessTreeRow>& rows(const ProcessTreeRowsKey& key,
                                                          const std::vector<Domain::ProcessSnapshot>& snapshots,
                                                          const std::vector<std::size_t>& order,
                                                          const std::unordered_set<std::uint64_t>& collapsedKeys)
    {
        if (!m_Valid || key != m_Key)
        {
            buildProcessTreeRows(snapshots, order, collapsedKeys, key.context, m_Scratch, m_Rows);
            m_MaxDepth = 0;
            for (const ProcessTreeRow& row : m_Rows)
            {
                m_MaxDepth = std::max(m_MaxDepth, row.depth);
            }
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

    /// The deepest row of the last build, 0 for none: what the tree's indent per level is fitted to (#838).
    [[nodiscard]] int maxDepth() const noexcept
    {
        return m_MaxDepth;
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
    int m_MaxDepth = 0;
    bool m_Valid = false;
};

} // namespace App::ProcessTreeFlatten
