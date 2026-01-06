// Benchmarks for Domain/History ring buffer
//
// These benchmarks measure the performance of the History class which is used
// extensively for time-series data (CPU%, memory, per-core metrics).
// Memory tracking is included to ensure no unexpected allocations.

#include "Domain/History.h"
#include "MemoryTracker.h"

#include <random>

#include <benchmark/benchmark.h>

namespace
{

// Benchmark push() operation - this is called every sample interval
static void BM_History_Push(benchmark::State& state)
{
    Domain::History<double, 300> history; // 5 minutes at 1Hz
    double value = 0.0;

    for (auto _ : state)
    {
        history.push(value);
        value += 0.1;
        benchmark::DoNotOptimize(history.size());
    }
}
BENCHMARK(BM_History_Push);

// Benchmark push() when history is full (steady-state operation)
static void BM_History_PushFull(benchmark::State& state)
{
    Domain::History<double, 300> history;
    // Fill the history first
    for (size_t i = 0; i < 300; ++i)
    {
        history.push(static_cast<double>(i));
    }

    double value = 300.0;
    for (auto _ : state)
    {
        history.push(value);
        value += 0.1;
        benchmark::DoNotOptimize(history.size());
    }
}
BENCHMARK(BM_History_PushFull);

// Benchmark operator[] access - used when rendering graphs
static void BM_History_RandomAccess(benchmark::State& state)
{
    Domain::History<double, 300> history;
    for (size_t i = 0; i < 300; ++i)
    {
        history.push(static_cast<double>(i));
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(0, 299);

    for (auto _ : state)
    {
        const size_t index = dist(rng);
        benchmark::DoNotOptimize(history[index]);
    }
}
BENCHMARK(BM_History_RandomAccess);

// Benchmark sequential access - typical for graph rendering
static void BM_History_SequentialAccess(benchmark::State& state)
{
    Domain::History<double, 300> history;
    for (size_t i = 0; i < 300; ++i)
    {
        history.push(static_cast<double>(i));
    }

    for (auto _ : state)
    {
        double sum = 0.0;
        for (size_t i = 0; i < history.size(); ++i)
        {
            sum += history[i];
        }
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_History_SequentialAccess);

// Benchmark copyTo() - used for ImPlot rendering
static void BM_History_CopyTo(benchmark::State& state)
{
    Domain::History<double, 300> history;
    for (size_t i = 0; i < 300; ++i)
    {
        history.push(static_cast<double>(i));
    }

    std::array<double, 300> buffer{};

    for (auto _ : state)
    {
        const auto copied = history.copyTo(buffer.data(), buffer.size());
        benchmark::DoNotOptimize(copied);
        benchmark::DoNotOptimize(buffer.data());
    }
}
BENCHMARK(BM_History_CopyTo);

// Benchmark copyTo() with wrapped data (worst case)
static void BM_History_CopyToWrapped(benchmark::State& state)
{
    Domain::History<double, 300> history;
    // Push 450 values to ensure data wraps around
    for (size_t i = 0; i < 450; ++i)
    {
        history.push(static_cast<double>(i));
    }

    std::array<double, 300> buffer{};

    for (auto _ : state)
    {
        const auto copied = history.copyTo(buffer.data(), buffer.size());
        benchmark::DoNotOptimize(copied);
        benchmark::DoNotOptimize(buffer.data());
    }
}
BENCHMARK(BM_History_CopyToWrapped);

// Benchmark latest() - frequently called for current value display
static void BM_History_Latest(benchmark::State& state)
{
    Domain::History<double, 300> history;
    for (size_t i = 0; i < 300; ++i)
    {
        history.push(static_cast<double>(i));
    }

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(history.latest());
    }
}
BENCHMARK(BM_History_Latest);

// Benchmark with different history sizes
static void BM_History_PushVariableSize(benchmark::State& state)
{
    const auto historySize = static_cast<size_t>(state.range(0));

    // We can't change template size at runtime, so we use the largest
    // and simulate smaller by limiting operations
    Domain::History<double, 3600> history; // 1 hour at 1Hz

    double value = 0.0;
    for (auto _ : state)
    {
        if (history.size() >= historySize)
        {
            history.clear();
        }
        history.push(value);
        value += 0.1;
        benchmark::DoNotOptimize(history.size());
    }
}
BENCHMARK(BM_History_PushVariableSize)->Range(60, 3600)->Unit(benchmark::kNanosecond);

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

    Domain::History<LargeValue, 300> history;

    // Fill and overwrite multiple times
    for (auto _ : state)
    {
        LargeValue val{1.0, 2.0, 3.0, 100, 200};
        history.push(val);
        benchmark::DoNotOptimize(history.size());
    }

    auto endStats = BenchmarkUtils::readMemoryStats();

    // Report expected vs actual memory
    constexpr size_t expectedBytes = sizeof(LargeValue) * 300 + sizeof(Domain::History<LargeValue, 300>);
    state.counters["expected_bytes"] = benchmark::Counter(static_cast<double>(expectedBytes));

    if (startStats.valid() && endStats.valid())
    {
        auto growth = static_cast<std::int64_t>(endStats.vmRSS) - static_cast<std::int64_t>(startStats.vmRSS);
        state.counters["actual_growth_bytes"] = benchmark::Counter(static_cast<double>(growth));
    }
}
BENCHMARK(BM_History_MemoryFootprint)->Iterations(10000);

} // namespace
