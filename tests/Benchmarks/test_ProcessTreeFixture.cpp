// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
// Tests for benchmarks/ProcessTreeFixture.h: the forest bench_ProcessTree.cpp times is a tree several
// levels deep, not the one-level star its old parent formula always made (#2976).
#include "App/Panels/ProcessTreeFlatten.h"
#include "ProcessTreeFixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <set>
#include <unordered_set>
#include <vector>

namespace BenchmarkUtils
{
namespace
{

using App::ProcessTreeFlatten::FilterContext;
using App::ProcessTreeFlatten::ProcessTreeRow;

struct ForestShape
{
    std::size_t rows = 0;
    std::size_t roots = 0;
    std::size_t parents = 0; // processes with at least one child
    int maxDepth = 0;
};

// The forest's shape as the benchmark's own call sees it: every process, in snapshot order.
ForestShape shapeOf(std::size_t count)
{
    const auto snapshots = makeProcessForest(count);
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    const std::unordered_set<std::uint64_t> collapsed;
    App::ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<ProcessTreeRow> rows;
    App::ProcessTreeFlatten::buildProcessTreeRows(snapshots, order, collapsed, scratch, rows);

    ForestShape shape;
    shape.rows = rows.size();
    for (const ProcessTreeRow& row : rows)
    {
        shape.roots += row.depth == 0 ? 1U : 0U;
        shape.parents += row.hasChildren ? 1U : 0U;
        shape.maxDepth = std::max(shape.maxDepth, row.depth);
    }
    return shape;
}

TEST(ProcessTreeFixtureTest, SplitMixMatchesTheReferenceOutput)
{
    // splitmix64 seeded with 0 first returns 0xE220A8397B1DCDAF (Vigna's reference implementation).
    EXPECT_EQ(splitmix64(0), 0xE220'A839'7B1D'CDAFULL);
}

TEST(ProcessTreeFixtureTest, EveryParentIsAnEarlierProcess)
{
    for (std::size_t i = PROCESS_FOREST_ROOTS; i < 10'000; ++i)
    {
        ASSERT_LT(processForestParent(i), i) << "process " << i;
    }
}

TEST(ProcessTreeFixtureTest, IsAForestSeveralLevelsDeepAtEveryBenchmarkedSize)
{
    for (const std::size_t count : {std::size_t{500}, std::size_t{2000}, std::size_t{10'000}})
    {
        SCOPED_TRACE(count);
        const ForestShape shape = shapeOf(count);
        EXPECT_EQ(shape.rows, count);
        EXPECT_EQ(shape.roots, PROCESS_FOREST_ROOTS);
        // The old formula made one parent (process 0) and depth 1 at every size.
        EXPECT_GT(shape.parents, count / 10);
        EXPECT_GT(shape.maxDepth, 2);
    }
}

TEST(ProcessTreeFixtureTest, ShapeIsTheSameOnEveryPlatform)
{
    // The figures ProcessTreeFixture.h documents; integer math only, so no platform may differ.
    EXPECT_EQ(shapeOf(500).parents, 173U);
    EXPECT_EQ(shapeOf(500).maxDepth, 6);
    EXPECT_EQ(shapeOf(2000).parents, 683U);
    EXPECT_EQ(shapeOf(2000).maxDepth, 8);
    EXPECT_EQ(shapeOf(10'000).parents, 3463U);
    EXPECT_EQ(shapeOf(10'000).maxDepth, 10);
}

TEST(ProcessTreeFixtureTest, FilteredMatchesHaveAncestorsAtSeveralLevels)
{
    // BM_ProcessTree_BuildRows_FilteredWithAncestors' input: one process in 50 matches.
    for (const std::size_t count : {std::size_t{2000}, std::size_t{10'000}})
    {
        SCOPED_TRACE(count);
        const auto snapshots = makeProcessForest(count);
        std::vector<std::size_t> matches;
        for (std::size_t i = 0; i < count; i += 50)
        {
            matches.push_back(i);
        }
        const std::unordered_set<std::uint64_t> collapsed;
        App::ProcessTreeFlatten::TreeFlattenScratch scratch;
        std::vector<ProcessTreeRow> rows;
        App::ProcessTreeFlatten::buildProcessTreeRows(snapshots, matches, collapsed, FilterContext::WithAncestors, scratch, rows);

        std::set<int> matchDepths;
        std::set<int> contextDepths;
        for (const ProcessTreeRow& row : rows)
        {
            (row.isContext ? contextDepths : matchDepths).insert(row.depth);
        }
        // Matches two or more levels down, so their ancestors are a chain, not just a root.
        EXPECT_GT(matchDepths.size(), 3U);
        EXPECT_GT(*matchDepths.rbegin(), 2);
        EXPECT_GT(contextDepths.size(), 2U);
    }
}

} // namespace
} // namespace BenchmarkUtils
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
