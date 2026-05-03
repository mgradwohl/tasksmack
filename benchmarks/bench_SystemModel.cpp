// Benchmarks for Domain/SystemModel
//
// These benchmarks measure the performance of system metric sampling and
// snapshot computation, which are called from the UI/main thread update cycle.
// Memory tracking is included to catch allocation regressions.

#include "Domain/SamplingConfig.h"
#include "Domain/SystemModel.h"
#include "MemoryTracker.h"
#include "Platform/Factory.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace
{

// Benchmark raw system probe sample (kernel-facing call)
// This measures actual OS API performance
static void BM_SystemProbe_Sample(benchmark::State& state)
{
    auto probe = Platform::makeSystemProbe();

    BenchmarkUtils::MemoryDeltaTracker memTracker;

    for (auto _ : state)
    {
        auto counters = probe->read();
        benchmark::DoNotOptimize(counters.cpuTotal.user);
    }

    BenchmarkUtils::reportMemoryCounters(state);
    BenchmarkUtils::reportMemoryDelta(state, memTracker);
}
BENCHMARK(BM_SystemProbe_Sample);

// Benchmark SystemModel refresh (full pipeline including delta computation)
// This is called from the UI/main-thread update cycle (not a background thread)
static void BM_SystemModel_Refresh(benchmark::State& state)
{
    auto probe = Platform::makeSystemProbe();
    Domain::SystemModel model(std::move(probe));

    // Prime the previous-counters state so deltas are computed on the first benchmarked call
    model.refresh();

    BenchmarkUtils::MemoryDeltaTracker memTracker;

    for (auto _ : state)
    {
        model.refresh();
        benchmark::DoNotOptimize(model);
    }

    BenchmarkUtils::reportMemoryCounters(state);
    BenchmarkUtils::reportMemoryDelta(state, memTracker);
}
BENCHMARK(BM_SystemModel_Refresh);

// Benchmark snapshot() – read-only copy returned to the UI thread
// This should be very fast (shared_mutex read lock + copy)
static void BM_SystemModel_Snapshot(benchmark::State& state)
{
    auto probe = Platform::makeSystemProbe();
    Domain::SystemModel model(std::move(probe));
    model.refresh();

    for (auto _ : state)
    {
        auto snap = model.snapshot();
        benchmark::DoNotOptimize(snap.cpuTotal.totalPercent);
    }
}
BENCHMARK(BM_SystemModel_Snapshot);

// Benchmark history accessors used by the UI for graph rendering
// Each accessor acquires a shared_mutex read lock and copies a deque
static void BM_SystemModel_CpuHistory(benchmark::State& state)
{
    auto probe = Platform::makeSystemProbe();
    Domain::SystemModel model(std::move(probe));

    // Populate history with multiple refresh calls
    for (int i = 0; i < 10; ++i)
    {
        model.refresh();
    }

    for (auto _ : state)
    {
        auto hist = model.cpuHistory();
        benchmark::DoNotOptimize(hist.data());
        benchmark::DoNotOptimize(hist.size());
    }
}
BENCHMARK(BM_SystemModel_CpuHistory);

// Benchmark perCoreHistory() – most expensive history accessor:
// returns a vector<vector<float>>, one inner vector per logical CPU core
static void BM_SystemModel_PerCoreHistory(benchmark::State& state)
{
    auto probe = Platform::makeSystemProbe();
    Domain::SystemModel model(std::move(probe));

    for (int i = 0; i < 10; ++i)
    {
        model.refresh();
    }

    for (auto _ : state)
    {
        auto hist = model.perCoreHistory();
        benchmark::DoNotOptimize(hist.data());
        benchmark::DoNotOptimize(hist.size());
    }
}
BENCHMARK(BM_SystemModel_PerCoreHistory);

// Benchmark memory growth over many refresh() cycles.
// Exercises the full production code path: probe read, delta computation,
// and history append. History accumulates monotonically across iterations
// because 500 tight back-to-back calls do not span enough wall-clock time
// to trigger trimming. Reports RSS and heap growth so regressions are
// visible in benchmark output.
static void BM_SystemModel_MemoryGrowth(benchmark::State& state)
{
    auto probe = Platform::makeSystemProbe();
    Domain::SystemModel model(std::move(probe));

    // Prime previous-counters state so the first benchmarked call computes valid deltas
    model.refresh();

    auto startStats = BenchmarkUtils::readMemoryStats();

    for (auto _ : state)
    {
        model.refresh();
        benchmark::DoNotOptimize(model);
    }

    auto endStats = BenchmarkUtils::readMemoryStats();

    if (startStats.valid() && endStats.valid())
    {
        auto rssGrowth = (static_cast<std::int64_t>(endStats.vmRSS)) - (static_cast<std::int64_t>(startStats.vmRSS));
        auto heapGrowth = (static_cast<std::int64_t>(endStats.vmData)) - (static_cast<std::int64_t>(startStats.vmData));

        state.counters["rss_growth_kb"] = benchmark::Counter((static_cast<double>(rssGrowth)) / 1024.0);
        state.counters["heap_growth_kb"] = benchmark::Counter((static_cast<double>(heapGrowth)) / 1024.0);
        state.counters["final_rss_mb"] = benchmark::Counter((static_cast<double>(endStats.vmRSS)) / (1024.0 * 1024.0));

        if (state.iterations() > 0)
        {
            state.counters["bytes_per_iter"] =
                benchmark::Counter((static_cast<double>(rssGrowth)) / (static_cast<double>(state.iterations())));
        }
    }
}
BENCHMARK(BM_SystemModel_MemoryGrowth)->Iterations(500);

} // namespace
