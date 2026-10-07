// Memory tracking utilities for benchmarks
//
// Two separate kinds of memory signal, reported under separate names:
// 1. OS-reported process memory (RSS, peak RSS, VM sizes) from /proc/self/status, plus RSS deltas.
//    These are what the kernel says the whole process occupies; they are NOT evidence of how much
//    or how often code allocates (see AllocationCounter.h, #879).
// 2. Allocator telemetry (allocation count, cumulative bytes, peak live bytes) through
//    TaskSmackMemoryManager and AllocationCounter. Dormant: no allocator hook records into it yet.

#pragma once

#include "AllocationCounter.h"

#include <benchmark/benchmark.h>

#include <charconv>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace BenchmarkUtils
{

/// Read current memory stats from /proc/self/status (Linux)
/// Returns values in bytes
struct MemoryStats
{
    std::uint64_t vmPeak = 0; ///< Peak virtual memory size
    std::uint64_t vmSize = 0; ///< Current virtual memory size
    std::uint64_t vmRSS = 0;  ///< Resident set size (physical memory)
    std::uint64_t vmHWM = 0;  ///< Peak resident set size (high water mark)
    std::uint64_t vmData = 0; ///< Data segment size (heap)
    std::uint64_t vmStk = 0;  ///< Stack size

    [[nodiscard]] bool valid() const
    {
        return vmRSS > 0;
    }
};

/// Read memory stats from /proc/self/status
[[nodiscard]] inline auto readMemoryStats() -> MemoryStats
{
    MemoryStats stats;

#ifdef __linux__
    std::ifstream status("/proc/self/status");
    if (!status.is_open())
    {
        return stats;
    }

    std::string line;

    // Lambda to parse /proc/self/status values - defined once outside the loop to avoid
    // recreating the callable each iteration and to keep the hot loop body minimal
    auto parseValue = [](std::string_view line, std::string_view prefix, std::uint64_t& target)
    {
        if (line.starts_with(prefix))
        {
            // Skip prefix and whitespace
            auto pos = line.find_first_of("0123456789", prefix.size());
            if (pos != std::string::npos)
            {
                // Value is in kB, convert to bytes
                // Use std::from_chars for efficiency - works directly with string_view
                std::uint64_t value = 0;
                auto sub = line.substr(pos);
                auto [ptr, ec] = std::from_chars(sub.data(), sub.data() + sub.size(), value);
                if (ec == std::errc{})
                {
                    target = value * 1024;
                }
                // On parse error (ec != errc{}), leave target unchanged.
                // /proc/self/status is a best-effort data source; malformed lines
                // should not affect benchmark results.
            }
        }
    };

    while (std::getline(status, line))
    {
        // Parse lines like "VmRSS:     12345 kB"
        parseValue(line, "VmPeak:", stats.vmPeak);
        parseValue(line, "VmSize:", stats.vmSize);
        parseValue(line, "VmRSS:", stats.vmRSS);
        parseValue(line, "VmHWM:", stats.vmHWM);
        parseValue(line, "VmData:", stats.vmData);
        parseValue(line, "VmStk:", stats.vmStk);
    }
#endif

    return stats;
}

/// RAII helper to measure memory change during a scope
class MemoryDeltaTracker
{
  public:
    MemoryDeltaTracker() : m_StartStats(readMemoryStats())
    {}

    /// Get memory stats at start
    [[nodiscard]] auto startStats() const -> const MemoryStats&
    {
        return m_StartStats;
    }

    /// Get current memory stats
    [[nodiscard]] static auto currentStats() -> MemoryStats
    {
        return readMemoryStats();
    }

    /// Get delta in RSS since construction
    [[nodiscard]] auto rssDelta() const -> std::int64_t
    {
        auto current = readMemoryStats();
        return static_cast<std::int64_t>(current.vmRSS) - static_cast<std::int64_t>(m_StartStats.vmRSS);
    }

    /// Get peak RSS delta (high water mark increase)
    [[nodiscard]] auto peakRssDelta() const -> std::int64_t
    {
        auto current = readMemoryStats();
        return static_cast<std::int64_t>(current.vmHWM) - static_cast<std::int64_t>(m_StartStats.vmHWM);
    }

  private:
    MemoryStats m_StartStats;
};

/// Report memory stats as benchmark counters
/// Call this at the end of a benchmark to report memory usage
inline void reportMemoryCounters(benchmark::State& state)
{
    auto stats = readMemoryStats();
    if (stats.valid())
    {
        // Report in MiB for readability (values already converted from kB to bytes, then to MiB)
        state.counters["rss_mb"] = benchmark::Counter((static_cast<double>(stats.vmRSS)) / (1024.0 * 1024.0));
        state.counters["heap_mb"] = benchmark::Counter((static_cast<double>(stats.vmData)) / (1024.0 * 1024.0));
        state.counters["peak_rss_mb"] = benchmark::Counter((static_cast<double>(stats.vmHWM)) / (1024.0 * 1024.0));
    }
}

/// Report memory delta as benchmark counters
/// Call with tracker created before the benchmark work.
/// On platforms where /proc/self/status is unavailable (e.g. Windows),
/// no counters are emitted rather than publishing misleading zero values.
inline void reportMemoryDelta(benchmark::State& state, const MemoryDeltaTracker& tracker)
{
    if (!tracker.startStats().valid())
    {
        return;
    }

    auto rssDelta = tracker.rssDelta();
    auto peakDelta = tracker.peakRssDelta();

    // Report in KB for finer granularity on deltas
    state.counters["rss_delta_kb"] = benchmark::Counter((static_cast<double>(rssDelta)) / 1024.0, benchmark::Counter::kDefaults);
    state.counters["peak_delta_kb"] = benchmark::Counter((static_cast<double>(peakDelta)) / 1024.0, benchmark::Counter::kDefaults);
}

// =============================================================================
// Allocation Tracking MemoryManager
// =============================================================================
//
// Reports AllocationCounter's totals through Google Benchmark's MemoryManager
// interface, which prints them under its own allocation fields -- never mixed
// with the RSS counters above. Dormant: it is not registered
// (benchmark::RegisterMemoryManager) and nothing records into
// AllocationCounter, since that needs global operator new/delete overrides,
// which add overhead to every timed benchmark.

/// Custom MemoryManager for Google Benchmark
class TaskSmackMemoryManager : public benchmark::MemoryManager
{
  public:
    void Start() override
    {
        // Reset counters at start of benchmark
        AllocationCounter::instance().reset();
    }

    void Stop(Result& result) override
    {
        const auto totals = AllocationCounter::instance().snapshot();
        result.num_allocs = static_cast<std::int64_t>(totals.allocationCount);
        // Peak *live* bytes, which is what max_bytes_used means. This used to
        // report the cumulative total allocated, which grows with every allocation
        // even when each is freed straight away (#879).
        result.max_bytes_used = totals.peakLiveBytes;
        result.total_allocated_bytes = static_cast<std::int64_t>(totals.bytesAllocated);
        result.net_heap_growth = totals.liveBytes;
    }
};

} // namespace BenchmarkUtils
