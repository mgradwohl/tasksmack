// Memory tracking utilities for benchmarks
//
// Provides mechanisms to track memory usage during benchmark execution:
// 1. Peak RSS (Resident Set Size) tracking via /proc/self/status
// 2. Allocation counting via custom MemoryManager
// 3. Simple memory delta measurement

#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

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

    // Lambda to parse /proc/self/status values - defined outside loop for clarity
    auto parseValue = [](std::string_view line, std::string_view prefix, std::uint64_t& target)
    {
        if (line.starts_with(prefix))
        {
            // Skip prefix and whitespace
            auto pos = line.find_first_of("0123456789", prefix.size());
            if (pos != std::string::npos)
            {
                // Value is in kB, convert to bytes
                try
                {
                    target = static_cast<std::uint64_t>(std::stoull(std::string(line.substr(pos)))) * 1024;
                }
                catch (const std::invalid_argument&)
                {
                    // Leave target unchanged on parse error
                }
                catch (const std::out_of_range&)
                {
                    // Leave target unchanged on overflow
                }
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
    {
    }

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
        state.counters["rss_mb"] = benchmark::Counter(static_cast<double>(stats.vmRSS) / (1024.0 * 1024.0));
        state.counters["heap_mb"] = benchmark::Counter(static_cast<double>(stats.vmData) / (1024.0 * 1024.0));
        state.counters["peak_rss_mb"] = benchmark::Counter(static_cast<double>(stats.vmHWM) / (1024.0 * 1024.0));
    }
}

/// Report memory delta as benchmark counters
/// Call with tracker created before the benchmark work
inline void reportMemoryDelta(benchmark::State& state, const MemoryDeltaTracker& tracker)
{
    auto rssDelta = tracker.rssDelta();
    auto peakDelta = tracker.peakRssDelta();

    // Report in KB for finer granularity on deltas
    state.counters["rss_delta_kb"] = benchmark::Counter(static_cast<double>(rssDelta) / 1024.0, benchmark::Counter::kDefaults);
    state.counters["peak_delta_kb"] = benchmark::Counter(static_cast<double>(peakDelta) / 1024.0, benchmark::Counter::kDefaults);
}

// =============================================================================
// Allocation Tracking MemoryManager
// =============================================================================
//
// This provides fine-grained allocation tracking by implementing
// benchmark::MemoryManager. It requires overriding global new/delete
// or using a custom allocator, which has overhead.
//
// For now, we use the simpler /proc/self/status approach above.
// The MemoryManager below can be enabled for more detailed tracking.

/// Thread-safe allocation counter
class AllocationCounter
{
  public:
    static auto instance() -> AllocationCounter&
    {
        static AllocationCounter counter;
        return counter;
    }

    // Singleton: prevent copying/moving
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;
    AllocationCounter(AllocationCounter&&) = delete;
    AllocationCounter& operator=(AllocationCounter&&) = delete;

    void recordAllocation(std::size_t bytes)
    {
        m_AllocationCount.fetch_add(1, std::memory_order_relaxed);
        m_BytesAllocated.fetch_add(bytes, std::memory_order_relaxed);
    }

    void recordDeallocation(std::size_t bytes)
    {
        m_DeallocationCount.fetch_add(1, std::memory_order_relaxed);
        m_BytesDeallocated.fetch_add(bytes, std::memory_order_relaxed);
    }

    void reset()
    {
        m_AllocationCount.store(0, std::memory_order_relaxed);
        m_DeallocationCount.store(0, std::memory_order_relaxed);
        m_BytesAllocated.store(0, std::memory_order_relaxed);
        m_BytesDeallocated.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] auto allocationCount() const -> std::uint64_t
    {
        return m_AllocationCount.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto deallocationCount() const -> std::uint64_t
    {
        return m_DeallocationCount.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto bytesAllocated() const -> std::uint64_t
    {
        return m_BytesAllocated.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto bytesDeallocated() const -> std::uint64_t
    {
        return m_BytesDeallocated.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto netBytesAllocated() const -> std::int64_t
    {
        return static_cast<std::int64_t>(bytesAllocated()) - static_cast<std::int64_t>(bytesDeallocated());
    }

  private:
    AllocationCounter() = default;

    std::atomic<std::uint64_t> m_AllocationCount{0};
    std::atomic<std::uint64_t> m_DeallocationCount{0};
    std::atomic<std::uint64_t> m_BytesAllocated{0};
    std::atomic<std::uint64_t> m_BytesDeallocated{0};
};

/// Custom MemoryManager for Google Benchmark
/// Note: This requires hooking new/delete to be useful.
/// See bench_main.cpp for optional global new/delete overrides.
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
        auto& counter = AllocationCounter::instance();
        result.num_allocs = static_cast<std::int64_t>(counter.allocationCount());
        result.max_bytes_used = static_cast<std::int64_t>(counter.bytesAllocated());
        // Note: net_heap_growth would require tracking live allocations
    }
};

} // namespace BenchmarkUtils
