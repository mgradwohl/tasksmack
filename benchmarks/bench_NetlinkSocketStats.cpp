// Benchmarks for NetlinkSocketStats (Linux-only)
//
// These benchmarks measure the performance of the Netlink INET_DIAG socket query
// which is used for per-process network monitoring. This was identified as a
// hot path consuming ~10% CPU (see issue #362).

#if defined(__linux__) && __has_include(<linux/inet_diag.h>) && __has_include(<linux/sock_diag.h>)

#include "Platform/Linux/NetlinkSocketStats.h"

#include <chrono>

#include <benchmark/benchmark.h>

namespace
{

// Benchmark the raw Netlink query (uncached)
// This measures the actual kernel cost per query
static void BM_NetlinkSocketStats_QueryUncached(benchmark::State& state)
{
    // Create with 0ms TTL to disable caching
    Platform::NetlinkSocketStats stats(std::chrono::milliseconds(0));

    if (!stats.isAvailable())
    {
        state.SkipWithError("Netlink INET_DIAG not available");
        return;
    }

    for (auto _ : state)
    {
        auto sockets = stats.queryAllSockets();
        benchmark::DoNotOptimize(sockets.data());
        benchmark::DoNotOptimize(sockets.size());
    }

    // Report socket count as a counter
    auto sockets = stats.queryAllSockets();
    state.counters["sockets"] = benchmark::Counter(static_cast<double>(sockets.size()));
}
BENCHMARK(BM_NetlinkSocketStats_QueryUncached)->Unit(benchmark::kMillisecond);

// Benchmark cached query (should be very fast after initial query)
// This simulates the typical usage pattern where queries happen faster than the TTL
static void BM_NetlinkSocketStats_QueryCached(benchmark::State& state)
{
    // Create with default TTL (500ms) - all benchmark iterations will hit cache
    Platform::NetlinkSocketStats stats;

    if (!stats.isAvailable())
    {
        state.SkipWithError("Netlink INET_DIAG not available");
        return;
    }

    // Prime the cache
    [[maybe_unused]] auto primed = stats.queryAllSockets();

    for (auto _ : state)
    {
        auto sockets = stats.queryAllSockets();
        benchmark::DoNotOptimize(sockets.data());
        benchmark::DoNotOptimize(sockets.size());
    }
}
BENCHMARK(BM_NetlinkSocketStats_QueryCached);

// Benchmark building the inode-to-PID mapping
// This scans /proc/[pid]/fd/* which can be expensive on systems with many processes
static void BM_NetlinkSocketStats_BuildInodeToPidMap(benchmark::State& state)
{
    for (auto _ : state)
    {
        auto mapping = Platform::buildInodeToPidMap();
        benchmark::DoNotOptimize(mapping.size());
    }

    // Report map size
    auto mapping = Platform::buildInodeToPidMap();
    state.counters["mappings"] = benchmark::Counter(static_cast<double>(mapping.size()));
}
BENCHMARK(BM_NetlinkSocketStats_BuildInodeToPidMap)->Unit(benchmark::kMillisecond);

// Benchmark the full network attribution pipeline (query + map + aggregate)
// This represents the full cost of per-process network stats
static void BM_NetlinkSocketStats_FullPipeline(benchmark::State& state)
{
    // Use uncached to measure full cost
    Platform::NetlinkSocketStats stats(std::chrono::milliseconds(0));

    if (!stats.isAvailable())
    {
        state.SkipWithError("Netlink INET_DIAG not available");
        return;
    }

    for (auto _ : state)
    {
        auto sockets = stats.queryAllSockets();
        auto inodeToPid = Platform::buildInodeToPidMap();
        auto pidStats = Platform::aggregateByPid(sockets, inodeToPid);
        benchmark::DoNotOptimize(pidStats.size());
    }
}
BENCHMARK(BM_NetlinkSocketStats_FullPipeline)->Unit(benchmark::kMillisecond);

// Benchmark with different cache TTL values to find optimal balance
static void BM_NetlinkSocketStats_CacheTTL(benchmark::State& state)
{
    const auto ttlMs = std::chrono::milliseconds(state.range(0));
    Platform::NetlinkSocketStats stats(ttlMs);

    if (!stats.isAvailable())
    {
        state.SkipWithError("Netlink INET_DIAG not available");
        return;
    }

    // Simulate refresh pattern (query, wait, query, wait, ...)
    // The benchmark framework handles timing, we just do the work
    for (auto _ : state)
    {
        auto sockets = stats.queryAllSockets();
        benchmark::DoNotOptimize(sockets.data());
    }

    state.counters["ttl_ms"] = benchmark::Counter(static_cast<double>(ttlMs.count()));
}
// Test different TTL values: 0 (no cache), 100ms, 250ms, 500ms (default), 1000ms
BENCHMARK(BM_NetlinkSocketStats_CacheTTL)->Arg(0)->Arg(100)->Arg(250)->Arg(500)->Arg(1000);

} // namespace

#endif // __linux__ && headers available
