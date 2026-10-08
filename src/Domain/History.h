#pragma once

#include "SharedHistory.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace Domain
{

/// Runtime-capacity ring buffer for storing time-series data.
///
/// The capacity is chosen at construction (and may be changed later via
/// setCapacity) so it can track user-configurable history windows and refresh
/// cadences without a compile-time worst-case allocation.
///
/// Design rationale:
/// - Single backing allocation; no per-sample allocation churn
/// - O(1) push (auto-evicts oldest when full)
/// - O(1) discardFront(count) for time-window trimming (advances the logical
///   start index; no element copies or buffer rebuilds)
/// - copyTo()/toVector() produce chronological contiguous output in at most
///   two std::copy_n chunks (cheap publication snapshots)
template<typename T> class HistoryBuffer
{
  public:
    HistoryBuffer() = default;

    explicit HistoryBuffer(std::size_t capacity)
    {
        setCapacity(capacity);
    }

    /// Change the capacity, preserving the newest min(size, capacity) elements.
    /// Allocates only when the capacity actually changes (user reconfiguration).
    void setCapacity(std::size_t capacity)
    {
        const std::size_t newCapacity = std::max<std::size_t>(capacity, 1);
        if (newCapacity == m_Data.size())
        {
            return;
        }

        std::vector<T> newData(newCapacity);
        const std::size_t keepCount = std::min(m_Size, newCapacity);
        const std::size_t dropCount = m_Size - keepCount;
        for (std::size_t i = 0; i < keepCount; ++i)
        {
            newData[i] = std::move(m_Data[physicalIndex(dropCount + i)]);
        }
        m_Data = std::move(newData);
        m_Start = 0;
        m_Size = keepCount;
    }

    /// Add a new value, overwriting oldest if full.
    void push(T value)
    {
        if (m_Data.empty())
        {
            m_Data.resize(1);
        }

        if (m_Size == m_Data.size())
        {
            m_Data[m_Start] = std::move(value);
            m_Start = (m_Start + 1) % m_Data.size();
        }
        else
        {
            m_Data[physicalIndex(m_Size)] = std::move(value);
            ++m_Size;
        }
    }

    /// Discard the oldest `count` elements in O(1) per call (no copies).
    void discardFront(std::size_t count) noexcept
    {
        const std::size_t removeCount = std::min(count, m_Size);
        if (removeCount == 0)
        {
            return;
        }
        m_Start = (m_Start + removeCount) % m_Data.size();
        m_Size -= removeCount;
    }

    /// Clear all data.
    void clear() noexcept
    {
        m_Start = 0;
        m_Size = 0;
    }

    /// Number of valid entries.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Size;
    }

    /// Maximum capacity.
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return m_Data.size();
    }

    /// Check if empty.
    [[nodiscard]] bool empty() const noexcept
    {
        return m_Size == 0;
    }

    /// Check if full.
    [[nodiscard]] bool full() const noexcept
    {
        return !m_Data.empty() && m_Size == m_Data.size();
    }

    /// Access element by logical index (0 = oldest, size()-1 = newest).
    [[nodiscard]] T operator[](std::size_t index) const
    {
        return m_Data[physicalIndex(index)];
    }

    /// Access element by logical index without copying (0 = oldest, size()-1 = newest).
    [[nodiscard]] const T& ref(std::size_t index) const noexcept
    {
        return m_Data[physicalIndex(index)];
    }

    /// Get most recent value (or default if empty).
    [[nodiscard]] T latest() const
    {
        if (m_Size == 0)
        {
            return T{};
        }
        return m_Data[physicalIndex(m_Size - 1)];
    }

    /// Copy data into a contiguous buffer for plotting.
    /// Returns number of elements copied.
    [[nodiscard]] std::size_t copyTo(T* buffer, std::size_t maxCount) const
    {
        const std::size_t count = std::min(maxCount, m_Size);
        if (count == 0)
        {
            return 0;
        }

        const std::size_t bufferCapacity = m_Data.size();
        if (m_Start + count <= bufferCapacity)
        {
            std::copy_n(m_Data.data() + m_Start, count, buffer);
        }
        else
        {
            const std::size_t firstChunk = bufferCapacity - m_Start;
            std::copy_n(m_Data.data() + m_Start, firstChunk, buffer);
            std::copy_n(m_Data.data(), count - firstChunk, buffer + firstChunk);
        }

        return count;
    }

  private:
    [[nodiscard]] std::size_t physicalIndex(std::size_t logicalIndex) const noexcept
    {
        return (m_Start + logicalIndex) % m_Data.size();
    }

    std::vector<T> m_Data;
    std::size_t m_Start = 0;
    std::size_t m_Size = 0;
};

namespace HistoryUtils
{

/// Convert a HistoryBuffer<T> to a vector in chronological order.
template<typename T> [[nodiscard]] std::vector<T> toVector(const HistoryBuffer<T>& history)
{
    std::vector<T> result(history.size());
    static_cast<void>(history.copyTo(result.data(), result.size()));
    return result;
}

/// Convert a SharedHistoryBuffer<T> to a vector in chronological order.
template<typename T> [[nodiscard]] std::vector<T> toVector(const SharedHistoryBuffer<T>& history)
{
    std::vector<T> result(history.size());
    static_cast<void>(history.copyTo(result.data(), result.size()));
    return result;
}

/// Whether a trim keeps `anchor`, the newest sample before `cutoff`, given `next`, the oldest sample
/// at or after it, and `newest`, the newest sample overall.
///
/// The anchor sits just before the window's left edge, so a chart's line runs off the edge of the
/// axis instead of starting a fraction of a refresh interval inside it and leaving an empty strip
/// after every trim (#1016). That only holds while the anchor is the sample just before the window:
/// a step from it to `next` longer than the window itself (newest - cutoff) is a gap -- sampling
/// paused, or the source was absent -- and keeping it would draw the old value connected across the
/// gap. So it is kept only when the step is no longer than the window.
[[nodiscard]] inline bool keepTrimAnchor(double anchor, double next, double cutoff, double newest) noexcept
{
    const double window = newest - cutoff;
    return (next - anchor) <= window;
}

/// How many leading entries of `timestamps` (oldest first) a trim to `cutoff` removes: every entry
/// before the cutoff except the newest of them when keepTrimAnchor() says to keep it, which needs a
/// newer sample to remain. The same rule as discardBefore(), for contiguous histories.
[[nodiscard]] inline std::size_t trimCountBefore(std::span<const double> timestamps, double cutoff) noexcept
{
    const auto firstInWindow = std::ranges::find_if(timestamps, [cutoff](double t) { return t >= cutoff; });
    const auto keepFrom = static_cast<std::size_t>(firstInWindow - timestamps.begin());
    if (keepFrom > 0 && keepFrom < timestamps.size() &&
        keepTrimAnchor(timestamps[keepFrom - 1], timestamps[keepFrom], cutoff, timestamps.back()))
    {
        return keepFrom - 1;
    }
    return keepFrom;
}

/// Discard the leading entries older than `cutoff` from the timestamp ring and every aligned ring,
/// in O(1) each -- all but the newest of them when keepTrimAnchor() says to keep it, which needs a
/// newer sample to remain.
/// Returns the number of discarded entries. Works on HistoryBuffer and SharedHistoryBuffer alike.
template<typename TimestampBuffer, typename... Buffers>
[[nodiscard]] std::size_t discardBefore(TimestampBuffer& timestamps, double cutoff, Buffers&... alignedBuffers)
{
    std::size_t removeCount = 0;
    while (removeCount < timestamps.size() && timestamps.ref(removeCount) < cutoff)
    {
        ++removeCount;
    }
    if (removeCount > 0 && removeCount < timestamps.size() &&
        keepTrimAnchor(timestamps.ref(removeCount - 1), timestamps.ref(removeCount), cutoff, timestamps.latest()))
    {
        --removeCount; // keep the newest sample before the cutoff (see keepTrimAnchor)
    }

    timestamps.discardFront(removeCount);
    (alignedBuffers.discardFront(removeCount), ...);
    return removeCount;
}

} // namespace HistoryUtils

} // namespace Domain
