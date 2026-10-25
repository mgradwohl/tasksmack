// Benchmarks for Domain/History.h's HistoryBuffer ring
//
// HistoryBuffer is the runtime-capacity ring ProcessModel keeps its aggregated system histories in.
// These benchmarks used to time the fixed-capacity History<T, N>, which had no production user and
// was removed (#1185); they were renamed BM_HistoryBuffer_* with it, since a runtime capacity times
// differently from a compile-time one and the old names' baseline numbers no longer apply.
// Memory tracking is included to ensure no unexpected allocations.
//
// Every benchmark passes the ring itself through benchmark::DoNotOptimize, not just
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

void fillHistory(Domain::HistoryBuffer<double>& history, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        history.push(static_cast<double>(i));
    }
}

// Benchmark push() operation - this is called every sample interval
static void BM_HistoryBuffer_Push(benchmark::State& state)
{
    Domain::HistoryBuffer<double> history(kCapacity);
    double value = 0.0;

    for (auto _ : state)
    {
        history.push(value);
        value += 0.1;
        benchmark::DoNotOptimize(history);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(double)));
}
BENCHMARK(BM_HistoryBuffer_Push);

// Benchmark push() when history is full (steady-state operation)
static void BM_HistoryBuffer_PushFull(benchmark::State& state)
{
    Domain::HistoryBuffer<double> history(kCapacity);
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
BENCHMARK(BM_HistoryBuffer_PushFull);

// Benchmark operator[] access - used when rendering graphs
static void BM_HistoryBuffer_RandomAccess(benchmark::State& state)
{
    Domain::HistoryBuffer<double> history(kCapacity);
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
BENCHMARK(BM_HistoryBuffer_RandomAccess);

// Benchmark sequential access - typical for graph rendering
static void BM_HistoryBuffer_SequentialAccess(benchmark::State& state)
{
    Domain::HistoryBuffer<double> history(kCapacity);
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
BENCHMARK(BM_HistoryBuffer_SequentialAccess);

// Benchmark copyTo() - used for ImPlot rendering
static void BM_HistoryBuffer_CopyTo(benchmark::State& state)
{
    Domain::HistoryBuffer<double> history(kCapacity);
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
BENCHMARK(BM_HistoryBuffer_CopyTo);

// Benchmark copyTo() with wrapped data (worst case)
static void BM_HistoryBuffer_CopyToWrapped(benchmark::State& state)
{
    Domain::HistoryBuffer<double> history(kCapacity);
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
BENCHMARK(BM_HistoryBuffer_CopyToWrapped);

// Benchmark latest() - frequently called for current value display
static void BM_HistoryBuffer_Latest(benchmark::State& state)
{
    Domain::HistoryBuffer<double> history(kCapacity);
    fillHistory(history, kCapacity);

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(history);
        benchmark::DoNotOptimize(history.latest());
    }
}
BENCHMARK(BM_HistoryBuffer_Latest);

// Benchmark steady-state push() into a full ring across capacities (1 minute to 1 hour at 1 Hz, plus
// the power-of-two sizes 64 and 512).
static void BM_HistoryBuffer_PushVariableSize(benchmark::State& state)
{
    const auto capacity = static_cast<std::size_t>(state.range(0));
    Domain::HistoryBuffer<double> history(capacity);
    fillHistory(history, capacity);

    double value = static_cast<double>(capacity);
    for (auto _ : state)
    {
        history.push(value);
        value += 0.1;
        benchmark::DoNotOptimize(history);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(double)));
}
BENCHMARK(BM_HistoryBuffer_PushVariableSize)->Arg(60)->Arg(64)->Arg(512)->Arg(3600)->Unit(benchmark::kNanosecond);

// Benchmark memory footprint of a HistoryBuffer of a larger type
// This measures if storing larger objects causes unexpected allocations
static void BM_HistoryBuffer_MemoryFootprint(benchmark::State& state)
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

    Domain::HistoryBuffer<LargeValue> history(kCapacity);

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

    // Report expected vs actual memory: the ring object plus its one backing allocation, made when it
    // was constructed. Pushing into it allocates nothing more.
    constexpr std::size_t expectedBytes = sizeof(Domain::HistoryBuffer<LargeValue>) + (kCapacity * sizeof(LargeValue));
    state.counters["expected_bytes"] = benchmark::Counter(static_cast<double>(expectedBytes));

    if (startStats.valid() && endStats.valid())
    {
        auto growth = static_cast<std::int64_t>(endStats.vmRSS) - static_cast<std::int64_t>(startStats.vmRSS);
        state.counters["actual_growth_bytes"] = benchmark::Counter(static_cast<double>(growth));
    }
}
BENCHMARK(BM_HistoryBuffer_MemoryFootprint)->Iterations(10000);

} // namespace
