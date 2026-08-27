#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace Domain
{

/// Fixed-size ring buffer for storing time-series data.
/// Provides efficient append and contiguous access for plotting.
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

/// Runtime-capacity ring buffer for storing time-series data.
///
/// Like History<T, N>, but the capacity is chosen at construction (and may be
/// changed later via setCapacity) so it can track user-configurable history
/// windows and refresh cadences without a compile-time worst-case allocation.
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

/// Convert a HistoryBuffer<T> to a vector in chronological order.
template<typename T> [[nodiscard]] std::vector<T> toVector(const HistoryBuffer<T>& history)
{
    std::vector<T> result(history.size());
    static_cast<void>(history.copyTo(result.data(), result.size()));
    return result;
}

/// Count leading timestamps strictly older than `cutoff`, then discard that
/// many entries from the timestamp ring and every aligned ring in O(1) each.
/// Returns the number of discarded entries.
template<typename... Buffers>
[[nodiscard]] std::size_t discardBefore(HistoryBuffer<double>& timestamps, double cutoff, Buffers&... alignedBuffers)
{
    std::size_t removeCount = 0;
    while (removeCount < timestamps.size() && timestamps.ref(removeCount) < cutoff)
    {
        ++removeCount;
    }

    timestamps.discardFront(removeCount);
    (alignedBuffers.discardFront(removeCount), ...);
    return removeCount;
}

} // namespace HistoryUtils

} // namespace Domain
