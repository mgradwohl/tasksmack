// Benchmarks for Domain/StorageModel
//
// These benchmarks measure the performance of disk I/O sampling and
// snapshot computation. StorageModel aggregates per-device counters into
// rates and maintains history for charting.
// Memory tracking is included to catch allocation regressions.

#include "Domain/SamplingConfig.h"
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

// Benchmark reading the shared timestamp axis used by all chart series from the latest publication,
// as StorageSection does each frame before reading any per-disk or aggregate series. The history
// reads here used to time per-series copy accessors that only tests called, removed in #1185.
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
        const auto publication = model.publication();
        const auto& ts = publication->timestamps;
        benchmark::DoNotOptimize(ts.data());
        benchmark::DoNotOptimize(ts.size());
    }
}
BENCHMARK(BM_StorageModel_HistoryTimestamps);

// Benchmark reading the aggregate read/write series from the latest publication,
// used to render the aggregate I/O charts
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
        const auto publication = model.publication();
        const auto& readHist = publication->totalReadHistory;
        const auto& writeHist = publication->totalWriteHistory;
        benchmark::DoNotOptimize(readHist.data());
        benchmark::DoNotOptimize(writeHist.data());
    }
}
BENCHMARK(BM_StorageModel_TotalRateHistory);

// Benchmark reading the per-disk series from the latest publication: one entry per disk device
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
        const auto publication = model.publication();
        const auto& hist = publication->perDiskHistory;
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

// Benchmark the combined cost of the history reads StorageSection makes on every render frame: the
// latest publication's timestamps, total read and write series and per-disk series. They are views
// of the shared history (#1412), so the cost should not grow with depth. History depth is controlled
// by varying the number of pre-seeded samples (trimming is disabled so the full depth is retained).
static void BM_StorageModel_HistoryCopyOverhead(benchmark::State& state)
{
    const auto sampleCount = static_cast<int>(state.range(0));

    auto probe = Platform::makeDiskProbe();
    Domain::StorageModel model(std::move(probe));

    // Keep all samples for the full duration of this benchmark: the longest window the model accepts
    // (it clamps to SamplingConfig's range, #1145), far longer than the pre-seeding takes.
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MAX);

    // Pre-seed the desired number of snapshots
    for (int i = 0; i < sampleCount; ++i)
    {
        model.sample();
    }

    for (auto _ : state)
    {
        const auto publication = model.publication();
        const auto& ts = publication->timestamps;
        const auto& readHist = publication->totalReadHistory;
        const auto& writeHist = publication->totalWriteHistory;
        const auto& perDisk = publication->perDiskHistory;
        benchmark::DoNotOptimize(ts.data());
        benchmark::DoNotOptimize(readHist.data());
        benchmark::DoNotOptimize(writeHist.data());
        benchmark::DoNotOptimize(perDisk.data());
    }

    // Report actual history depth
    state.counters["history_size"] = benchmark::Counter(static_cast<double>(model.publication()->timestamps.size()));
    state.counters["sample_count"] = benchmark::Counter(static_cast<double>(sampleCount));
}
// 10, 60, 300 samples mirrors realistic history depths at 1 s / 1 min / 5 min of uptime
BENCHMARK(BM_StorageModel_HistoryCopyOverhead)->Arg(10)->Arg(60)->Arg(300)->Unit(benchmark::kMicrosecond);

} // namespace
