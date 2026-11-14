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
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
#include "Platform/Windows/WindowsCommitPaging.h"
#include "Platform/Windows/WindowsNtQuery.h"
#include "Platform/Windows/WindowsProcAddress.h"
#include "Platform/Windows/WindowsSystemProbeMath.h"

// clang-format off
#include <winsock2.h> // Before iphlpapi.h, which needs it
#include <ws2ipdef.h> // MIB_IF_ROW2 and MIB_IF_TABLE2 in netioapi.h
#include <windows.h>
#include <winternl.h>
#include <iphlpapi.h>
// clang-format on
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

// The network read's OS calls (#3713). Each sample used to call GetIfTable2, which asks every row for its
// counters: a context switch per NDIS filter-module row, rows the probe then skips. The probe now lists
// the interfaces without their counters (every NETWORK_INTERFACE_RESCAN_INTERVAL_SECONDS) and reads each
// counted row with GetIfEntry2 (each sample). switches_per_call is the benchmark thread's context
// switches over the timed calls, from its SYSTEM_THREAD_INFORMATION record, per call.

// The calling thread's context switches so far: SYSTEM_THREAD_INFORMATION::Reserved3 is ContextSwitches.
std::optional<ULONG> currentThreadContextSwitches(std::vector<std::byte>& buffer)
{
    const auto written =
        Platform::WindowsCommitPaging::querySystemInformationInto(Platform::Windows::ntQuerySystemInformation(),
                                                                  Platform::WindowsCommitPaging::SYSTEM_PROCESS_INFORMATION_CLASS,
                                                                  buffer,
                                                                  Platform::WindowsCommitPaging::PROCESS_INFORMATION_INITIAL_BYTES);
    if (!written.has_value())
    {
        return std::nullopt;
    }
    const auto processId = static_cast<ULONG_PTR>(GetCurrentProcessId());
    const auto threadId = static_cast<ULONG_PTR>(GetCurrentThreadId());
    std::size_t offset = 0;
    while (offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= *written)
    {
        SYSTEM_PROCESS_INFORMATION process{};
        std::memcpy(&process, buffer.data() + offset, sizeof(process));
        if (reinterpret_cast<ULONG_PTR>(process.UniqueProcessId) == processId)
        {
            for (ULONG i = 0; i < process.NumberOfThreads; ++i)
            {
                const std::size_t at = offset + sizeof(SYSTEM_PROCESS_INFORMATION) + (i * sizeof(SYSTEM_THREAD_INFORMATION));
                if (at + sizeof(SYSTEM_THREAD_INFORMATION) > *written)
                {
                    break;
                }
                SYSTEM_THREAD_INFORMATION thread{};
                std::memcpy(&thread, buffer.data() + at, sizeof(thread));
                if (reinterpret_cast<ULONG_PTR>(thread.ClientId.UniqueThread) == threadId)
                {
                    return thread.Reserved3;
                }
            }
            return std::nullopt;
        }
        if (process.NextEntryOffset == 0)
        {
            break;
        }
        offset += process.NextEntryOffset;
    }
    return std::nullopt;
}

// Times @p call per iteration and reports the context switches it made per call. The snapshot that
// counts them is taken outside the timed region.
template<typename Call> void runCountingSwitches(benchmark::State& state, Call call)
{
    std::vector<std::byte> buffer;
    const std::optional<ULONG> before = currentThreadContextSwitches(buffer);
    for (auto _ : state)
    {
        call();
    }
    const std::optional<ULONG> after = currentThreadContextSwitches(buffer);
    if (before.has_value() && after.has_value() && state.iterations() > 0)
    {
        state.counters["switches_per_call"] = static_cast<double>(*after - *before) / static_cast<double>(state.iterations());
    }
}

// GetIfTable2Ex with its level as an integer, as WindowsSystemProbe calls it: the SDK declares
// MibIfTableNormalWithoutStatistics only above this build's NTDDI_VERSION.
using GetIfTable2ExFn = DWORD(WINAPI*)(ULONG, MIB_IF_TABLE2**);
constexpr ULONG MIB_IF_TABLE_NORMAL_WITHOUT_STATISTICS = 2;

GetIfTable2ExFn getIfTable2Ex()
{
    return Platform::Windows::getProcAddress<GetIfTable2ExFn>(GetModuleHandleW(L"iphlpapi.dll"), "GetIfTable2Ex");
}

// The counted rows' LUIDs, from a list without statistics (as the probe keeps them)
std::vector<NET_LUID> countedInterfaceLuids()
{
    auto* const list = getIfTable2Ex();
    std::vector<NET_LUID> luids;
    MIB_IF_TABLE2* table = nullptr;
    if (list == nullptr || list(MIB_IF_TABLE_NORMAL_WITHOUT_STATISTICS, &table) != NO_ERROR || table == nullptr)
    {
        return luids;
    }
    const std::span<const MIB_IF_ROW2> rows(static_cast<const MIB_IF_ROW2*>(table->Table), table->NumEntries);
    for (const MIB_IF_ROW2& row : rows)
    {
        if (Platform::isCountedNetworkRow(row.Type, row.InterfaceAndOperStatusFlags.FilterInterface != 0))
        {
            luids.push_back(row.InterfaceLuid);
        }
    }
    FreeMibTable(table);
    return luids;
}

// Before #3713, each sample: every row with its counters
static void BM_WindowsNetwork_GetIfTable2(benchmark::State& state)
{
    runCountingSwitches(state,
                        []
                        {
                            MIB_IF_TABLE2* table = nullptr;
                            if (GetIfTable2(&table) == NO_ERROR && table != nullptr)
                            {
                                benchmark::DoNotOptimize(table->NumEntries);
                                FreeMibTable(table);
                            }
                        });
}
BENCHMARK(BM_WindowsNetwork_GetIfTable2)->Unit(benchmark::kMicrosecond);

// After, at the rescan: every row without its counters
static void BM_WindowsNetwork_ListWithoutStatistics(benchmark::State& state)
{
    auto* const list = getIfTable2Ex();
    if (list == nullptr)
    {
        state.SkipWithError("no GetIfTable2Ex");
        return;
    }
    runCountingSwitches(state,
                        [list]
                        {
                            MIB_IF_TABLE2* table = nullptr;
                            if (list(MIB_IF_TABLE_NORMAL_WITHOUT_STATISTICS, &table) == NO_ERROR && table != nullptr)
                            {
                                benchmark::DoNotOptimize(table->NumEntries);
                                FreeMibTable(table);
                            }
                        });
}
BENCHMARK(BM_WindowsNetwork_ListWithoutStatistics)->Unit(benchmark::kMicrosecond);

// After, each sample: GetIfEntry2 on each counted row
static void BM_WindowsNetwork_CountedEntries(benchmark::State& state)
{
    const std::vector<NET_LUID> luids = countedInterfaceLuids();
    runCountingSwitches(state,
                        [&luids]
                        {
                            for (const NET_LUID& luid : luids)
                            {
                                MIB_IF_ROW2 row{}; // NOLINT(bugprone-invalid-enum-default-initialization) - zeroed, as GetIfEntry2 expects
                                row.InterfaceLuid = luid;
                                if (GetIfEntry2(&row) == NO_ERROR)
                                {
                                    benchmark::DoNotOptimize(row.InOctets);
                                }
                            }
                        });
    state.counters["rows"] = static_cast<double>(luids.size());
}
BENCHMARK(BM_WindowsNetwork_CountedEntries)->Unit(benchmark::kMicrosecond);

// BM_SystemProbe_Sample with the context switches a whole read() makes (#3713): every switch is a wake-up
// of the system sampler, which reads once a second.
static void BM_WindowsSystemProbe_SampleSwitches(benchmark::State& state)
{
    auto probe = Platform::makeSystemProbe();
    // Untimed: the seed, and the read that opens PDH's counter (#3154)
    (void) probe->read();
    (void) probe->read();
    runCountingSwitches(state,
                        [&probe]
                        {
                            auto counters = probe->read();
                            benchmark::DoNotOptimize(counters.cpuTotal.user);
                        });
}
BENCHMARK(BM_WindowsSystemProbe_SampleSwitches)->Unit(benchmark::kMicrosecond);
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
