// Benchmarks for Domain/ProcessModel
//
// These benchmarks measure the performance of process enumeration and snapshot
// computation, which are the most frequently executed operations in the app.
// Memory tracking is included to catch allocation regressions.

#include "Domain/ProcessModel.h"
#include "MemoryTracker.h"
#include "Platform/Factory.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

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

// The rest of a refresh() besides enumerate() (#2982): what ProcessModel::refresh() asks the probe for
// each sample after it -- the total CPU time, the per-connection network reading, the package energy
// and the capabilities, twice -- on this machine's real probe.
static void BM_ProcessProbe_RefreshExtras(benchmark::State& state)
{
    const auto probe = Platform::makeProcessProbe();
    (void) probe->enumerate();

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(probe->capabilities());
        benchmark::DoNotOptimize(probe->totalCpuTime());
        auto traffic = probe->readSocketTraffic();
        benchmark::DoNotOptimize(traffic);
        benchmark::DoNotOptimize(probe->capabilities());
        auto energy = probe->readPackageEnergy();
        benchmark::DoNotOptimize(energy);
    }
}
BENCHMARK(BM_ProcessProbe_RefreshExtras)->Unit(benchmark::kMicrosecond);

// Domain alone (#2982): ProcessModel's snapshot build (per-process deltas, rates and the published
// vector) over one real enumerate() of this machine, its real names and command lines, without the
// probe. Each sample moves the model's clock on 1 s and every process's CPU time on a little, so the
// rates are computed as a 1 s refresh would compute them.
static void BM_ProcessModel_UpdateCapturedCounters(benchmark::State& state)
{
    auto probe = Platform::makeProcessProbe();
    auto counters = probe->enumerate();
    std::uint64_t totalCpuTime = probe->totalCpuTime();
    const auto now = std::make_shared<Domain::ProcessModel::Clock::time_point>(Domain::ProcessModel::Clock::now());
    Domain::ProcessModel model(std::move(probe), [now] { return *now; });

    const auto sampleOnce = [&]
    {
        for (std::size_t i = 0; i < counters.size(); ++i)
        {
            counters[i].userTime += i % 3;
            counters[i].systemTime += i % 2;
        }
        totalCpuTime += 10'000'000;
        *now += std::chrono::seconds(1);
        model.updateFromCounters(counters, totalCpuTime);
    };
    sampleOnce(); // Seeds the previous counters, so the timed samples compute deltas

    for (auto _ : state)
    {
        sampleOnce();
        benchmark::DoNotOptimize(model.processCount());
    }
    state.counters["processes"] = benchmark::Counter(static_cast<double>(model.processCount()));
}
BENCHMARK(BM_ProcessModel_UpdateCapturedCounters)->Unit(benchmark::kMicrosecond);

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

// Benchmark process lookup by PID through ProcessModel::findSnapshot(), the lookup the app makes
// (ProcessesPanel::findSnapshot() delegates to it): a linear search under the model's lock that
// copies only the matching snapshot. It used to copy the whole vector with snapshots() and search
// the copy, which made it a second GetSnapshots: the copy was nearly all of its time, and that
// copy's cost swings about 40% between hosted-runner CPU classes (#2332).
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
        const auto found = model.findSnapshot(targetPid);
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
        auto rssGrowth = (static_cast<std::int64_t>(endStats.vmRSS) - static_cast<std::int64_t>(startStats.vmRSS));
        auto heapGrowth = (static_cast<std::int64_t>(endStats.vmData) - static_cast<std::int64_t>(startStats.vmData));

        state.counters["rss_growth_kb"] = benchmark::Counter((static_cast<double>(rssGrowth)) / 1024.0);
        state.counters["heap_growth_kb"] = benchmark::Counter((static_cast<double>(heapGrowth)) / 1024.0);
        state.counters["final_rss_mb"] = benchmark::Counter((static_cast<double>(endStats.vmRSS)) / (1024.0 * 1024.0));

        // Per-iteration memory (should be ~0 for stable implementation)
        if (state.iterations() > 0)
        {
            state.counters["bytes_per_iter"] =
                benchmark::Counter((static_cast<double>(rssGrowth)) / (static_cast<double>(state.iterations())));
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
    state.counters["rate_hz"] = benchmark::Counter(1000.0 / (static_cast<double>(std::max(delayMilliseconds, int64_t{1}))));
}
// Test 0ms (as fast as possible), 100ms (10Hz), 500ms (2Hz), 1000ms (1Hz)
BENCHMARK(BM_ProcessModel_RefreshRate)->Arg(0)->Arg(100)->Arg(500)->Arg(1000)->Unit(benchmark::kMillisecond);

} // namespace
