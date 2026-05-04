// Benchmarks for Domain/StorageModel
//
// These benchmarks measure the performance of disk I/O sampling and
// snapshot computation. StorageModel aggregates per-device counters into
// rates and maintains history for charting.
// Memory tracking is included to catch allocation regressions.

#include "Domain/StorageModel.h"
#include "MemoryTracker.h"
#include "Platform/Factory.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace
{

// Benchmark raw disk probe sample (kernel-facing call)
// This measures actual OS API performance for disk I/O counters
static void BM_DiskProbe_Read(benchmark::State& state)
{
    auto probe = Platform::makeDiskProbe();

    BenchmarkUtils::MemoryDeltaTracker memTracker;

    for (auto _ : state)
    {
        auto counters = probe->read();
        benchmark::DoNotOptimize(counters.disks.size());
    }

    // Report disk count for context
    auto finalCounters = probe->read();
    state.counters["disks"] = benchmark::Counter(static_cast<double>(finalCounters.disks.size()));

    BenchmarkUtils::reportMemoryCounters(state);
    BenchmarkUtils::reportMemoryDelta(state, memTracker);
}
BENCHMARK(BM_DiskProbe_Read);

// Benchmark StorageModel::sample() – full pipeline including delta computation
// and history append. Currently called from the update loop on the main thread
// (BackgroundSampler exists but is not yet active in production).
static void BM_StorageModel_Sample(benchmark::State& state)
{
    auto probe = Platform::makeDiskProbe();
    Domain::StorageModel model(std::move(probe));

    // Prime previous-state so the first benchmarked call computes valid deltas
    model.sample();

    BenchmarkUtils::MemoryDeltaTracker memTracker;

    for (auto _ : state)
    {
        model.sample();
        benchmark::DoNotOptimize(model);
    }

    BenchmarkUtils::reportMemoryCounters(state);
    BenchmarkUtils::reportMemoryDelta(state, memTracker);
}
BENCHMARK(BM_StorageModel_Sample);

// Benchmark latestSnapshot() – read-only copy returned to the UI thread.
// Acquires a shared_mutex read lock and returns StorageSnapshot by value,
// which includes a deep copy of the per-disk vector.
static void BM_StorageModel_LatestSnapshot(benchmark::State& state)
{
    auto probe = Platform::makeDiskProbe();
    Domain::StorageModel model(std::move(probe));
    model.sample();

    for (auto _ : state)
    {
        auto snap = model.latestSnapshot();
        benchmark::DoNotOptimize(snap.totalReadBytesPerSec);
        benchmark::DoNotOptimize(snap.totalWriteBytesPerSec);
    }
}
BENCHMARK(BM_StorageModel_LatestSnapshot);

// Benchmark historyTimestamps() – returns the shared timestamp axis used by
// all chart series. Called once per frame by StorageSection before reading
// any per-disk or aggregate series.
static void BM_StorageModel_HistoryTimestamps(benchmark::State& state)
{
    auto probe = Platform::makeDiskProbe();
    Domain::StorageModel model(std::move(probe));

    // Populate history with multiple sample calls
    for (int i = 0; i < 10; ++i)
    {
        model.sample();
    }

    for (auto _ : state)
    {
        auto ts = model.historyTimestamps();
        benchmark::DoNotOptimize(ts.data());
        benchmark::DoNotOptimize(ts.size());
    }
}
BENCHMARK(BM_StorageModel_HistoryTimestamps);

// Benchmark totalReadHistory() and totalWriteHistory()
// Used by SystemMetricsPanel to render aggregate I/O charts
static void BM_StorageModel_TotalRateHistory(benchmark::State& state)
{
    auto probe = Platform::makeDiskProbe();
    Domain::StorageModel model(std::move(probe));

    for (int i = 0; i < 10; ++i)
    {
        model.sample();
    }

    for (auto _ : state)
    {
        auto readHist = model.totalReadHistory();
        auto writeHist = model.totalWriteHistory();
        benchmark::DoNotOptimize(readHist.data());
        benchmark::DoNotOptimize(writeHist.data());
    }
}
BENCHMARK(BM_StorageModel_TotalRateHistory);

// Benchmark perDiskHistory() – most expensive history accessor:
// returns a vector<PerDiskHistory>, one entry per disk device
static void BM_StorageModel_PerDiskHistory(benchmark::State& state)
{
    auto probe = Platform::makeDiskProbe();
    Domain::StorageModel model(std::move(probe));

    for (int i = 0; i < 10; ++i)
    {
        model.sample();
    }

    for (auto _ : state)
    {
        auto hist = model.perDiskHistory();
        benchmark::DoNotOptimize(hist.data());
        benchmark::DoNotOptimize(hist.size());
    }
}
BENCHMARK(BM_StorageModel_PerDiskHistory);

// Benchmark memory growth over many sample() cycles.
// Exercises the full production code path: probe read, delta computation,
// history append, and trimming. Reports RSS and heap growth so regressions
// are visible in benchmark output.
static void BM_StorageModel_MemoryGrowth(benchmark::State& state)
{
    auto probe = Platform::makeDiskProbe();
    Domain::StorageModel model(std::move(probe));

    // Prime previous-state
    model.sample();

    auto startStats = BenchmarkUtils::readMemoryStats();

    for (auto _ : state)
    {
        model.sample();
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
BENCHMARK(BM_StorageModel_MemoryGrowth)->Iterations(500);

// Benchmark the combined copy overhead of all four StorageSection history accessors
// called on every render frame: historyTimestamps(), totalReadHistory(),
// totalWriteHistory(), and perDiskHistory(). History depth is controlled by
// varying the number of pre-seeded samples (trimming is disabled so the full
// depth is retained).
static void BM_StorageModel_HistoryCopyOverhead(benchmark::State& state)
{
    const auto sampleCount = static_cast<int>(state.range(0));

    auto probe = Platform::makeDiskProbe();
    Domain::StorageModel model(std::move(probe));

    // Keep all samples for the full duration of this benchmark
    model.setMaxHistorySeconds(3600.0);

    // Pre-seed the desired number of snapshots
    for (int i = 0; i < sampleCount; ++i)
    {
        model.sample();
    }

    for (auto _ : state)
    {
        auto ts = model.historyTimestamps();
        auto readHist = model.totalReadHistory();
        auto writeHist = model.totalWriteHistory();
        auto perDisk = model.perDiskHistory();
        benchmark::DoNotOptimize(ts.data());
        benchmark::DoNotOptimize(readHist.data());
        benchmark::DoNotOptimize(writeHist.data());
        benchmark::DoNotOptimize(perDisk.data());
    }

    // Report actual history depth
    auto ts = model.historyTimestamps();
    state.counters["history_size"] = benchmark::Counter(static_cast<double>(ts.size()));
    state.counters["sample_count"] = benchmark::Counter(static_cast<double>(sampleCount));
}
// 10, 60, 300 samples mirrors realistic history depths at 1 s / 1 min / 5 min of uptime
BENCHMARK(BM_StorageModel_HistoryCopyOverhead)->Arg(10)->Arg(60)->Arg(300)->Unit(benchmark::kMicrosecond);

} // namespace
