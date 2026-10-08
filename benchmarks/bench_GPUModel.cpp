// Benchmarks for Domain/GPUModel
//
// These benchmarks measure the performance of GPU metric sampling and
// snapshot computation. GPUModel aggregates per-GPU counters, computes
// derived metrics (memory%, power%), and maintains history for charting.
// Memory tracking is included to catch allocation regressions.
//
// They use the real platform probe. When that probe enumerates no GPU (as on the GPU-less CI runner),
// every one of them skips (skipWithoutGpu()) rather than timing the probe's early return (#1420).

#include "Domain/GPUModel.h"
#include "MemoryTracker.h"
#include "Platform/Factory.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{

// Skips @p state, and returns true, when the real GPU probe finds no GPU. Each benchmark here would
// otherwise time an empty probe's early return -- 1-110 ns of nothing -- and hand it to the
// regression gate as if it measured GPU sampling (#1420). check-benchmark-regression.py reports a
// skipped benchmark as not measured, outside its coverage figure.
bool skipWithoutGpu(benchmark::State& state)
{
    static const bool k_HasGpu = !Platform::makeGPUProbe()->enumerateGPUs().empty();
    if (!k_HasGpu)
    {
        state.SkipWithMessage("No GPUs available");
    }
    return !k_HasGpu;
}

// Benchmark raw GPU probe enumeration (discovers GPUs and their static info)
static void BM_GPUProbe_Enumerate(benchmark::State& state)
{
    if (skipWithoutGpu(state))
    {
        return;
    }

    auto probe = Platform::makeGPUProbe();

    for (auto _ : state)
    {
        auto info = probe->enumerateGPUs();
        benchmark::DoNotOptimize(info.data());
        benchmark::DoNotOptimize(info.size());
    }

    // Report GPU count for context
    auto info = probe->enumerateGPUs();
    state.counters["gpus"] = benchmark::Counter(static_cast<double>(info.size()));
}
BENCHMARK(BM_GPUProbe_Enumerate);

// Benchmark raw GPU counter read (hot path – called every refresh cycle).
// enumerateGPUs() is called first to match the production code path; on
// Windows this populates the DXGI→LUID map that readGPUCounters() relies on
// to merge per-adapter PDH utilization data.
static void BM_GPUProbe_ReadCounters(benchmark::State& state)
{
    if (skipWithoutGpu(state))
    {
        return;
    }

    auto probe = Platform::makeGPUProbe();

    // Must enumerate before reading counters so the DXGI→LUID map is ready
    // on Windows (no-op on Linux).
    auto gpus = probe->enumerateGPUs();
    benchmark::DoNotOptimize(gpus);

    BenchmarkUtils::MemoryDeltaTracker memTracker;

    for (auto _ : state)
    {
        auto counters = probe->readGPUCounters();
        benchmark::DoNotOptimize(counters.data());
        benchmark::DoNotOptimize(counters.size());
    }

    auto counters = probe->readGPUCounters();
    state.counters["gpus"] = benchmark::Counter(static_cast<double>(counters.size()));

    BenchmarkUtils::reportMemoryCounters(state);
    BenchmarkUtils::reportMemoryDelta(state, memTracker);
}
BENCHMARK(BM_GPUProbe_ReadCounters);

// Benchmark GPUModel::refresh() – full pipeline including GPU counter read,
// memory% and power% computation, and history append.
static void BM_GPUModel_Refresh(benchmark::State& state)
{
    if (skipWithoutGpu(state))
    {
        return;
    }

    auto probe = Platform::makeGPUProbe();
    Domain::GPUModel model(std::move(probe));

    // Prime previous-counters state so the first benchmarked call computes valid deltas
    model.refresh();

    BenchmarkUtils::MemoryDeltaTracker memTracker;

    for (auto _ : state)
    {
        model.refresh();
        benchmark::DoNotOptimize(model);
    }

    auto snaps = model.snapshots();
    state.counters["gpus"] = benchmark::Counter(static_cast<double>(snaps.size()));

    BenchmarkUtils::reportMemoryCounters(state);
    BenchmarkUtils::reportMemoryDelta(state, memTracker);
}
BENCHMARK(BM_GPUModel_Refresh);

// Benchmark snapshots() – read-only copy of current GPU data returned to the UI.
// Should be fast (shared_mutex read lock + copy of a small vector).
static void BM_GPUModel_Snapshots(benchmark::State& state)
{
    if (skipWithoutGpu(state))
    {
        return;
    }

    auto probe = Platform::makeGPUProbe();
    Domain::GPUModel model(std::move(probe));
    model.refresh();

    for (auto _ : state)
    {
        auto snaps = model.snapshots();
        benchmark::DoNotOptimize(snaps.data());
        benchmark::DoNotOptimize(snaps.size());
    }
}
BENCHMARK(BM_GPUModel_Snapshots);

// Benchmark gpuInfo() – returns static GPU identification data.
// Called once per frame for the GPU section header in the UI.
static void BM_GPUModel_GpuInfo(benchmark::State& state)
{
    if (skipWithoutGpu(state))
    {
        return;
    }

    auto probe = Platform::makeGPUProbe();
    Domain::GPUModel model(std::move(probe));

    for (auto _ : state)
    {
        auto info = model.gpuInfo();
        benchmark::DoNotOptimize(info.data());
        benchmark::DoNotOptimize(info.size());
    }
}
BENCHMARK(BM_GPUModel_GpuInfo);

// Benchmark reading a GPU's timestamps from the latest publication – used to align chart X axes.
// The history reads here used to time per-series copy accessors that only tests called, removed in
// #1185; the charts read the published views (#1412).
static void BM_GPUModel_HistoryTimestamps(benchmark::State& state)
{
    if (skipWithoutGpu(state))
    {
        return;
    }

    auto probe = Platform::makeGPUProbe();
    Domain::GPUModel model(std::move(probe));

    for (int i = 0; i < 10; ++i)
    {
        model.refresh();
    }

    // A GPU ID to query (the probe found a GPU, so the model lists it, but guard anyway)
    auto info = model.gpuInfo();
    if (info.empty())
    {
        state.SkipWithMessage("No GPUs available");
        return;
    }

    const std::string gpuId = info[0].id;

    for (auto _ : state)
    {
        const auto publication = model.publication();
        const auto it = publication->histories.find(gpuId);
        const std::size_t size = (it != publication->histories.end()) ? it->second.timestamps.size() : 0;
        benchmark::DoNotOptimize(size);
    }
}
BENCHMARK(BM_GPUModel_HistoryTimestamps);

// Benchmark reading a GPU's utilization series from the latest publication. Read per-GPU per-frame
// for chart rendering.
static void BM_GPUModel_UtilizationHistory(benchmark::State& state)
{
    if (skipWithoutGpu(state))
    {
        return;
    }

    auto probe = Platform::makeGPUProbe();
    Domain::GPUModel model(std::move(probe));

    for (int i = 0; i < 10; ++i)
    {
        model.refresh();
    }

    // A GPU ID to query (the probe found a GPU, so the model lists it, but guard anyway)
    auto info = model.gpuInfo();
    if (info.empty())
    {
        state.SkipWithMessage("No GPUs available");
        return;
    }

    const std::string gpuId = info[0].id;

    for (auto _ : state)
    {
        const auto publication = model.publication();
        const auto it = publication->histories.find(gpuId);
        const std::size_t size = (it != publication->histories.end()) ? it->second.utilization.size() : 0;
        benchmark::DoNotOptimize(size);
    }
}
BENCHMARK(BM_GPUModel_UtilizationHistory);

// Benchmark readProcessGPUCounters() – the per-process GPU read that each refresh() now makes on
// the GPU sampler and publishes for ProcessModel to merge (#1417).
static void BM_GPUModel_ProcessGpuCounters(benchmark::State& state)
{
    if (skipWithoutGpu(state))
    {
        return;
    }

    auto probe = Platform::makeGPUProbe();
    Domain::GPUModel model(std::move(probe));
    model.refresh();

    BenchmarkUtils::MemoryDeltaTracker memTracker;

    for (auto _ : state)
    {
        auto counters = model.readProcessGPUCounters();
        benchmark::DoNotOptimize(counters.data());
        benchmark::DoNotOptimize(counters.size());
    }

    auto counters = model.readProcessGPUCounters();
    state.counters["gpu_process_entries"] = benchmark::Counter(static_cast<double>(counters.size()));

    BenchmarkUtils::reportMemoryCounters(state);
    BenchmarkUtils::reportMemoryDelta(state, memTracker);
}
BENCHMARK(BM_GPUModel_ProcessGpuCounters);

// Benchmark memory growth over many refresh() cycles.
// Exercises the full production code path: probe read, delta computation,
// and history append. Reports RSS and heap growth so regressions are
// visible in benchmark output.
static void BM_GPUModel_MemoryGrowth(benchmark::State& state)
{
    if (skipWithoutGpu(state))
    {
        return;
    }

    auto probe = Platform::makeGPUProbe();
    Domain::GPUModel model(std::move(probe));

    // Prime previous-counters state
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
BENCHMARK(BM_GPUModel_MemoryGrowth)->Iterations(500);

} // namespace
