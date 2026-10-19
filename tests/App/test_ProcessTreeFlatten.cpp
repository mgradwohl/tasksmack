#include "App/Panels/ProcessTreeFlatten.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Mocks/MockProbes.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numeric>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Domain::ProcessSnapshot;
using ProcessTreeFlatten::ProcessTreeRow;

/// Builds a minimal snapshot for tree-flattening tests: only uniqueKey and childrenIndices are
/// read by buildProcessTreeRows(), so every other ProcessSnapshot field is left default.
[[nodiscard]] ProcessSnapshot makeSnapshot(std::uint64_t uniqueKey, std::vector<std::size_t> childrenIndices = {})
{
    ProcessSnapshot snap;
    snap.uniqueKey = uniqueKey;
    snap.childrenIndices = std::move(childrenIndices);
    return snap;
}

/// The rows buildProcessTreeRows() gives for `order` (FilterContext::MatchesOnly), with fresh buffers.
[[nodiscard]] std::vector<ProcessTreeRow> buildRows(const std::vector<ProcessSnapshot>& snapshots,
                                                    const std::vector<std::size_t>& order,
                                                    const std::unordered_set<std::uint64_t>& collapsedKeys = {})
{
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, order, collapsedKeys, scratch, rows);
    return rows;
}

/// (procIdx, depth, hasChildren, isExpanded) of each row.
using RowState = std::tuple<std::size_t, int, bool, bool>;

[[nodiscard]] std::vector<RowState> rowStates(const std::vector<ProcessTreeRow>& rows)
{
    std::vector<RowState> out;
    out.reserve(rows.size());
    for (const ProcessTreeRow& row : rows)
    {
        out.emplace_back(row.procIdx, row.depth, row.hasChildren, row.isExpanded);
    }
    return out;
}

// =============================================================================
// Pre-order traversal
// =============================================================================

TEST(ProcessTreeFlattenTest, SingleNodeWithNoChildrenProducesOneRow)
{
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(1)};

    const auto rows = buildRows(snapshots, {0});

    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_EQ(rows[0].depth, 0);
    EXPECT_FALSE(rows[0].hasChildren);
    EXPECT_TRUE(rows[0].isExpanded); // not in collapsedKeys
    EXPECT_FALSE(rows[0].isContext);
    EXPECT_EQ(rows[0].childCount, 0U);
}

TEST(ProcessTreeFlattenTest, VisitsRootThenFirstChildThenGrandchildThenSecondChild)
{
    // Tree: 0 -> {1, 2}; 1 -> {3}. Pre-order DFS visiting children left-to-right must yield
    // 0, 1, 3, 2 -- not 0, 2, 1, 3 (which the reverse-push stack trick exists to avoid) and not
    // a breadth-first order (0, 1, 2, 3).
    const std::vector<ProcessSnapshot> snapshots = {
        makeSnapshot(10, {1, 2}), // idx0: root, children idx1 and idx2
        makeSnapshot(20, {3}),    // idx1: first child, has its own child idx3
        makeSnapshot(30),         // idx2: second child, leaf
        makeSnapshot(40),         // idx3: grandchild, leaf
    };

    const auto rows = buildRows(snapshots, {0, 1, 2, 3});

    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_EQ(rows[0].depth, 0);
    EXPECT_EQ(rows[1].procIdx, 1U);
    EXPECT_EQ(rows[1].depth, 1);
    EXPECT_EQ(rows[2].procIdx, 3U);
    EXPECT_EQ(rows[2].depth, 2);
    EXPECT_EQ(rows[3].procIdx, 2U);
    EXPECT_EQ(rows[3].depth, 1);
}

TEST(ProcessTreeFlattenTest, EveryRootComesOutAndOutRowsIsClearedFirst)
{
    // Unrelated processes are each a root, all in one list -- a regression that stopped after the
    // first root would silently drop the rest. Rows left from an earlier build are replaced, not kept.
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(1), makeSnapshot(2)};
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows = {
        ProcessTreeRow{.procIdx = 1, .depth = 3, .hasChildren = true, .isExpanded = false, .isContext = true}};

    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {0, 1}, {}, scratch, rows);

    const std::vector<RowState> expected = {{0, 0, false, true}, {1, 0, false, true}};
    EXPECT_EQ(rowStates(rows), expected);
}

// =============================================================================
// Collapsed descendants
// =============================================================================

TEST(ProcessTreeFlattenTest, CollapsedNodeSuppressesDescendantsButStillAppearsItself)
{
    const std::vector<ProcessSnapshot> snapshots = {
        makeSnapshot(10, {1}), // idx0: root, one child
        makeSnapshot(20),      // idx1: child, leaf
    };
    const std::unordered_set<std::uint64_t> collapsedKeys = {10}; // collapse the root by its uniqueKey

    const auto rows = buildRows(snapshots, {0, 1}, collapsedKeys);

    ASSERT_EQ(rows.size(), 1U) << "the child must not appear while its parent is collapsed";
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_TRUE(rows[0].hasChildren) << "hasChildren reflects tree structure, independent of collapse state";
    EXPECT_FALSE(rows[0].isExpanded);
}

TEST(ProcessTreeFlattenTest, CollapsingAMidTreeNodeOnlyHidesItsOwnSubtree)
{
    // Tree: 0 -> {1, 2}; 1 -> {3}. Collapse node 1 (uniqueKey 20): node 3 must disappear, but
    // sibling node 2 (not a descendant of 1) must still appear.
    const std::vector<ProcessSnapshot> snapshots = {
        makeSnapshot(10, {1, 2}),
        makeSnapshot(20, {3}),
        makeSnapshot(30),
        makeSnapshot(40),
    };
    const std::unordered_set<std::uint64_t> collapsedKeys = {20};

    const auto rows = buildRows(snapshots, {0, 1, 2, 3}, collapsedKeys);

    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_EQ(rows[1].procIdx, 1U);
    EXPECT_FALSE(rows[1].isExpanded);
    EXPECT_EQ(rows[2].procIdx, 2U) << "sibling of the collapsed node must still be visited";
}

// =============================================================================
// Filtering
// =============================================================================

TEST(ProcessTreeFlattenTest, FilteredOutChildIsExcludedFromRowsAndFromHasChildren)
{
    const std::vector<ProcessSnapshot> snapshots = {
        makeSnapshot(10, {1}), // idx0: root, one child -- but that child will be filtered out
        makeSnapshot(20),      // idx1: child, leaf
    };

    const auto rows = buildRows(snapshots, {0}); // idx1 does not pass the filter

    ASSERT_EQ(rows.size(), 1U) << "the filtered-out child must not produce a row";
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_FALSE(rows[0].hasChildren) << "hasChildren must reflect filtered children, not raw childrenIndices";
}

TEST(ProcessTreeFlattenTest, PartiallyFilteredChildrenKeepsOnlyThePassingOnes)
{
    const std::vector<ProcessSnapshot> snapshots = {
        makeSnapshot(10, {1, 2}), // idx0: root, two children; idx2 will be filtered out
        makeSnapshot(20),         // idx1: passes the filter
        makeSnapshot(30),         // idx2: filtered out
    };

    const auto rows = buildRows(snapshots, {0, 1});

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_TRUE(rows[0].hasChildren);
    EXPECT_EQ(rows[1].procIdx, 1U);
}

// =============================================================================
// Malformed parent/child data: the depth limit, cycles and self-parents
// =============================================================================

TEST(ProcessTreeFlattenTest, AChainDeeperThanTheLimitStopsAtMaxDepth)
{
    // Each process lists the next as its child, MAX_DEPTH + 2 deep. The walk draws depths 0 to
    // MAX_DEPTH - 1 and abandons the rest of the branch rather than going on.
    constexpr auto COUNT = static_cast<std::size_t>(ProcessTreeFlatten::MAX_DEPTH) + 2;
    std::vector<ProcessSnapshot> snapshots;
    snapshots.reserve(COUNT);
    std::vector<std::size_t> order(COUNT);
    for (std::size_t i = 0; i < COUNT; ++i)
    {
        snapshots.push_back(i + 1 < COUNT ? makeSnapshot(1000 + i, {i + 1}) : makeSnapshot(1000 + i));
        order[i] = i;
    }

    const auto rows = buildRows(snapshots, order);

    ASSERT_EQ(rows.size(), static_cast<std::size_t>(ProcessTreeFlatten::MAX_DEPTH));
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        ASSERT_EQ(rows[i].procIdx, i);
        ASSERT_EQ(rows[i].depth, static_cast<int>(i));
    }
    EXPECT_TRUE(rows.back().hasChildren); // its child is cut by the limit, not filtered out
}

TEST(ProcessTreeFlattenTest, AParentCycleAmongTheMatchesEndsTheBuild)
{
    // 0 and 1 list each other as children: a corrupt snapshot. The build ends, and the unrelated
    // process is still drawn.
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(10, {1}), makeSnapshot(11, {0}), makeSnapshot(12)};

    const auto rows = buildRows(snapshots, {0, 1, 2});

    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.back().procIdx, 2U);
    EXPECT_LE(rows.size(), 3U);
}

TEST(ProcessTreeFlattenTest, AProcessListedAsItsOwnChildIsNotDrawnUnderItself)
{
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(10, {0, 1}), makeSnapshot(11)};

    const auto rows = buildRows(snapshots, {0, 1});

    const std::vector<RowState> expected = {{0, 0, true, true}, {1, 1, false, true}};
    EXPECT_EQ(rowStates(rows), expected);
    EXPECT_EQ(rows[0].childCount, 1U);
}

// =============================================================================
// Whole-tree build and its cache (#1138)
// =============================================================================

/// Two trees and a lone process: 0 -> {1 -> {3, 4}, 2}, 5 -> {6}, 7.
[[nodiscard]] std::vector<ProcessSnapshot> forest()
{
    return {makeSnapshot(100, {1, 2}),
            makeSnapshot(101, {3, 4}),
            makeSnapshot(102),
            makeSnapshot(103),
            makeSnapshot(104),
            makeSnapshot(105, {6}),
            makeSnapshot(106),
            makeSnapshot(107)};
}

TEST(ProcessTreeFlattenTest, WholeTreeBuildGivesEveryRowItsDepthChildrenAndExpandedState)
{
    const auto snapshots = forest();
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;

    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {0, 1, 2, 3, 4, 5, 6, 7}, {}, scratch, rows);
    const std::vector<RowState> whole = {{0, 0, true, true},
                                         {1, 1, true, true},
                                         {3, 2, false, true},
                                         {4, 2, false, true},
                                         {2, 1, false, true},
                                         {5, 0, true, true},
                                         {6, 1, false, true},
                                         {7, 0, false, true}};
    EXPECT_EQ(rowStates(rows), whole);

    // A filtered-out parent promotes its filtered children to roots; a collapsed node hides its own.
    // The same scratch is reused, as ProcessTreeRowsCache does.
    const std::unordered_set<std::uint64_t> collapsed = {105};
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {1, 3, 4, 5, 6, 7}, collapsed, scratch, rows);
    const std::vector<RowState> filtered = {
        {1, 0, true, true}, {3, 1, false, true}, {4, 1, false, true}, {5, 0, true, false}, {7, 0, false, true}};
    EXPECT_EQ(rowStates(rows), filtered); // 6 hidden under the collapsed 5
}
TEST(ProcessTreeFlattenTest, WholeTreeBuildIgnoresAChildIndexOutOfRange)
{
    // The hash sets treated an unknown index as filtered out; the bitmaps must too, not read past
    // their end.
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(10, {1, 99}), makeSnapshot(11)};
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {0, 1}, {}, scratch, rows);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_TRUE(rows[0].hasChildren);
    EXPECT_EQ(rows[1].procIdx, 1U);
    EXPECT_EQ(rows[1].depth, 1);
}

// =============================================================================
// Sibling sorting (#838): the order given sorts every set of siblings, the hierarchy stays
// =============================================================================

/// (procIdx, depth) of each row, for comparing a whole render order at once.
[[nodiscard]] std::vector<std::pair<std::size_t, int>> shape(const std::vector<ProcessTreeRow>& rows)
{
    std::vector<std::pair<std::size_t, int>> out;
    out.reserve(rows.size());
    for (const ProcessTreeRow& row : rows)
    {
        out.emplace_back(row.procIdx, row.depth);
    }
    return out;
}

TEST(ProcessTreeFlattenTest, ASortedOrderSortsEachSetOfSiblingsUnderItsParent)
{
    // 0 -> {1 -> {3, 4}, 2}, 5 -> {6}, 7; walked in reverse snapshot order.
    const auto snapshots = forest();
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {7, 6, 5, 4, 3, 2, 1, 0}, {}, scratch, rows);

    // Roots 7, 5, 0 in that order; 0's children 2 then 1; 1's children 4 then 3. Every child is
    // still drawn right under its parent, one level deeper.
    const std::vector<std::pair<std::size_t, int>> expected = {{7, 0}, {5, 0}, {6, 1}, {0, 0}, {2, 1}, {1, 1}, {4, 2}, {3, 2}};
    EXPECT_EQ(shape(rows), expected);
    EXPECT_TRUE(rows[3].hasChildren);
    EXPECT_FALSE(rows[4].hasChildren);
}

TEST(ProcessTreeFlattenTest, NaturalOrderGivesTheTreeInSnapshotOrder)
{
    // Snapshot order is what tree view showed before it sorted: children in the order their parent
    // lists them, which is snapshot order (ProcessModel appends them as it meets them).
    const auto snapshots = forest();
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {0, 1, 2, 3, 4, 5, 6, 7}, {}, scratch, rows);
    const std::vector<std::pair<std::size_t, int>> expected = {{0, 0}, {1, 1}, {3, 2}, {4, 2}, {2, 1}, {5, 0}, {6, 1}, {7, 0}};
    EXPECT_EQ(shape(rows), expected);
}

TEST(ProcessTreeFlattenTest, ASortedFilteredTreePromotesOrphansToRootsInSortedPlace)
{
    // 0 filtered out: its shown children 1 and 2 become roots, placed among the other roots by the
    // order, and 1 still has its own children under it, sorted.
    const auto snapshots = forest();
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    const std::unordered_set<std::uint64_t> collapsed = {105}; // 5 is collapsed: 6 is hidden
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {2, 4, 5, 1, 3, 6}, collapsed, scratch, rows);
    const std::vector<std::pair<std::size_t, int>> expected = {{2, 0}, {5, 0}, {1, 0}, {4, 1}, {3, 1}};
    EXPECT_EQ(shape(rows), expected);
    EXPECT_TRUE(rows[1].hasChildren); // collapsed, but still has a child to show
    EXPECT_FALSE(rows[1].isExpanded);
}

TEST(ProcessTreeFlattenTest, MalformedChildListsDrawEveryProcessOnce)
{
    // 0 lists 2 as its child, and so does 1; 1 lists itself; the order repeats 2. Each process is
    // drawn exactly once: 2 under 0 (the first parent in snapshot order), 1 as a root.
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(10, {2}), makeSnapshot(11, {1, 2}), makeSnapshot(12)};
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {1, 2, 0, 2}, {}, scratch, rows);
    const std::vector<std::pair<std::size_t, int>> expected = {{1, 0}, {0, 0}, {2, 1}};
    EXPECT_EQ(shape(rows), expected);
    EXPECT_FALSE(rows[0].hasChildren); // its only other child is drawn under 0
}

TEST(ProcessTreeFlattenTest, ABigSortedTreeKeepsEveryChildUnderItsParentInOrder)
{
    // 2000 processes in one tree (each one's parent an earlier process, a few with many children),
    // walked in a scrambled order. Every process is drawn once; each row's parent is the nearest row
    // above it one level up; and within each parent, the children come in the given order.
    constexpr std::size_t COUNT = 2000;
    std::vector<ProcessSnapshot> snapshots;
    snapshots.reserve(COUNT);
    std::vector<std::size_t> parentOf(COUNT, ProcessTreeFlatten::NOT_SHOWN);
    for (std::size_t i = 0; i < COUNT; ++i)
    {
        snapshots.push_back(makeSnapshot(1000 + i));
    }
    for (std::size_t i = 1; i < COUNT; ++i)
    {
        parentOf[i] = (i * 2'654'435'761U) % i;
        snapshots[parentOf[i]].childrenIndices.push_back(i);
    }
    std::vector<std::size_t> order(COUNT);
    std::vector<std::size_t> rank(COUNT);
    for (std::size_t pos = 0; pos < COUNT; ++pos)
    {
        order[pos] = (pos * 7919U) % COUNT; // 7919 is prime, so this visits every index once
        rank[order[pos]] = pos;
    }

    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, order, {}, scratch, rows);

    ASSERT_EQ(rows.size(), COUNT);
    std::vector<std::size_t> ancestors; // the row's ancestors, by depth
    std::vector<std::size_t> lastChildRank(COUNT, 0);
    std::vector<char> seen(COUNT, 0);
    for (const ProcessTreeRow& row : rows)
    {
        ASSERT_EQ(seen[row.procIdx], 0) << row.procIdx;
        seen[row.procIdx] = 1;
        ASSERT_LE(static_cast<std::size_t>(row.depth), ancestors.size());
        ancestors.resize(static_cast<std::size_t>(row.depth));
        if (row.depth == 0)
        {
            EXPECT_EQ(parentOf[row.procIdx], ProcessTreeFlatten::NOT_SHOWN) << row.procIdx;
        }
        else
        {
            const std::size_t parent = ancestors.back();
            ASSERT_EQ(parentOf[row.procIdx], parent) << row.procIdx;
            EXPECT_GT(rank[row.procIdx] + 1, lastChildRank[parent]) << "siblings out of order under " << parent;
            lastChildRank[parent] = rank[row.procIdx] + 1;
        }
        ancestors.push_back(row.procIdx);
    }
}

TEST(ProcessTreeRowsCacheTest, RebuildsOnlyWhenTheSnapshotFilterSortOrCollapseStateChanges)
{
    const auto snapshots = forest();
    std::vector<std::size_t> filtered = {0, 1, 2, 3, 4, 5, 6, 7};
    std::unordered_set<std::uint64_t> collapsed;
    ProcessTreeFlatten::ProcessTreeRowsCache cache;
    ProcessTreeFlatten::ProcessTreeRowsKey key{.snapshotVersion = 1, .filterGeneration = 1, .collapseGeneration = 0};

    EXPECT_EQ(cache.rows(key, snapshots, filtered, collapsed).size(), 8U);
    EXPECT_EQ(cache.rows(key, snapshots, filtered, collapsed).size(), 8U); // a later frame: no rebuild
    EXPECT_EQ(cache.buildCount(), 1U);

    // Collapse toggle: the panel advances collapseGeneration with the set.
    collapsed.insert(101);
    ++key.collapseGeneration;
    const std::vector<RowState> afterCollapse = {
        {0, 0, true, true}, {1, 1, true, false}, {2, 1, false, true}, {5, 0, true, true}, {6, 1, false, true}, {7, 0, false, true}};
    EXPECT_EQ(rowStates(cache.rows(key, snapshots, filtered, collapsed)), afterCollapse);
    EXPECT_EQ(cache.buildCount(), 2U);

    // Filter change: the panel advances filterGeneration when it rebuilds the indices.
    filtered = {5, 6, 7};
    ++key.filterGeneration;
    EXPECT_EQ(cache.rows(key, snapshots, filtered, collapsed).size(), 3U);
    EXPECT_EQ(cache.buildCount(), 3U);

    // New snapshot.
    ++key.snapshotVersion;
    std::ignore = cache.rows(key, snapshots, filtered, collapsed);
    EXPECT_EQ(cache.buildCount(), 4U);
    std::ignore = cache.rows(key, snapshots, filtered, collapsed);
    EXPECT_EQ(cache.buildCount(), 4U);

    // Re-sorted (#838): the panel advances sortGeneration with the order.
    filtered = {7, 6, 5};
    ++key.sortGeneration;
    EXPECT_EQ(cache.rows(key, snapshots, filtered, collapsed).front().procIdx, 7U);
    EXPECT_EQ(cache.buildCount(), 5U);

    cache.invalidate();
    std::ignore = cache.rows(key, snapshots, filtered, collapsed);
    EXPECT_EQ(cache.buildCount(), 6U);

    // A filter turned on shows the matches' ancestors (#838): the context is part of the key.
    key.context = ProcessTreeFlatten::FilterContext::WithAncestors;
    filtered = {6};
    EXPECT_EQ(cache.rows(key, snapshots, filtered, collapsed).size(), 2U); // 5, then 6 under it
    EXPECT_EQ(cache.buildCount(), 7U);
}

// =============================================================================
// A filtered tree's ancestors (#838): a match keeps its place under its dimmed ancestors
// =============================================================================

/// (procIdx, depth, isContext) of each row.
[[nodiscard]] std::vector<std::tuple<std::size_t, int, bool>> contextShape(const std::vector<ProcessTreeRow>& rows)
{
    std::vector<std::tuple<std::size_t, int, bool>> out;
    out.reserve(rows.size());
    for (const ProcessTreeRow& row : rows)
    {
        out.emplace_back(row.procIdx, row.depth, row.isContext);
    }
    return out;
}

TEST(ProcessTreeFlattenTest, AFilteredTreeShowsEachMatchUnderItsAncestors)
{
    // 0 -> {1 -> {3, 4}, 2}, 5 -> {6}, 7; the filter matches 3 and 6. Their ancestors 0, 1 and 5 are
    // shown as context; 2, 4 and 7 match nothing and lead to no match, so they stay hidden.
    const auto snapshots = forest();
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {3, 6}, {}, ProcessTreeFlatten::FilterContext::WithAncestors, scratch, rows);
    const std::vector<std::tuple<std::size_t, int, bool>> expected = {
        {0, 0, true}, {1, 1, true}, {3, 2, false}, {5, 0, true}, {6, 1, false}};
    EXPECT_EQ(contextShape(rows), expected);

    // Without the context the same matches are roots, as before (#838's old behaviour, still there).
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {3, 6}, {}, ProcessTreeFlatten::FilterContext::MatchesOnly, scratch, rows);
    const std::vector<std::tuple<std::size_t, int, bool>> roots = {{3, 0, false}, {6, 0, false}};
    EXPECT_EQ(contextShape(rows), roots);
}

TEST(ProcessTreeFlattenTest, AnAncestorThatMatchesIsNotContext)
{
    const auto snapshots = forest();
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {1, 3}, {}, ProcessTreeFlatten::FilterContext::WithAncestors, scratch, rows);
    const std::vector<std::tuple<std::size_t, int, bool>> expected = {{0, 0, true}, {1, 1, false}, {3, 2, false}};
    EXPECT_EQ(contextShape(rows), expected);
}

TEST(ProcessTreeFlattenTest, AContextAncestorSortsWhereItsFirstMatchDoes)
{
    // Sorted with 6 before 3: 5's branch (leading to 6) comes before 0's (leading to 3).
    const auto snapshots = forest();
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {6, 3}, {}, ProcessTreeFlatten::FilterContext::WithAncestors, scratch, rows);
    const std::vector<std::pair<std::size_t, int>> expected = {{5, 0}, {6, 1}, {0, 0}, {1, 1}, {3, 2}};
    EXPECT_EQ(shape(rows), expected);
}

TEST(ProcessTreeFlattenTest, AParentCycleEndsTheClimbToAncestors)
{
    // 0 and 1 list each other as children: a corrupt snapshot. The climb stops, and the build ends.
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(10, {1}), makeSnapshot(11, {0}), makeSnapshot(12)};
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {0, 2}, {}, ProcessTreeFlatten::FilterContext::WithAncestors, scratch, rows);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.back().procIdx, 2U); // the unrelated match is still drawn
    EXPECT_LE(rows.size(), 3U);
}

// =============================================================================
// Expand all / Collapse all, and exited processes' collapse state (#838)
// =============================================================================

TEST(ProcessTreeFlattenTest, CollapseAllCollapsesEveryParentSoOnlyRootsShow)
{
    const auto snapshots = forest();
    std::unordered_set<std::uint64_t> collapsed;
    EXPECT_TRUE(ProcessTreeFlatten::collapseAll(snapshots, collapsed));
    EXPECT_EQ(collapsed, (std::unordered_set<std::uint64_t>{100, 101, 105}));
    EXPECT_FALSE(ProcessTreeFlatten::collapseAll(snapshots, collapsed)); // already all collapsed

    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {0, 1, 2, 3, 4, 5, 6, 7}, collapsed, scratch, rows);
    const std::vector<std::pair<std::size_t, int>> roots = {{0, 0}, {5, 0}, {7, 0}};
    EXPECT_EQ(shape(rows), roots);

    // Expand all is an empty set: the whole tree again.
    collapsed.clear();
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {0, 1, 2, 3, 4, 5, 6, 7}, collapsed, scratch, rows);
    EXPECT_EQ(rows.size(), 8U);
}

TEST(ProcessTreeFlattenTest, PruningDropsOnlyTheKeysOfExitedProcesses)
{
    std::unordered_set<std::uint64_t> collapsed = {101, 105, 999};
    const std::unordered_set<std::uint64_t> live = {100, 101, 102, 105};
    EXPECT_TRUE(ProcessTreeFlatten::pruneCollapsedKeys(collapsed, live));
    EXPECT_EQ(collapsed, (std::unordered_set<std::uint64_t>{101, 105}));
    EXPECT_FALSE(ProcessTreeFlatten::pruneCollapsedKeys(collapsed, live)); // nothing left to drop
}

// =============================================================================
// The child-count badge and the tree's depth (#838)
// =============================================================================

/// procIdx -> childCount of each row.
[[nodiscard]] std::map<std::size_t, std::size_t> childCounts(const std::vector<ProcessTreeRow>& rows)
{
    std::map<std::size_t, std::size_t> counts;
    for (const ProcessTreeRow& row : rows)
    {
        counts.emplace(row.procIdx, row.childCount);
    }
    return counts;
}

TEST(ProcessTreeFlattenTest, EachRowCountsTheChildrenShownUnderItCollapsedOrNot)
{
    const auto snapshots = forest();
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {0, 1, 2, 3, 4, 5, 6, 7}, {101}, scratch, rows);
    const std::map<std::size_t, std::size_t> all = {{0, 2}, {1, 2}, {2, 0}, {5, 1}, {6, 0}, {7, 0}};
    EXPECT_EQ(childCounts(rows), all); // 101 is collapsed and still counts its two

    // Filtered, only the children shown are counted: 101's are filtered out, so it has none.
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, {0, 1, 2}, {}, scratch, rows);
    const std::map<std::size_t, std::size_t> filtered = {{0, 2}, {1, 0}, {2, 0}};
    EXPECT_EQ(childCounts(rows), filtered);
}

TEST(ProcessTreeRowsCacheTest, KnowsTheDeepestRowOfItsLastBuild)
{
    const auto snapshots = forest();
    const std::vector<std::size_t> order = {0, 1, 2, 3, 4, 5, 6, 7};
    std::unordered_set<std::uint64_t> collapsed;
    ProcessTreeFlatten::ProcessTreeRowsCache cache;
    ProcessTreeFlatten::ProcessTreeRowsKey key{.snapshotVersion = 1};
    EXPECT_EQ(cache.maxDepth(), 0);
    std::ignore = cache.rows(key, snapshots, order, collapsed);
    EXPECT_EQ(cache.maxDepth(), 2); // 103 and 104, under 101 under 100

    collapsed.insert(101); // their parent folded: only depth 1 is left in view
    ++key.collapseGeneration;
    std::ignore = cache.rows(key, snapshots, order, collapsed);
    EXPECT_EQ(cache.maxDepth(), 1);
}

// =============================================================================
// Regression (#838): the tree across real ProcessModel refreshes -- exited, reparented and zombie
// parents, PID reuse, and collapse state that has to survive them
// =============================================================================

constexpr std::uint64_t RSS = std::uint64_t{1} << 20U;

/// init (1) -> server (100) -> worker (101) -> helper (102), started in that order.
[[nodiscard]] std::vector<Platform::ProcessCounters> family()
{
    using TestMocks::makeProcessCounters;
    return {makeProcessCounters(1, "init", 'S', 10, 5, 100, RSS, 0),
            makeProcessCounters(100, "server", 'S', 10, 5, 1000, RSS, 1),
            makeProcessCounters(101, "worker", 'S', 10, 5, 1100, RSS, 100),
            makeProcessCounters(102, "helper", 'S', 10, 5, 1200, RSS, 101)};
}

/// A ProcessModel over a mock probe the test keeps a handle to, so each refresh can see other processes.
class Sampler
{
  public:
    Sampler()
    {
        auto owned = std::make_unique<TestMocks::MockProcessProbe>();
        m_Probe = owned.get();
        m_Model = std::make_unique<Domain::ProcessModel>(std::move(owned));
    }

    /// Samples @p processes and returns the snapshots they make.
    [[nodiscard]] std::vector<ProcessSnapshot> refresh(std::vector<Platform::ProcessCounters> processes)
    {
        m_Probe->setCounters(std::move(processes));
        m_Model->refresh();
        return m_Model->snapshots();
    }

  private:
    TestMocks::MockProcessProbe* m_Probe = nullptr;
    std::unique_ptr<Domain::ProcessModel> m_Model;
};

/// A row of the tree as (pid, depth, childCount).
using TreeRow = std::tuple<std::int32_t, int, std::size_t>;

/// The tree's rows for @p snapshots, every process shown in snapshot order, with @p collapsed folded.
[[nodiscard]] std::vector<TreeRow> treeOf(const std::vector<ProcessSnapshot>& snapshots,
                                          const std::unordered_set<std::uint64_t>& collapsed = {})
{
    std::vector<std::size_t> order(snapshots.size());
    std::ranges::iota(order, std::size_t{0});
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, order, collapsed, scratch, rows);
    std::vector<TreeRow> tree;
    tree.reserve(rows.size());
    for (const ProcessTreeRow& row : rows)
    {
        tree.emplace_back(snapshots[row.procIdx].pid, row.depth, row.childCount);
    }
    return tree;
}

[[nodiscard]] std::uint64_t keyOf(const std::vector<ProcessSnapshot>& snapshots, std::int32_t pid)
{
    const auto found = std::ranges::find(snapshots, pid, &ProcessSnapshot::pid);
    return found == snapshots.end() ? 0 : found->uniqueKey;
}

[[nodiscard]] std::unordered_set<std::uint64_t> liveKeys(const std::vector<ProcessSnapshot>& snapshots)
{
    std::unordered_set<std::uint64_t> keys;
    for (const ProcessSnapshot& snapshot : snapshots)
    {
        keys.insert(snapshot.uniqueKey);
    }
    return keys;
}

TEST(ProcessTreeRegressionTest, AChildWhoseParentExitsBecomesARootThenFollowsItsNewParent)
{
    Sampler sampler;
    const std::vector<TreeRow> whole = {{1, 0, 1}, {100, 1, 1}, {101, 2, 1}, {102, 3, 0}};
    EXPECT_EQ(treeOf(sampler.refresh(family())), whole);

    // server exits; worker still names it as its parent (Windows keeps a dead parent's PID), so worker
    // is a root, with helper still under it.
    auto orphaned = family();
    orphaned.erase(orphaned.begin() + 1);
    const std::vector<TreeRow> rooted = {{1, 0, 0}, {101, 0, 1}, {102, 1, 0}};
    EXPECT_EQ(treeOf(sampler.refresh(orphaned)), rooted);

    // Reparented to init, as Linux does: worker and its subtree move under init.
    orphaned[1].parentPid = 1;
    const std::vector<TreeRow> adopted = {{1, 0, 1}, {101, 1, 1}, {102, 2, 0}};
    EXPECT_EQ(treeOf(sampler.refresh(orphaned)), adopted);
}

TEST(ProcessTreeRegressionTest, AZombieChildStaysUnderItsParentUntilItIsReaped)
{
    Sampler sampler;
    auto processes = family();
    processes.pop_back();     // helper is gone
    processes[2].state = 'Z'; // worker has exited, and server has not reaped it yet
    const std::vector<TreeRow> zombie = {{1, 0, 1}, {100, 1, 1}, {101, 2, 0}};
    EXPECT_EQ(treeOf(sampler.refresh(processes)), zombie);

    processes.pop_back(); // reaped: server is a leaf again
    const std::vector<TreeRow> reaped = {{1, 0, 1}, {100, 1, 0}};
    EXPECT_EQ(treeOf(sampler.refresh(processes)), reaped);
}

TEST(ProcessTreeRegressionTest, ANewerProcessGivenAnExitedParentsPidDoesNotAdoptItsChildren)
{
    Sampler sampler;
    std::ignore = sampler.refresh(family());
    auto reused = family();
    reused[1] = TestMocks::makeProcessCounters(100, "newcomer", 'S', 10, 5, 5000, RSS, 1); // started after worker
    const std::vector<TreeRow> tree = {{1, 0, 1}, {100, 1, 0}, {101, 0, 1}, {102, 1, 0}};
    EXPECT_EQ(treeOf(sampler.refresh(reused)), tree);
}

TEST(ProcessTreeRegressionTest, ACollapsedNodeStaysCollapsedAcrossRefreshesButNotForTheNextOwnerOfItsPid)
{
    Sampler sampler;
    auto processes = family();
    std::vector<ProcessSnapshot> snapshots = sampler.refresh(processes);
    const std::uint64_t serverKey = keyOf(snapshots, 100);
    std::unordered_set<std::uint64_t> collapsed = {serverKey};
    const std::vector<TreeRow> folded = {{1, 0, 1}, {100, 1, 1}};
    for (int sample = 0; sample < 3; ++sample)
    {
        for (Platform::ProcessCounters& process : processes)
        {
            process.userTime += 50; // the same processes, busier: new snapshots, the same identities
        }
        snapshots = sampler.refresh(processes);
        EXPECT_FALSE(ProcessTreeFlatten::pruneCollapsedKeys(collapsed, liveKeys(snapshots))) << sample;
        EXPECT_EQ(treeOf(snapshots, collapsed), folded) << sample;
    }

    // server exits and a newcomer is given PID 100, with a child of its own. It is another process
    // (another uniqueKey), so it is not collapsed, and server's key is pruned.
    processes[1] = TestMocks::makeProcessCounters(100, "newcomer", 'S', 10, 5, 5000, RSS, 1);
    processes[2] = TestMocks::makeProcessCounters(103, "newchild", 'S', 10, 5, 5100, RSS, 100);
    processes.pop_back();
    snapshots = sampler.refresh(processes);
    EXPECT_NE(keyOf(snapshots, 100), serverKey);
    const std::vector<TreeRow> fresh = {{1, 0, 1}, {100, 1, 1}, {103, 2, 0}};
    EXPECT_EQ(treeOf(snapshots, collapsed), fresh);
    EXPECT_TRUE(ProcessTreeFlatten::pruneCollapsedKeys(collapsed, liveKeys(snapshots)));
    EXPECT_TRUE(collapsed.empty());
}

} // namespace
} // namespace App
