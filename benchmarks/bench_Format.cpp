// Benchmarks for UI/Format functions
//
// These benchmarks measure the performance of formatting functions used
// extensively in the UI for displaying values. These are called every frame
// for every visible row in tables.

#include "UI/Format.h"

#include <random>
#include <vector>

#include <benchmark/benchmark.h>

namespace
{

// Benchmark formatBytes() - called for memory columns
static void BM_Format_FormatBytes(benchmark::State& state)
{
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(0, (1ULL << 40)); // Up to 1TB

    for (auto _ : state)
    {
        const auto bytes = dist(rng);
        auto result = UI::Format::formatBytes(static_cast<double>(bytes));
        benchmark::DoNotOptimize(result.data());
    }
}
BENCHMARK(BM_Format_FormatBytes);

// Benchmark formatBytesPerSec() - called for I/O rate columns
static void BM_Format_FormatBytesPerSec(benchmark::State& state)
{
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 1e9); // Up to 1GB/s

    for (auto _ : state)
    {
        const auto rate = dist(rng);
        auto result = UI::Format::formatBytesPerSec(rate);
        benchmark::DoNotOptimize(result.data());
    }
}
BENCHMARK(BM_Format_FormatBytesPerSec);

// Benchmark percentCompact() - called for CPU% column
static void BM_Format_PercentCompact(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 100.0);

    for (auto _ : state)
    {
        const auto percent = dist(rng);
        auto result = UI::Format::percentCompact(percent);
        benchmark::DoNotOptimize(result.data());
    }
}
BENCHMARK(BM_Format_PercentCompact);

// Benchmark formatCpuTimeCompact() - called for TIME+ column
static void BM_Format_FormatCpuTimeCompact(benchmark::State& state)
{
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(0, 86400ULL * 100); // Up to 100 days in seconds

    for (auto _ : state)
    {
        const auto seconds = dist(rng);
        auto result = UI::Format::formatCpuTimeCompact(static_cast<double>(seconds));
        benchmark::DoNotOptimize(result.data());
    }
}
BENCHMARK(BM_Format_FormatCpuTimeCompact);

// Benchmark formatIntLocalized() - called for PID, thread count columns
static void BM_Format_FormatIntLocalized(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_int_distribution<int64_t> dist(0, 1000000);

    for (auto _ : state)
    {
        const auto value = dist(rng);
        auto result = UI::Format::formatIntLocalized(value);
        benchmark::DoNotOptimize(result.data());
    }
}
BENCHMARK(BM_Format_FormatIntLocalized);

// Benchmark splitBytesForAlignment() - called for aligned byte display
static void BM_Format_SplitBytesForAlignment(benchmark::State& state)
{
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(0, (1ULL << 40));

    for (auto _ : state)
    {
        const auto bytes = static_cast<double>(dist(rng));
        const auto unit = UI::Format::chooseByteUnit(bytes);
        auto parts = UI::Format::splitBytesForAlignment(bytes, unit);
        benchmark::DoNotOptimize(parts.wholePart.data());
        benchmark::DoNotOptimize(parts.decimalPart.data());
        benchmark::DoNotOptimize(parts.unitPart.data());
    }
}
BENCHMARK(BM_Format_SplitBytesForAlignment);

// Benchmark chooseByteUnit() - called to determine unit for byte values
static void BM_Format_ChooseByteUnit(benchmark::State& state)
{
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(0, (1ULL << 50));

    for (auto _ : state)
    {
        const auto bytes = static_cast<double>(dist(rng));
        auto unit = UI::Format::chooseByteUnit(bytes);
        benchmark::DoNotOptimize(unit);
    }
}
BENCHMARK(BM_Format_ChooseByteUnit);

// Benchmark formatOrDash() with non-zero values
static void BM_Format_FormatOrDash_WithValue(benchmark::State& state)
{
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(1, (1ULL << 30));

    for (auto _ : state)
    {
        const auto value = dist(rng);
        auto result = UI::Format::formatOrDash(value, UI::Format::formatBytes);
        benchmark::DoNotOptimize(result.data());
    }
}
BENCHMARK(BM_Format_FormatOrDash_WithValue);

// Benchmark formatOrDash() with zero values (fast path)
static void BM_Format_FormatOrDash_WithZero(benchmark::State& state)
{
    for (auto _ : state)
    {
        auto result = UI::Format::formatOrDash(0ULL, UI::Format::formatBytes);
        benchmark::DoNotOptimize(result.data());
    }
}
BENCHMARK(BM_Format_FormatOrDash_WithZero);

// Simulate formatting a full process table row
// This represents real-world usage where multiple formats are called per row
static void BM_Format_FullProcessRow(benchmark::State& state)
{
    // Simulate typical process values
    const std::int64_t pid = 12345;
    const double cpuPercent = 25.3;
    const double memPercent = 12.7;
    const uint64_t rssBytes = ((1024ULL * 1024) * 512);    // 512MB
    const uint64_t virtBytes = ((1024ULL * 1024) * 2048);  // 2GB
    const uint64_t sharedBytes = ((1024ULL * 1024) * 128); // 128MB
    const uint64_t cpuTimeSeconds = 3661;                  // 1h 1m 1s
    const std::int32_t threadCount = 24;
    const double ioReadRate = ((1024.0 * 1024) * 50);  // 50MB/s
    const double ioWriteRate = ((1024.0 * 1024) * 10); // 10MB/s

    for (auto _ : state)
    {
        // Format all columns as done in ProcessesPanel
        auto pidStr = UI::Format::formatId(pid);
        auto cpuStr = UI::Format::percentCompact(cpuPercent);
        auto memStr = UI::Format::percentCompact(memPercent);
        auto rssStr = UI::Format::formatBytes(static_cast<double>(rssBytes));
        auto virtStr = UI::Format::formatBytes(static_cast<double>(virtBytes));
        auto shrStr = UI::Format::formatBytes(static_cast<double>(sharedBytes));
        auto timeStr = UI::Format::formatCpuTimeCompact(static_cast<double>(cpuTimeSeconds));
        auto threadsStr = UI::Format::formatIntLocalized(threadCount);
        auto ioReadStr = UI::Format::formatBytesPerSec(ioReadRate);
        auto ioWriteStr = UI::Format::formatBytesPerSec(ioWriteRate);

        benchmark::DoNotOptimize(pidStr.data());
        benchmark::DoNotOptimize(cpuStr.data());
        benchmark::DoNotOptimize(memStr.data());
        benchmark::DoNotOptimize(rssStr.data());
        benchmark::DoNotOptimize(virtStr.data());
        benchmark::DoNotOptimize(shrStr.data());
        benchmark::DoNotOptimize(timeStr.data());
        benchmark::DoNotOptimize(threadsStr.data());
        benchmark::DoNotOptimize(ioReadStr.data());
        benchmark::DoNotOptimize(ioWriteStr.data());
    }

    // Report throughput as rows/second
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Format_FullProcessRow);

// Benchmark with different numbers of processes to simulate table rendering
static void BM_Format_ProcessTable(benchmark::State& state)
{
    const auto processCount = static_cast<size_t>(state.range(0));

    // Pre-generate random values
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> cpuDist(0.0, 100.0);
    std::uniform_int_distribution<uint64_t> memDist((1024 * 1024), (((1024ULL * 1024) * 1024) * 16));

    std::vector<double> cpuValues(processCount);
    std::vector<uint64_t> memValues(processCount);

    for (size_t i = 0; i < processCount; ++i)
    {
        cpuValues[i] = cpuDist(rng);
        memValues[i] = memDist(rng);
    }

    for (auto _ : state)
    {
        for (size_t i = 0; i < processCount; ++i)
        {
            auto cpuStr = UI::Format::percentCompact(cpuValues[i]);
            auto memStr = UI::Format::formatBytes(static_cast<double>(memValues[i]));
            benchmark::DoNotOptimize(cpuStr.data());
            benchmark::DoNotOptimize(memStr.data());
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(processCount));
}
// Typical visible row counts: 20, 50, 100, 500, 1000 processes
BENCHMARK(BM_Format_ProcessTable)->Arg(20)->Arg(50)->Arg(100)->Arg(500)->Arg(1000);

} // namespace
