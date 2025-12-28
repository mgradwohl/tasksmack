#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace Domain
{

/// Fixed-size ring buffer for storing time-series data.
/// Provides efficient append and contiguous access for plotting.
template<typename T, size_t Capacity> class History
{
  public:
    /// Add a new value, overwriting oldest if full.
    void push(T value)
    {
        m_Data[m_WriteIndex] = value;
        m_WriteIndex = (m_WriteIndex + 1) % Capacity;
        if (m_Size < Capacity)
        {
            ++m_Size;
        }
    }

    /// Clear all data.
    void clear()
    {
        m_Size = 0;
        m_WriteIndex = 0;
    }

    /// Number of valid entries.
    [[nodiscard]] size_t size() const
    {
        return m_Size;
    }

    /// Maximum capacity.
    [[nodiscard]] static constexpr size_t capacity()
    {
        return Capacity;
    }

    /// Check if empty.
    [[nodiscard]] bool empty() const
    {
        return m_Size == 0;
    }

    /// Check if full.
    [[nodiscard]] bool full() const
    {
        return m_Size == Capacity;
    }

    /// Access element by logical index (0 = oldest, size()-1 = newest).
    [[nodiscard]] T operator[](size_t index) const
    {
        size_t readIndex = (m_WriteIndex + Capacity - m_Size + index) % Capacity;
        return m_Data[readIndex];
    }

    /// Get most recent value (or default if empty).
    [[nodiscard]] T latest() const
    {
        if (m_Size == 0)
        {
            return T{};
        }
        return m_Data[(m_WriteIndex + Capacity - 1) % Capacity];
    }

    /// Copy data into a contiguous buffer for plotting.
    /// Returns number of elements copied.
    size_t copyTo(T* buffer, size_t maxCount) const
    {
        const size_t count = std::min(maxCount, m_Size);
        if (count == 0)
        {
            return 0;
        }

        // Optimize: if data is not wrapped, use single memcpy-equivalent
        const size_t readStart = (m_WriteIndex + Capacity - m_Size) % Capacity;

        if (readStart + count <= Capacity)
        {
            // Data is contiguous in the ring buffer
            std::copy_n(m_Data.data() + readStart, count, buffer);
        }
        else
        {
            // Data wraps around: copy in two chunks
            const size_t firstChunk = Capacity - readStart;
            std::copy_n(m_Data.data() + readStart, firstChunk, buffer);
            std::copy_n(m_Data.data(), count - firstChunk, buffer + firstChunk);
        }

        return count;
    }

    /// Get raw data pointer (for advanced usage - not contiguous in ring order).
    [[nodiscard]] const T* data() const
    {
        return m_Data.data();
    }

  private:
    std::array<T, Capacity> m_Data{};
    size_t m_WriteIndex = 0;
    size_t m_Size = 0;
};

} // namespace Domain
