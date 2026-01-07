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

// Note: A benchmark comparing different cache TTL values was considered but not included
// because Google Benchmark's tight iteration loop doesn't produce meaningful time-based
// cache behavior differences - after the first query, all subsequent queries hit the cache
// regardless of TTL value (100ms, 250ms, etc.). Use BM_QueryCached vs BM_QueryUncached
// to measure cache hit vs miss performance instead.

} // namespace

#endif // __linux__ && headers available
