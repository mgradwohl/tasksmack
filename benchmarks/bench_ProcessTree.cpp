// Cost of building the Processes table's tree-view rows (#838, #1138).
//
// ProcessTreeFlatten::buildProcessTreeRows() turns the filtered processes into the flattened,
// render-ordered rows the tree view's ImGuiListClipper draws. It runs once per adopted snapshot,
// filter change, sort change or fold, not per frame, but it is the whole tree's cost when it does.
// NaturalOrder walks the processes in snapshot order (tree view before #838); SortedOrder walks them
// in a sorted order, so each parent's children come out sorted too (#838).

#include "App/Panels/ProcessTreeFlatten.h"
#include "Domain/ProcessSnapshot.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <unordered_set>
#include <vector>

namespace
{

/// @p count processes in one tree of a realistic shape: each process's parent is an earlier one,
/// picked by a multiplicative hash, so a few processes have many children and most have none.
std::vector<Domain::ProcessSnapshot> makeForest(std::size_t count)
{
    std::vector<Domain::ProcessSnapshot> snapshots(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        snapshots[i].pid = static_cast<std::int32_t>(i + 1);
        snapshots[i].uniqueKey = i + 1;
        snapshots[i].cpuPercent = static_cast<double>((i * 37U) % 101U);
    }
    for (std::size_t i = 1; i < count; ++i)
    {
        const std::size_t parent = (i * 2'654'435'761U) % i;
        snapshots[parent].childrenIndices.push_back(i);
    }
    return snapshots;
}

void runBuild(benchmark::State& state, bool sorted)
{
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto snapshots = makeForest(count);
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    if (sorted)
    {
        std::ranges::sort(
            order,
            [&snapshots](std::size_t a, std::size_t b)
            { return snapshots[a].cpuPercent > snapshots[b].cpuPercent || (snapshots[a].cpuPercent == snapshots[b].cpuPercent && a < b); });
    }
    const std::unordered_set<std::uint64_t> collapsed;
    App::ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<App::ProcessTreeFlatten::ProcessTreeRow> rows;

    for (auto _ : state)
    {
        App::ProcessTreeFlatten::buildProcessTreeRows(snapshots, order, collapsed, scratch, rows);
        benchmark::DoNotOptimize(rows.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void BM_ProcessTree_BuildRows_NaturalOrder(benchmark::State& state)
{
    runBuild(state, /*sorted=*/false);
}

void BM_ProcessTree_BuildRows_SortedOrder(benchmark::State& state)
{
    runBuild(state, /*sorted=*/true);
}

} // namespace

BENCHMARK(BM_ProcessTree_BuildRows_NaturalOrder)->Arg(500)->Arg(2000)->Arg(10000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessTree_BuildRows_SortedOrder)->Arg(500)->Arg(2000)->Arg(10000)->Unit(benchmark::kMicrosecond);
