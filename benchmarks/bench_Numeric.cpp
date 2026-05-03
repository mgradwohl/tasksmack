// Benchmarks for Domain/Numeric utility functions
//
// These benchmarks measure the performance of numeric conversion and
// clamping utilities that are called on every render frame for every
// visible row in the process table and every chart value.
// These are micro-benchmarks; the overhead per call should be near-zero.

#include "Domain/Numeric.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>

namespace
{

// =============================================================================
// toDouble Benchmarks
// =============================================================================

// Benchmark toDouble<int> - called for tick counter conversions
static void BM_Numeric_ToDouble_Int(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(-1000000, 1000000);

    for (auto _ : state)
    {
        const int value = dist(rng);
        benchmark::DoNotOptimize(Domain::Numeric::toDouble(value));
    }
}
BENCHMARK(BM_Numeric_ToDouble_Int);

// Benchmark toDouble<uint64_t> - called for OS counter conversions (most common)
static void BM_Numeric_ToDouble_UInt64(benchmark::State& state)
{
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<std::uint64_t> dist(0, UINT64_MAX / 2);

    for (auto _ : state)
    {
        const auto value = dist(rng);
        benchmark::DoNotOptimize(Domain::Numeric::toDouble(value));
    }
}
BENCHMARK(BM_Numeric_ToDouble_UInt64);

// Benchmark toDouble<float> - called for GPU and power metric conversions
static void BM_Numeric_ToDouble_Float(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-100.0F, 100.0F);

    for (auto _ : state)
    {
        const float value = dist(rng);
        benchmark::DoNotOptimize(Domain::Numeric::toDouble(value));
    }
}
BENCHMARK(BM_Numeric_ToDouble_Float);

// =============================================================================
// clampPercentToFloat Benchmarks
// =============================================================================

// Benchmark clampPercentToFloat in normal range (most common path)
static void BM_Numeric_ClampPercentToFloat_InRange(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 100.0);

    for (auto _ : state)
    {
        const double value = dist(rng);
        benchmark::DoNotOptimize(Domain::Numeric::clampPercentToFloat(value));
    }
}
BENCHMARK(BM_Numeric_ClampPercentToFloat_InRange);

// Benchmark clampPercentToFloat with out-of-range values (rare but must be fast)
static void BM_Numeric_ClampPercentToFloat_OutOfRange(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-200.0, 200.0);

    for (auto _ : state)
    {
        const double value = dist(rng);
        benchmark::DoNotOptimize(Domain::Numeric::clampPercentToFloat(value));
    }
}
BENCHMARK(BM_Numeric_ClampPercentToFloat_OutOfRange);

// =============================================================================
// narrowOr Benchmarks
// =============================================================================

// Benchmark narrowOr<int, int64_t> - most common usage for process counters
static void BM_Numeric_NarrowOr_InRange(benchmark::State& state)
{
    std::mt19937_64 rng(42);
    // Values that fit in int (typical case – 99.9% of process counters)
    std::uniform_int_distribution<std::int64_t> dist(0, 1000000);

    for (auto _ : state)
    {
        const std::int64_t value = dist(rng);
        benchmark::DoNotOptimize(Domain::Numeric::narrowOr<int>(value, -1));
    }
}
BENCHMARK(BM_Numeric_NarrowOr_InRange);

// Benchmark narrowOr<int, int64_t> with out-of-range values (overflow path)
static void BM_Numeric_NarrowOr_Overflow(benchmark::State& state)
{
    // Always overflow – worst case (branch misprediction cost)
    const std::int64_t largeValue = static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1000LL;

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(Domain::Numeric::narrowOr<int>(largeValue, -1));
    }
}
BENCHMARK(BM_Numeric_NarrowOr_Overflow);

// Benchmark narrowOr<uint8_t, int> - used for thread count / handle count narrowing
static void BM_Numeric_NarrowOr_UInt8(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 300); // Mix of in-range and overflow

    for (auto _ : state)
    {
        const int value = dist(rng);
        benchmark::DoNotOptimize(Domain::Numeric::narrowOr<std::uint8_t>(value, std::uint8_t{0}));
    }
}
BENCHMARK(BM_Numeric_NarrowOr_UInt8);

// =============================================================================
// Combined workload benchmark
// =============================================================================

// Simulate the numeric operations performed during a single process snapshot
// computation, which is the hot path called for every process every second.
static void BM_Numeric_ProcessSnapshotOperations(benchmark::State& state)
{
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<std::uint64_t> counterDist(0, 1ULL << 32);
    std::uniform_real_distribution<double> percentDist(0.0, 120.0);   // May be slightly out of range
    std::uniform_int_distribution<std::int64_t> pidDist(1, 4000000);

    for (auto _ : state)
    {
        // These conversions are called inside ProcessModel::computeSnapshot()
        const auto userTime = counterDist(rng);
        const auto sysTime = counterDist(rng);
        const auto rssBytes = counterDist(rng);

        const auto cpuPercent = percentDist(rng);
        const auto memPercent = percentDist(rng);

        const auto pid = pidDist(rng);

        // Typical operations in computeSnapshot
        const auto cpuF = Domain::Numeric::clampPercentToFloat(cpuPercent);
        const auto memF = Domain::Numeric::clampPercentToFloat(memPercent);
        const auto userD = Domain::Numeric::toDouble(userTime);
        const auto sysD = Domain::Numeric::toDouble(sysTime);
        const auto rssD = Domain::Numeric::toDouble(rssBytes);
        const auto pidI = Domain::Numeric::narrowOr<std::int32_t>(pid, -1);

        benchmark::DoNotOptimize(cpuF);
        benchmark::DoNotOptimize(memF);
        benchmark::DoNotOptimize(userD);
        benchmark::DoNotOptimize(sysD);
        benchmark::DoNotOptimize(rssD);
        benchmark::DoNotOptimize(pidI);
    }

    // Report throughput as process snapshots/second
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Numeric_ProcessSnapshotOperations);

// Simulate processing a full process table (N processes, each with numeric conversions)
static void BM_Numeric_ProcessTableWorkload(benchmark::State& state)
{
    const auto processCount = static_cast<size_t>(state.range(0));

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> percentDist(0.0, 100.0);
    std::uniform_int_distribution<std::uint64_t> counterDist(0, UINT32_MAX);

    // Pre-generate values to isolate numeric conversion cost from random generation
    std::vector<double> cpuValues(processCount);
    std::vector<double> memValues(processCount);
    std::vector<std::uint64_t> counterValues(processCount);

    for (size_t i = 0; i < processCount; ++i)
    {
        cpuValues[i] = percentDist(rng);
        memValues[i] = percentDist(rng);
        counterValues[i] = counterDist(rng);
    }

    for (auto _ : state)
    {
        for (size_t i = 0; i < processCount; ++i)
        {
            benchmark::DoNotOptimize(Domain::Numeric::clampPercentToFloat(cpuValues[i]));
            benchmark::DoNotOptimize(Domain::Numeric::clampPercentToFloat(memValues[i]));
            benchmark::DoNotOptimize(Domain::Numeric::toDouble(counterValues[i]));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(processCount));
}
// Typical visible row counts in the process table
BENCHMARK(BM_Numeric_ProcessTableWorkload)->Arg(50)->Arg(200)->Arg(500)->Arg(1000);

} // namespace
