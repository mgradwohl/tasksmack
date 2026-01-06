// Benchmarks for Domain/ProcessModel
//
// These benchmarks measure the performance of process enumeration and snapshot
// computation, which are the most frequently executed operations in the app.
// Memory tracking is included to catch allocation regressions.

#include "Domain/ProcessModel.h"
#include "MemoryTracker.h"
#include "Platform/Factory.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#include <benchmark/benchmark.h>

namespace
{

// Benchmark real process enumeration via platform probe
// This measures actual OS API performance and memory allocation
static void BM_ProcessProbe_Enumerate(benchmark::State& state)
{
    auto probe = Platform::makeProcessProbe();

    BenchmarkUtils::MemoryDeltaTracker memTracker;

    for (auto _ : state)
    {
        auto processes = probe->enumerate();
        benchmark::DoNotOptimize(processes.data());
        benchmark::DoNotOptimize(processes.size());
    }

    // Report process count for context
    auto finalEnumerate = probe->enumerate();
    state.counters["processes"] = benchmark::Counter(static_cast<double>(finalEnumerate.size()));

    // Report memory usage
    BenchmarkUtils::reportMemoryCounters(state);
    BenchmarkUtils::reportMemoryDelta(state, memTracker);
}
BENCHMARK(BM_ProcessProbe_Enumerate);

// Benchmark ProcessModel refresh (full pipeline) with memory tracking
static void BM_ProcessModel_Refresh(benchmark::State& state)
{
    auto probe = Platform::makeProcessProbe();
    Domain::ProcessModel model(std::move(probe));

    // Initial refresh to populate previous counters
    model.refresh();

    BenchmarkUtils::MemoryDeltaTracker memTracker;

    for (auto _ : state)
    {
        model.refresh();
        benchmark::DoNotOptimize(model.processCount());
    }

    state.counters["processes"] = benchmark::Counter(static_cast<double>(model.processCount()));

    // Report memory - this is key for detecting allocation bloat
    BenchmarkUtils::reportMemoryCounters(state);
    BenchmarkUtils::reportMemoryDelta(state, memTracker);
}
BENCHMARK(BM_ProcessModel_Refresh);

// Benchmark snapshot access (read-only, should be very fast)
static void BM_ProcessModel_GetSnapshots(benchmark::State& state)
{
    auto probe = Platform::makeProcessProbe();
    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    for (auto _ : state)
    {
        const auto& snapshots = model.snapshots();
        benchmark::DoNotOptimize(snapshots.data());
        benchmark::DoNotOptimize(snapshots.size());
    }
}
BENCHMARK(BM_ProcessModel_GetSnapshots);

// Benchmark process lookup by PID (linear search through snapshots)
// This simulates finding a specific process in the list
static void BM_ProcessModel_FindByPid(benchmark::State& state)
{
    auto probe = Platform::makeProcessProbe();
    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Get a real PID to search for
    const auto snapshots = model.snapshots();
    if (snapshots.empty())
    {
        state.SkipWithError("No processes found");
        return;
    }

    // Intentionally use integer division to select the middle snapshot PID for lookup benchmarking
    const auto targetPid = snapshots[snapshots.size() / 2].pid;

    for (auto _ : state)
    {
        // Linear search through snapshots using std::ranges
        const auto currentSnapshots = model.snapshots();
        auto it = std::ranges::find_if(currentSnapshots, [targetPid](const auto& snap) { return snap.pid == targetPid; });
        // Use boolean instead of pointer to avoid dangling reference to temporary vector
        const bool found = (it != currentSnapshots.end());
        benchmark::DoNotOptimize(found);
    }
}
BENCHMARK(BM_ProcessModel_FindByPid);

// Benchmark memory allocation during repeated refreshes
// This tracks how much memory grows over many refresh cycles
static void BM_ProcessModel_MemoryGrowth(benchmark::State& state)
{
    auto probe = Platform::makeProcessProbe();
    Domain::ProcessModel model(std::move(probe));

    auto startStats = BenchmarkUtils::readMemoryStats();

    // Run many refresh cycles
    for (auto _ : state)
    {
        model.refresh();
    }

    auto endStats = BenchmarkUtils::readMemoryStats();

    // Report memory growth metrics
    state.counters["processes"] = benchmark::Counter(static_cast<double>(model.processCount()));

    if (startStats.valid() && endStats.valid())
    {
        auto rssGrowth = static_cast<std::int64_t>(endStats.vmRSS) - static_cast<std::int64_t>(startStats.vmRSS);
        auto heapGrowth = static_cast<std::int64_t>(endStats.vmData) - static_cast<std::int64_t>(startStats.vmData);

        state.counters["rss_growth_kb"] = benchmark::Counter(static_cast<double>(rssGrowth) / 1024.0);
        state.counters["heap_growth_kb"] = benchmark::Counter(static_cast<double>(heapGrowth) / 1024.0);
        state.counters["final_rss_mb"] = benchmark::Counter(static_cast<double>(endStats.vmRSS) / (1024.0 * 1024.0));

        // Per-iteration memory (should be ~0 for stable implementation)
        if (state.iterations() > 0)
        {
            state.counters["bytes_per_iter"] = benchmark::Counter(static_cast<double>(rssGrowth) / static_cast<double>(state.iterations()));
        }
    }
}
BENCHMARK(BM_ProcessModel_MemoryGrowth)->Iterations(1000);

// Benchmark with parameterized refresh interval simulation
// Simulates different sampling rates
static void BM_ProcessModel_RefreshRate(benchmark::State& state)
{
    auto probe = Platform::makeProcessProbe();
    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto delayMilliseconds = state.range(0);

    for (auto _ : state)
    {
        // Simulate refresh at different rates
        model.refresh();

        // Add artificial delay to simulate real-world sampling
        if (delayMilliseconds > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMilliseconds));
        }

        benchmark::DoNotOptimize(model.processCount());
    }

    // Calculate rate in Hz (0ms delay is clamped to 1ms to avoid division by zero)
    state.counters["rate_hz"] = benchmark::Counter(1000.0 / static_cast<double>(std::max(delayMilliseconds, 1L)));
}
// Test 0ms (as fast as possible), 100ms (10Hz), 500ms (2Hz), 1000ms (1Hz)
BENCHMARK(BM_ProcessModel_RefreshRate)->Arg(0)->Arg(100)->Arg(500)->Arg(1000)->Unit(benchmark::kMillisecond);

} // namespace
