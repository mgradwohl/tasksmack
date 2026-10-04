#include "App/Panels/ProcessTreeFlatten.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Domain::ProcessSnapshot;
using ProcessTreeFlatten::collectProcessTreeRows;
using ProcessTreeFlatten::ProcessTreeRow;

/// Builds a minimal snapshot for tree-flattening tests: only uniqueKey and childrenIndices are
/// read by collectProcessTreeRows(), so every other ProcessSnapshot field is left default.
[[nodiscard]] ProcessSnapshot makeSnapshot(std::uint64_t uniqueKey, std::vector<std::size_t> childrenIndices = {})
{
    ProcessSnapshot snap;
    snap.uniqueKey = uniqueKey;
    snap.childrenIndices = std::move(childrenIndices);
    return snap;
}

/// Every index in `snapshots` passes the filter -- the common case for these tests.
[[nodiscard]] std::unordered_set<std::size_t> allIndices(std::size_t count)
{
    std::unordered_set<std::size_t> result;
    for (std::size_t i = 0; i < count; ++i)
    {
        result.insert(i);
    }
    return result;
}

// =============================================================================
// Pre-order traversal
// =============================================================================

TEST(ProcessTreeFlattenTest, SingleNodeWithNoChildrenProducesOneRow)
{
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(1)};
    std::vector<ProcessTreeRow> rows;

    collectProcessTreeRows(snapshots, allIndices(1), {}, 0, 0, rows);

    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_EQ(rows[0].depth, 0);
    EXPECT_FALSE(rows[0].hasChildren);
    EXPECT_TRUE(rows[0].isExpanded); // not in collapsedKeys
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
    std::vector<ProcessTreeRow> rows;

    collectProcessTreeRows(snapshots, allIndices(4), {}, 0, 0, rows);

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

TEST(ProcessTreeFlattenTest, OutRowsAccumulatesAcrossMultipleRootCalls)
{
    // renderTreeView() calls collectProcessTreeRows() once per top-level root, all sharing one
    // `rows` vector -- a regression that cleared or overwrote `outRows` would silently drop
    // every root after the first.
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(1), makeSnapshot(2)};
    std::vector<ProcessTreeRow> rows;

    collectProcessTreeRows(snapshots, allIndices(2), {}, 0, 0, rows);
    collectProcessTreeRows(snapshots, allIndices(2), {}, 1, 0, rows);

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_EQ(rows[1].procIdx, 1U);
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
    std::vector<ProcessTreeRow> rows;

    collectProcessTreeRows(snapshots, allIndices(2), collapsedKeys, 0, 0, rows);

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
    std::vector<ProcessTreeRow> rows;

    collectProcessTreeRows(snapshots, allIndices(4), collapsedKeys, 0, 0, rows);

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
    const std::unordered_set<std::size_t> filteredSet = {0}; // idx1 does not pass the filter
    std::vector<ProcessTreeRow> rows;

    collectProcessTreeRows(snapshots, filteredSet, {}, 0, 0, rows);

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
    const std::unordered_set<std::size_t> filteredSet = {0, 1};
    std::vector<ProcessTreeRow> rows;

    collectProcessTreeRows(snapshots, filteredSet, {}, 0, 0, rows);

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].procIdx, 0U);
    EXPECT_TRUE(rows[0].hasChildren);
    EXPECT_EQ(rows[1].procIdx, 1U);
}

// =============================================================================
// Depth
// =============================================================================

TEST(ProcessTreeFlattenTest, StartingDepthOffsetsEveryRow)
{
    // renderTreeView() always starts a root descent at depth 0, but collectProcessTreeRows()
    // itself takes an explicit starting depth -- verify it's honored and propagated to children.
    const std::vector<ProcessSnapshot> snapshots = {makeSnapshot(10, {1}), makeSnapshot(20)};
    std::vector<ProcessTreeRow> rows;

    collectProcessTreeRows(snapshots, allIndices(2), {}, 0, 5, rows);

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].depth, 5);
    EXPECT_EQ(rows[1].depth, 6);
}

// =============================================================================
// Whole-tree build and its cache (#1138)
// =============================================================================

/// The rows renderTreeView() used to build every frame: a hash set of the filtered indices, a second
/// one of the filtered children, then one collectProcessTreeRows() walk per root. buildProcessTreeRows()
/// must produce exactly these.
[[nodiscard]] std::vector<ProcessTreeRow> referenceTreeRows(const std::vector<ProcessSnapshot>& snapshots,
                                                            const std::vector<std::size_t>& filteredIndices,
                                                            const std::unordered_set<std::uint64_t>& collapsedKeys)
{
    const std::unordered_set<std::size_t> filteredSet(filteredIndices.begin(), filteredIndices.end());
    std::unordered_set<std::size_t> isChild;
    for (const std::size_t idx : filteredIndices)
    {
        for (const std::size_t child : snapshots[idx].childrenIndices)
        {
            if (filteredSet.contains(child))
            {
                isChild.insert(child);
            }
        }
    }
    std::vector<ProcessTreeRow> rows;
    for (const std::size_t idx : filteredIndices)
    {
        if (!isChild.contains(idx))
        {
            collectProcessTreeRows(snapshots, filteredSet, collapsedKeys, idx, 0, rows);
        }
    }
    return rows;
}

void expectSameRows(const std::vector<ProcessTreeRow>& actual, const std::vector<ProcessTreeRow>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        EXPECT_EQ(actual[i].procIdx, expected[i].procIdx) << "row " << i;
        EXPECT_EQ(actual[i].depth, expected[i].depth) << "row " << i;
        EXPECT_EQ(actual[i].hasChildren, expected[i].hasChildren) << "row " << i;
        EXPECT_EQ(actual[i].isExpanded, expected[i].isExpanded) << "row " << i;
    }
}

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

TEST(ProcessTreeFlattenTest, WholeTreeBuildMatchesThePerFrameWalkItReplaces)
{
    const auto snapshots = forest();
    ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;

    const std::vector<std::size_t> all = {0, 1, 2, 3, 4, 5, 6, 7};
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, all, {}, scratch, rows);
    expectSameRows(rows, referenceTreeRows(snapshots, all, {}));
    ASSERT_EQ(rows.size(), 8U);

    // A filtered-out parent promotes its filtered children to roots; a collapsed node hides its own.
    const std::vector<std::size_t> filtered = {1, 3, 4, 5, 6, 7};
    const std::unordered_set<std::uint64_t> collapsed = {105};
    ProcessTreeFlatten::buildProcessTreeRows(snapshots, filtered, collapsed, scratch, rows);
    expectSameRows(rows, referenceTreeRows(snapshots, filtered, collapsed));
    ASSERT_EQ(rows.size(), 5U); // 1, 3, 4, 5 (collapsed: 6 hidden), 7
    EXPECT_TRUE(rows[3].hasChildren);
    EXPECT_FALSE(rows[3].isExpanded);
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

TEST(ProcessTreeRowsCacheTest, RebuildsOnlyWhenTheSnapshotFilterOrCollapseStateChanges)
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
    const auto& afterCollapse = cache.rows(key, snapshots, filtered, collapsed);
    expectSameRows(afterCollapse, referenceTreeRows(snapshots, filtered, collapsed));
    EXPECT_EQ(afterCollapse.size(), 6U);
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

    cache.invalidate();
    std::ignore = cache.rows(key, snapshots, filtered, collapsed);
    EXPECT_EQ(cache.buildCount(), 5U);
}

} // namespace
} // namespace App
