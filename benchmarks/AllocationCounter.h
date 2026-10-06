// Allocator-side telemetry for benchmarks: allocation count, cumulative bytes allocated, and live/peak-live
// bytes, as reported by hooks on the allocator itself.
//
// This is deliberately separate from MemoryTracker.h's RSS/VM readings. Those are what the OS reports for
// the whole process: RSS can stay flat while allocation churn drops (freed pages stay resident) and can
// grow without a single new allocation (first touch of already-reserved memory), so it says nothing about
// allocation count or rate (#879).
//
// Nothing records into this counter yet: it needs global operator new/delete overrides (or a custom
// allocator) calling recordAllocation()/recordDeallocation(), plus TaskSmackMemoryManager registered via
// benchmark::RegisterMemoryManager(). It has no Google Benchmark dependency so its logic is unit-tested
// in tests/Benchmarks/test_AllocationCounter.cpp.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace BenchmarkUtils
{

/// A point-in-time copy of an AllocationCounter's totals since its last reset().
struct AllocationTotals
{
    std::uint64_t allocationCount = 0;   ///< Allocations recorded
    std::uint64_t deallocationCount = 0; ///< Deallocations recorded
    std::uint64_t bytesAllocated = 0;    ///< Cumulative bytes allocated (never decreases)
    std::uint64_t bytesDeallocated = 0;  ///< Cumulative bytes deallocated (never decreases)
    std::int64_t liveBytes = 0;          ///< Bytes allocated minus bytes deallocated: net heap growth
    std::int64_t peakLiveBytes = 0;      ///< High-water mark of liveBytes: peak live allocation
};

/// Thread-safe allocation counter.
///
/// Live and peak-live bytes are relative to the last reset(): memory allocated before the reset and freed
/// after it drives liveBytes negative instead of being misattributed, and the peak never drops below the
/// zero the reset started from.
class AllocationCounter
{
  public:
    AllocationCounter() = default;

    /// The process-wide counter that allocator hooks record into.
    static auto instance() -> AllocationCounter&
    {
        static AllocationCounter counter;
        return counter;
    }

    // Atomics are neither copyable nor movable, and a copied counter would silently miss further records.
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;
    AllocationCounter(AllocationCounter&&) = delete;
    AllocationCounter& operator=(AllocationCounter&&) = delete;
    ~AllocationCounter() = default;

    void recordAllocation(std::size_t bytes)
    {
        const auto signedBytes = static_cast<std::int64_t>(bytes);
        m_AllocationCount.fetch_add(1, std::memory_order_relaxed);
        m_BytesAllocated.fetch_add(bytes, std::memory_order_relaxed);

        // fetch_add returns the live total immediately before this allocation, so live + bytes is exactly
        // the live total this allocation produced, even with other threads allocating and freeing
        // concurrently. Live bytes only rise on an allocation, so that is the only place the peak can move.
        const std::int64_t live = m_LiveBytes.fetch_add(signedBytes, std::memory_order_relaxed) + signedBytes;
        std::int64_t peak = m_PeakLiveBytes.load(std::memory_order_relaxed);
        while (live > peak && !m_PeakLiveBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed))
        {
            // compare_exchange_weak reloaded `peak`; retry while this allocation's live total is still higher.
        }
    }

    void recordDeallocation(std::size_t bytes)
    {
        m_DeallocationCount.fetch_add(1, std::memory_order_relaxed);
        m_BytesDeallocated.fetch_add(bytes, std::memory_order_relaxed);
        m_LiveBytes.fetch_sub(static_cast<std::int64_t>(bytes), std::memory_order_relaxed);
    }

    void reset()
    {
        m_AllocationCount.store(0, std::memory_order_relaxed);
        m_DeallocationCount.store(0, std::memory_order_relaxed);
        m_BytesAllocated.store(0, std::memory_order_relaxed);
        m_BytesDeallocated.store(0, std::memory_order_relaxed);
        m_LiveBytes.store(0, std::memory_order_relaxed);
        m_PeakLiveBytes.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] auto allocationCount() const -> std::uint64_t
    {
        return m_AllocationCount.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto deallocationCount() const -> std::uint64_t
    {
        return m_DeallocationCount.load(std::memory_order_relaxed);
    }
    /// Cumulative bytes allocated since reset() -- a churn measure, not a memory-use measure.
    [[nodiscard]] auto bytesAllocated() const -> std::uint64_t
    {
        return m_BytesAllocated.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto bytesDeallocated() const -> std::uint64_t
    {
        return m_BytesDeallocated.load(std::memory_order_relaxed);
    }
    /// Bytes currently allocated and not yet freed, relative to reset().
    [[nodiscard]] auto liveBytes() const -> std::int64_t
    {
        return m_LiveBytes.load(std::memory_order_relaxed);
    }
    /// Highest liveBytes() reached since reset().
    [[nodiscard]] auto peakLiveBytes() const -> std::int64_t
    {
        return m_PeakLiveBytes.load(std::memory_order_relaxed);
    }

    /// Copy every total. Each field is read separately, so a snapshot taken while other threads are still
    /// recording is not one atomic cut; take it once allocation activity has stopped.
    [[nodiscard]] auto snapshot() const -> AllocationTotals
    {
        return AllocationTotals{
            .allocationCount = allocationCount(),
            .deallocationCount = deallocationCount(),
            .bytesAllocated = bytesAllocated(),
            .bytesDeallocated = bytesDeallocated(),
            .liveBytes = liveBytes(),
            .peakLiveBytes = peakLiveBytes(),
        };
    }

  private:
    std::atomic<std::uint64_t> m_AllocationCount{0};
    std::atomic<std::uint64_t> m_DeallocationCount{0};
    std::atomic<std::uint64_t> m_BytesAllocated{0};
    std::atomic<std::uint64_t> m_BytesDeallocated{0};
    std::atomic<std::int64_t> m_LiveBytes{0};
    std::atomic<std::int64_t> m_PeakLiveBytes{0};
};

} // namespace BenchmarkUtils
