// Benchmarks for Domain/History ring buffer
//
// These benchmarks measure the performance of the History class which is used
// extensively for time-series data (CPU%, memory, per-core metrics).
// Memory tracking is included to ensure no unexpected allocations.
//
// Every benchmark passes the History object itself through benchmark::DoNotOptimize, not just
// history.size(): observing only the size let the compiler treat the stored payload as dead (the ring
// is a local whose elements were never read back), so a push could compile down to the index/size
// bookkeeping alone. Laundering the whole object also stops reads from being constant-folded out of a
// buffer that was filled with compile-time-known values (#877).

#include "Domain/History.h"
#include "MemoryTracker.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace
{

constexpr std::size_t kCapacity = 300; // 5 minutes at 1Hz

// Pre-generated index pool so RNG cost stays out of the timed loop; a power of two for cheap masking.
constexpr std::size_t kIndexPoolSize = 1024;
constexpr std::size_t kIndexPoolMask = kIndexPoolSize - 1;

template<std::size_t Capacity> void fillHistory(Domain::History<double, Capacity>& history, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        history.push(static_cast<double>(i));
    }
}

// Benchmark push() operation - this is called every sample interval
static void BM_History_Push(benchmark::State& state)
{
    Domain::History<double, kCapacity> history;
    double value = 0.0;

    for (auto _ : state)
    {
        history.push(value);
        value += 0.1;
        benchmark::DoNotOptimize(history);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(double)));
}
BENCHMARK(BM_History_Push);

// Benchmark push() when history is full (steady-state operation)
static void BM_History_PushFull(benchmark::State& state)
{
    Domain::History<double, kCapacity> history;
    fillHistory(history, kCapacity);

    double value = static_cast<double>(kCapacity);
    for (auto _ : state)
    {
        history.push(value);
        value += 0.1;
        benchmark::DoNotOptimize(history);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(double)));
}
BENCHMARK(BM_History_PushFull);

// Benchmark operator[] access - used when rendering graphs
static void BM_History_RandomAccess(benchmark::State& state)
{
    Domain::History<double, kCapacity> history;
    fillHistory(history, kCapacity);

    // Indices are drawn before timing starts: the per-iteration mt19937 + distribution call used to cost
    // more than the indexed read it was meant to measure (#877).
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> dist(0, kCapacity - 1);
    std::array<std::size_t, kIndexPoolSize> indices{};
    for (auto& index : indices)
    {
        index = dist(rng);
    }

    std::size_t idx = 0;
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(history);
        benchmark::DoNotOptimize(history[indices[idx & kIndexPoolMask]]);
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_History_RandomAccess);

// Benchmark sequential access - typical for graph rendering
static void BM_History_SequentialAccess(benchmark::State& state)
{
    Domain::History<double, kCapacity> history;
    fillHistory(history, kCapacity);

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(history);
        double sum = 0.0;
        for (std::size_t i = 0; i < history.size(); ++i)
        {
            sum += history[i];
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kCapacity * sizeof(double)));
}
BENCHMARK(BM_History_SequentialAccess);

// Benchmark copyTo() - used for ImPlot rendering
static void BM_History_CopyTo(benchmark::State& state)
{
    Domain::History<double, kCapacity> history;
    fillHistory(history, kCapacity);

    std::array<double, kCapacity> buffer{};

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(history);
        auto copied = history.copyTo(buffer.data(), buffer.size());
        benchmark::DoNotOptimize(copied);
        benchmark::DoNotOptimize(buffer);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kCapacity * sizeof(double)));
}
BENCHMARK(BM_History_CopyTo);

// Benchmark copyTo() with wrapped data (worst case)
static void BM_History_CopyToWrapped(benchmark::State& state)
{
    Domain::History<double, kCapacity> history;
    // Push 1.5x capacity so the data wraps around
    fillHistory(history, kCapacity + (kCapacity / 2));

    std::array<double, kCapacity> buffer{};

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(history);
        auto copied = history.copyTo(buffer.data(), buffer.size());
        benchmark::DoNotOptimize(copied);
        benchmark::DoNotOptimize(buffer);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kCapacity * sizeof(double)));
}
BENCHMARK(BM_History_CopyToWrapped);

// Benchmark latest() - frequently called for current value display
static void BM_History_Latest(benchmark::State& state)
{
    Domain::History<double, kCapacity> history;
    fillHistory(history, kCapacity);

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(history);
        benchmark::DoNotOptimize(history.latest());
    }
}
BENCHMARK(BM_History_Latest);

// Steady-state push into a full ring of the given compile-time capacity.
template<std::size_t Capacity> void pushIntoFullRing(benchmark::State& state)
{
    Domain::History<double, Capacity> history;
    fillHistory(history, Capacity);

    double value = static_cast<double>(Capacity);
    for (auto _ : state)
    {
        history.push(value);
        value += 0.1;
        benchmark::DoNotOptimize(history);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(double)));
}

// Benchmark push() across ring capacities. History's capacity is a template parameter, so each argument
// dispatches to a History of exactly that capacity. This used to push into a single History<double, 3600>
// and clear() it every N pushes, so the capacity never actually varied -- the argument only changed how
// often the cheap clear() branch ran (#877). The argument list is the old Range(60, 3600) expansion, so
// the benchmark names (and their perf-data/linux-ci-baseline.json entries) are unchanged.
static void BM_History_PushVariableSize(benchmark::State& state)
{
    switch (state.range(0))
    {
    case 60:
        pushIntoFullRing<60>(state);
        break;
    case 64:
        pushIntoFullRing<64>(state);
        break;
    case 512:
        pushIntoFullRing<512>(state);
        break;
    case 3600:
        pushIntoFullRing<3600>(state); // 1 hour at 1Hz
        break;
    default:
        state.SkipWithError("BM_History_PushVariableSize: no History instantiation for this capacity");
        break;
    }
}
BENCHMARK(BM_History_PushVariableSize)->Arg(60)->Arg(64)->Arg(512)->Arg(3600)->Unit(benchmark::kNanosecond);

// Benchmark memory footprint of History with complex types
// This measures if storing larger objects causes unexpected allocations
static void BM_History_MemoryFootprint(benchmark::State& state)
{
    auto startStats = BenchmarkUtils::readMemoryStats();

    // Create a history with larger objects (simulating ProcessSnapshot-like data)
    struct LargeValue
    {
        double value1 = 0.0;
        double value2 = 0.0;
        double value3 = 0.0;
        std::uint64_t counter1 = 0;
        std::uint64_t counter2 = 0;
    };

    Domain::History<LargeValue, kCapacity> history;

    // Fill and overwrite multiple times. The pushed value varies per iteration and the whole ring is
    // observed, so the stores can be neither constant-folded nor discarded as dead.
    std::uint64_t counter = 0;
    for (auto _ : state)
    {
        const auto asDouble = static_cast<double>(counter);
        history.push(LargeValue{asDouble, asDouble * 2.0, asDouble * 3.0, counter, counter + 1});
        ++counter;
        benchmark::DoNotOptimize(history);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(LargeValue)));

    auto endStats = BenchmarkUtils::readMemoryStats();

    // Report expected vs actual memory. History stores its elements inline (std::array), so the object's
    // own size already includes the payload.
    constexpr std::size_t expectedBytes = sizeof(Domain::History<LargeValue, kCapacity>);
    state.counters["expected_bytes"] = benchmark::Counter(static_cast<double>(expectedBytes));

    if (startStats.valid() && endStats.valid())
    {
        auto growth = static_cast<std::int64_t>(endStats.vmRSS) - static_cast<std::int64_t>(startStats.vmRSS);
        state.counters["actual_growth_bytes"] = benchmark::Counter(static_cast<double>(growth));
    }
}
BENCHMARK(BM_History_MemoryFootprint)->Iterations(10000);

} // namespace
