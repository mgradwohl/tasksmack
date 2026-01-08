#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace Domain
{

/// Fixed-size ring buffer for storing time-series data.
/// Provides efficient append and contiguous access for plotting.
template<typename T, std::size_t Capacity> class History
{
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

} // namespace Domain
