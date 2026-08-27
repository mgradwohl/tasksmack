#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace Domain
{

/// Capacity constants for fixed-size ring buffers.
/// Chosen to support typical history windows @ 1Hz sampling rate.
namespace HistoryCapacity
{
/// Standard history capacity: 1800 samples
/// At 1 Hz sampling rate: 1800 samples = 1800 seconds = 30 minutes (maximum retention)
/// Default history window: 300 seconds (5 minutes, via Sampling::HISTORY_SECONDS_DEFAULT)
///
/// Capacity justification:
/// - Provides 6× safety margin above default 300-second window
/// - Supports up to 30 minutes of continuous 1Hz data without loss
/// - For faster sampling rates, effective retention time decreases
///   (e.g., at 10Hz: 1800 samples = 180 seconds = 3 minutes)
/// - Users can increase duration via setMaxHistorySeconds(), but total
///   samples are capped at capacity (no allocation churn, fixed memory)
///
/// When ring buffer becomes full:
/// - New pushes overwrite oldest data automatically
/// - No allocation or deallocation occurs
/// - Memory footprint remains constant (~14.4 KB per History<double>)
///
/// To extend maximum retention window:
/// - Increase STANDARD to larger value (e.g., 3600 for 1 hour @ 1Hz)
/// - Requires recompilation
/// - Memory cost per increase: 8 bytes per sample for double, 4 for float
///   (e.g., 3600 samples: ~28.8 KB per History<double>)
constexpr std::size_t STANDARD = 1800;

/// Maximum supported samples (preallocated at compile time)
/// To support longer windows, recompile with larger constant
constexpr std::size_t MAXIMUM = 1800;
} // namespace HistoryCapacity

/// Fixed-size ring buffer for storing time-series data.
/// Provides efficient append and contiguous access for plotting.
///
/// Design rationale:
/// - Eliminates allocation churn: no allocations after construction
/// - Constant memory footprint regardless of collection duration
/// - O(1) push operation (modulo arithmetic only)
/// - O(n) copyTo() for contiguous access (needed for charting)
/// - O(1) random access via operator[]
///
/// Ring buffer semantics:
/// - push(value): Adds value at end, overwrites oldest if full
/// - size(): Returns number of valid elements (0 to Capacity)
/// - empty()/full(): Quick capacity checks
/// - operator[](i): Logical access (0=oldest, size()-1=newest)
/// - copyTo(): Efficiently copies chronological data to buffer
///
/// Capacity implications:
/// - At 1Hz sampling: 1800 samples = 30 minutes of history
/// - With default 300-second window: only 1/6 of capacity used
/// - No wasteful trimming operations needed
/// - History automatically maintains fixed window via auto-wrap
///
/// For consumers (SystemModel, ProcessModel, StorageModel):
/// - HistoryUtils::toVector() converts ring buffer to vector
/// - All history accessors return vectors (API stable)
/// - No breaking changes to public interfaces
template<typename T, std::size_t Capacity> class History
{
    static_assert(Capacity > 0, "History capacity must be greater than zero");

  public:
    /// Add a new value, overwriting oldest if full.
    void push(T value) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        m_Data[m_WriteIndex] = std::move(value);
        m_WriteIndex = (m_WriteIndex + 1) % Capacity;
        if (m_Size < Capacity)
        {
            ++m_Size;
        }
    }

    /// Clear all data.
    void clear() noexcept
    {
        m_Size = 0;
        m_WriteIndex = 0;
    }

    /// Number of valid entries.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Size;
    }

    /// Maximum capacity.
    [[nodiscard]] static constexpr std::size_t capacity() noexcept
    {
        return Capacity;
    }

    /// Check if empty.
    [[nodiscard]] bool empty() const noexcept
    {
        return m_Size == 0;
    }

    /// Check if full.
    [[nodiscard]] bool full() const noexcept
    {
        return m_Size == Capacity;
    }

    /// Access element by logical index (0 = oldest, size()-1 = newest).
    [[nodiscard]] T operator[](std::size_t index) const noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        const std::size_t readIndex = (m_WriteIndex + Capacity - m_Size + index) % Capacity;
        return m_Data[readIndex];
    }

    /// Access element by logical index without copying (0 = oldest, size()-1 = newest).
    [[nodiscard]] const T& ref(std::size_t index) const noexcept
    {
        const std::size_t readIndex = (m_WriteIndex + Capacity - m_Size + index) % Capacity;
        return m_Data[readIndex];
    }

    /// Get most recent value (or default if empty).
    [[nodiscard]] T latest() const noexcept(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_copy_constructible_v<T>)
    {
        if (m_Size == 0)
        {
            return T{};
        }
        return m_Data[(m_WriteIndex + Capacity - 1) % Capacity];
    }

    /// Copy data into a contiguous buffer for plotting.
    /// Returns number of elements copied.
    [[nodiscard]] std::size_t copyTo(T* buffer, std::size_t maxCount) const noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        const std::size_t count = std::min(maxCount, m_Size);
        if (count == 0)
        {
            return 0;
        }

        // Optimize: if data is not wrapped, use single memcpy-equivalent
        const std::size_t readStart = (m_WriteIndex + Capacity - m_Size) % Capacity;

        if (readStart + count <= Capacity)
        {
            // Data is contiguous in the ring buffer
            std::copy_n(m_Data.data() + readStart, count, buffer);
        }
        else
        {
            // Data wraps around: copy in two chunks
            const std::size_t firstChunk = Capacity - readStart;
            std::copy_n(m_Data.data() + readStart, firstChunk, buffer);
            std::copy_n(m_Data.data(), count - firstChunk, buffer + firstChunk);
        }

        return count;
    }

    /// Get raw data pointer (for advanced usage - not contiguous in ring order).
    [[nodiscard]] const T* data() const noexcept
    {
        return m_Data.data();
    }

  private:
    std::array<T, Capacity> m_Data{};
    std::size_t m_WriteIndex = 0;
    std::size_t m_Size = 0;
};

namespace HistoryUtils
{

/// Convert a ring buffer History<T, C> to a vector.
/// Returns all elements in chronological order (oldest to newest).
template<typename T, std::size_t Capacity> [[nodiscard]] std::vector<T> toVector(const History<T, Capacity>& history)
{
    std::vector<T> result;
    result.reserve(history.size());
    for (std::size_t i = 0; i < history.size(); ++i)
    {
        result.push_back(history[i]);
    }
    return result;
}

template<typename Sequence> void trimFront(Sequence& sequence, std::size_t count)
{
    while (count > 0 && !sequence.empty())
    {
        sequence.pop_front();
        --count;
    }
}

template<typename... Sequences> void trimFrontToSize(std::size_t targetSize, Sequences&... sequences)
{
    (trimFront(sequences, sequences.size() > targetSize ? sequences.size() - targetSize : 0), ...);
}

template<typename Sequence> void alignFrontToSize(Sequence& sequence, std::size_t targetSize)
{
    trimFrontToSize(targetSize, sequence);
    while (sequence.size() < targetSize)
    {
        sequence.push_front({});
    }
}

template<typename TimestampSequence, typename... Sequences>
[[nodiscard]] std::size_t trimBefore(TimestampSequence& timestamps, double cutoff, Sequences&... alignedSequences)
{
    std::size_t removeCount = 0;
    while (removeCount < timestamps.size() && timestamps[removeCount] < cutoff)
    {
        ++removeCount;
    }

    trimFront(timestamps, removeCount);
    (trimFront(alignedSequences, removeCount), ...);
    return removeCount;
}

template<typename... Sequences> [[nodiscard]] std::size_t minimumNonEmptySize(const Sequences&... sequences)
{
    std::size_t minimum = std::numeric_limits<std::size_t>::max();
    const auto updateMinimum = [&minimum](const auto& sequence)
    {
        if (!sequence.empty())
        {
            minimum = std::min(minimum, sequence.size());
        }
    };
    (updateMinimum(sequences), ...);
    return minimum;
}

template<typename Sequence> [[nodiscard]] auto toVector(const Sequence& sequence) -> std::vector<typename Sequence::value_type>
{
    return {sequence.begin(), sequence.end()};
}

} // namespace HistoryUtils

} // namespace Domain
