#include "App/Panels/ProcessTreeFlatten.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
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

} // namespace
} // namespace App
