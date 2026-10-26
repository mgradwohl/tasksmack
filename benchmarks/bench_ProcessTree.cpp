// Cost of building the Processes table's tree-view rows (#838, #1138).
//
// ProcessTreeFlatten::buildProcessTreeRows() turns the filtered processes into the flattened,
// render-ordered rows the tree view's ImGuiListClipper draws. It runs once per adopted snapshot,
// filter change, sort change or fold, not per frame, but it is the whole tree's cost when it does.
// NaturalOrder walks the processes in snapshot order (tree view before #838); SortedOrder walks them
// in a sorted order, so each parent's children come out sorted too (#838). FilteredWithAncestors builds a
// filtered tree that also shows each match's ancestors (#838), and ProcessFilter_Indices is the filter
// pass itself: every process's name and command line searched for the term (#838).
//
// Every benchmark here runs on BenchmarkUtils::makeProcessForest() (ProcessTreeFixture.h): four roots
// and most processes 2-6 levels deep, a shape tests/Benchmarks/test_ProcessTreeFixture.cpp pins (#2976).

#include "App/Panels/ProcessFilterCache.h"
#include "App/Panels/ProcessTreeFlatten.h"
#include "Domain/ProcessSnapshot.h"
#include "ProcessTreeFixture.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numeric>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{

void runBuild(benchmark::State& state, bool sorted)
{
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto snapshots = BenchmarkUtils::makeProcessForest(count);
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

// One process in 50 matches; their ancestors are added as context.
void BM_ProcessTree_BuildRows_FilteredWithAncestors(benchmark::State& state)
{
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto snapshots = BenchmarkUtils::makeProcessForest(count);
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < count; i += 50)
    {
        matches.push_back(i);
    }
    const std::unordered_set<std::uint64_t> collapsed;
    App::ProcessTreeFlatten::TreeFlattenScratch scratch;
    std::vector<App::ProcessTreeFlatten::ProcessTreeRow> rows;

    for (auto _ : state)
    {
        App::ProcessTreeFlatten::buildProcessTreeRows(
            snapshots, matches, collapsed, App::ProcessTreeFlatten::FilterContext::WithAncestors, scratch, rows);
        benchmark::DoNotOptimize(rows.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

// The filter pass over @p range(0) processes with Windows-like command lines. range(1) 0: a term in
// no name or command line (every byte is searched); 1: one in every command line.
void BM_ProcessFilter_Indices(benchmark::State& state)
{
    const auto count = static_cast<std::size_t>(state.range(0));
    auto snapshots = BenchmarkUtils::makeProcessForest(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        snapshots[i].name = std::format("process-{}.exe", i);
        snapshots[i].command = std::format(
            R"("C:\Program Files\Vendor Name\Product\process-{}.exe" --type=renderer --lang=en-US --field-trial-handle={})", i, i * 7919U);
    }
    const std::string_view term = state.range(1) == 0 ? "service" : "renderer";
    std::vector<std::size_t> out;
    out.reserve(count);

    for (auto _ : state)
    {
        out.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            if (App::ProcessFilterCache::matches(snapshots[i].name, snapshots[i].command, term))
            {
                out.push_back(i);
            }
        }
        benchmark::DoNotOptimize(out.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

} // namespace

BENCHMARK(BM_ProcessTree_BuildRows_NaturalOrder)->Arg(500)->Arg(2000)->Arg(10000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessTree_BuildRows_SortedOrder)->Arg(500)->Arg(2000)->Arg(10000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessTree_BuildRows_FilteredWithAncestors)->Arg(2000)->Arg(10000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessFilter_Indices)->Args({2000, 0})->Args({2000, 1})->Unit(benchmark::kMicrosecond);
