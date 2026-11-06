// Benchmarks for Domain/SystemModel
//
// These benchmarks measure the performance of system metric sampling and
// snapshot computation, which are called from the UI/main thread update cycle.
// Memory tracking is included to catch allocation regressions.

#include "Domain/SamplingConfig.h"
#include "Domain/SystemModel.h"
#include "MemoryTracker.h"
#include "Platform/Factory.h"
#include "Platform/SystemTypes.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
#include "Platform/Windows/WindowsCommitPaging.h"
#include "Platform/Windows/WindowsNtQuery.h"
#endif

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

#ifdef _WIN32
// The two reads WindowsSystemProbe makes for compressed memory (#3483): the whole SystemProcessInformation
// snapshot that finds the Memory Compression process (each sample before #3483, now only to search for
// it), and its session's list alone, which each sample reads now. Each into a buffer kept between
// iterations, as the probe keeps it.
static void BM_WindowsMemoryCompression_WholeSnapshot(benchmark::State& state)
{
    const auto query = Platform::Windows::ntQuerySystemInformation();
    std::vector<std::byte> buffer;
    for (auto _ : state)
    {
        const auto written =
            Platform::WindowsCommitPaging::querySystemInformationInto(query,
                                                                      Platform::WindowsCommitPaging::SYSTEM_PROCESS_INFORMATION_CLASS,
                                                                      buffer,
                                                                      Platform::WindowsCommitPaging::PROCESS_INFORMATION_INITIAL_BYTES);
        if (!written.has_value())
        {
            state.SkipWithError("SystemProcessInformation failed");
            break;
        }
        auto process = Platform::WindowsCommitPaging::findMemoryCompressionProcess(std::span<const std::byte>(buffer.data(), *written));
        benchmark::DoNotOptimize(process);
    }
}
BENCHMARK(BM_WindowsMemoryCompression_WholeSnapshot)->Unit(benchmark::kMicrosecond);

static void BM_WindowsMemoryCompression_SessionList(benchmark::State& state)
{
    const auto query = Platform::Windows::ntQuerySystemInformation();
    std::vector<std::byte> buffer;
    const auto written =
        Platform::WindowsCommitPaging::querySystemInformationInto(query,
                                                                  Platform::WindowsCommitPaging::SYSTEM_PROCESS_INFORMATION_CLASS,
                                                                  buffer,
                                                                  Platform::WindowsCommitPaging::PROCESS_INFORMATION_INITIAL_BYTES);
    const auto found = written.has_value()
                         ? Platform::WindowsCommitPaging::findMemoryCompressionProcess(std::span<const std::byte>(buffer.data(), *written))
                         : std::nullopt;
    if (!found.has_value())
    {
        state.SkipWithError("no Memory Compression process (compression is off)");
        return;
    }
    for (auto _ : state)
    {
        const auto session = Platform::WindowsCommitPaging::querySessionProcessesInto(
            query, found->sessionId, buffer, Platform::WindowsCommitPaging::PROCESS_INFORMATION_INITIAL_BYTES);
        if (!session.has_value())
        {
            state.SkipWithError("SystemSessionProcessInformation failed");
            break;
        }
        auto process = Platform::WindowsCommitPaging::findMemoryCompressionProcess(std::span<const std::byte>(buffer.data(), *session),
                                                                                   found->processId);
        benchmark::DoNotOptimize(process);
    }
}
BENCHMARK(BM_WindowsMemoryCompression_SessionList)->Unit(benchmark::kMicrosecond);
#endif

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

// Benchmark the history reads the UI makes for graph rendering: the latest publication and one of
// its series, a view of the model's shared history rather than a copy (#1412). These used to time
// per-series copy accessors that only tests called, removed in #1185.
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
        const auto publication = model.publication();
        const auto& hist = publication->cpuHistory;
        benchmark::DoNotOptimize(hist.data());
        benchmark::DoNotOptimize(hist.size());
    }
}
BENCHMARK(BM_SystemModel_CpuHistory);

// Benchmark the per-core history read: the latest publication's per-core series, one view per
// logical CPU core
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
        const auto publication = model.publication();
        const auto& hist = publication->perCoreHistory;
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

// Per-sample cost against the network interface count, with churn: container hosts run hundreds of
// veth/bridge interfaces that come and go. Matching each interface to the previous sample by name,
// and each known interface to this sample, should scale about linearly (#1415); with linear scans it
// was quadratic. Alternates two interface sets in which a fifth of the interfaces differ, listed in
// opposite orders. A 10 s window at 1 s steps keeps the history, and so the publish copy, short.
static void BM_SystemModel_InterfaceChurn(benchmark::State& state)
{
    const auto interfaceCount = static_cast<std::size_t>(state.range(0));
    std::array<Platform::SystemCounters, 2> counterSets;
    for (std::size_t set = 0; set < counterSets.size(); ++set)
    {
        auto& counters = counterSets[set];
        for (std::size_t i = 0; i < interfaceCount; ++i)
        {
            Platform::SystemCounters::InterfaceCounters iface;
            // Every fifth interface is replaced by a differently named one in the second set.
            iface.name = ((set == 1) && (i % 5 == 0)) ? "veth-new" + std::to_string(i) : "veth" + std::to_string(i);
            iface.isVirtual = true;
            iface.isVirtualKnown = true;
            counters.networkInterfaces.push_back(std::move(iface));
        }
        if (set == 1)
        {
            std::ranges::reverse(counters.networkInterfaces);
        }
    }

    Domain::SystemModel model(nullptr);
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);
    double nowSeconds = 1000.0;
    std::uint64_t bytes = 0;
    std::size_t sample = 0;
    for (auto _ : state)
    {
        auto& counters = counterSets[sample++ % counterSets.size()];
        bytes += 1000;
        for (auto& iface : counters.networkInterfaces)
        {
            iface.rxBytes = bytes;
            iface.txBytes = bytes;
        }
        model.updateFromCounters(counters, nowSeconds += 1.0);
        benchmark::DoNotOptimize(model);
    }
    state.SetComplexityN(static_cast<benchmark::IterationCount>(interfaceCount));
}
BENCHMARK(BM_SystemModel_InterfaceChurn)->Arg(10)->Arg(100)->Arg(500)->Complexity()->Unit(benchmark::kMicrosecond);

} // namespace
