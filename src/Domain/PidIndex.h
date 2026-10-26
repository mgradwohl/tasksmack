#pragma once

// PID -> row lookup for ProcessModel's process-tree build (#2981): a flat open-addressing table,
// rebuilt every refresh into storage it keeps, so a refresh whose process count fits the table it
// already has allocates nothing. The std::unordered_map it replaces allocated one node per process
// per refresh. Not thread-safe; its one owner (the sampling writer) serializes every call.
//
// A PID listed more than once maps to its last row, as std::unordered_map::operator[] assignment
// did. The table only grows to fit, with a load factor of at most 1/2, and shrinks back once it is
// more than SHRINK_FACTOR times larger than the refresh needs, so its retained memory stays bounded
// after a spike in the process count.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace Domain
{

class PidIndex
{
  public:
    /// find()'s result for a PID that is not in the table.
    static constexpr std::size_t NOT_FOUND = std::numeric_limits<std::size_t>::max();

    /// Maps each item's PID (@p pidOf(item)) to its position in @p items, the last position for a
    /// PID listed more than once. Allocates only when @p items does not fit the current table (or
    /// is far smaller than it); an allocation failure throws std::bad_alloc and leaves the table
    /// empty (every find() is NOT_FOUND), never holding the previous build's rows.
    template<std::ranges::sized_range Range, typename PidOf> void rebuild(const Range& items, PidOf pidOf)
    {
        prepare(static_cast<std::size_t>(std::ranges::size(items)));
        std::uint32_t row = 0;
        for (const auto& item : items)
        {
            insert(std::invoke(pidOf, item), row);
            ++row;
        }
    }

    /// The row rebuild() gave @p pid, or NOT_FOUND.
    [[nodiscard]] std::size_t find(std::int32_t pid) const noexcept
    {
        if (m_ActiveSlots == 0)
        {
            return NOT_FOUND;
        }
        for (std::size_t slot = slotOf(pid);; slot = (slot + 1) & (m_ActiveSlots - 1))
        {
            const Slot& entry = m_Slots[slot];
            if (entry.row == EMPTY_ROW)
            {
                return NOT_FOUND;
            }
            if (entry.pid == pid)
            {
                return entry.row;
            }
        }
    }

    /// Slots held (for tests: unchanged across rebuilds that fit means no allocation).
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return m_Slots.size();
    }

    /// Slots the last rebuild() used: a power of two at least twice its item count, 0 when empty.
    [[nodiscard]] std::size_t activeSlots() const noexcept
    {
        return m_ActiveSlots;
    }

    /// A table larger than this many times what a rebuild needs is reallocated at the needed size.
    static constexpr std::size_t SHRINK_FACTOR = 8;
    /// The smallest table, so a handful of processes never reallocates as the count wobbles.
    static constexpr std::size_t MIN_SLOTS = 16;

  private:
    static constexpr std::uint32_t EMPTY_ROW = std::numeric_limits<std::uint32_t>::max();
    /// Rows must fit below EMPTY_ROW, and twice the count (the table size) must fit a size_t.
    static constexpr std::size_t MAX_ITEMS = std::min(static_cast<std::size_t>(EMPTY_ROW) - 1, std::numeric_limits<std::size_t>::max() / 4);
    struct Slot
    {
        std::int32_t pid = 0;
        std::uint32_t row = EMPTY_ROW;
    };

    /// Sizes and clears the table for @p count items.
    void prepare(std::size_t count)
    {
        m_ActiveSlots = 0; // empty until the table is ready, so a throw below leaves no stale rows
        if (count > MAX_ITEMS)
        {
            throw std::length_error("PidIndex: too many processes");
        }
        const std::size_t needed = std::max(MIN_SLOTS, std::bit_ceil(count * 2));
        if (m_Slots.size() < needed || m_Slots.size() / SHRINK_FACTOR > needed)
        {
            std::vector<Slot> resized(needed); // may throw; m_Slots is untouched if it does
            m_Slots.swap(resized);
        }
        else
        {
            std::fill_n(m_Slots.begin(), needed, Slot{});
        }
        m_ActiveSlots = needed;
        m_Shift = static_cast<unsigned>(std::numeric_limits<std::uint64_t>::digits - std::countr_zero(needed));
    }

    /// Requires prepare() for at least as many items as are inserted, so a free slot always exists.
    void insert(std::int32_t pid, std::uint32_t row) noexcept
    {
        for (std::size_t slot = slotOf(pid);; slot = (slot + 1) & (m_ActiveSlots - 1))
        {
            Slot& entry = m_Slots[slot];
            if (entry.row == EMPTY_ROW || entry.pid == pid)
            {
                entry.pid = pid;
                entry.row = row; // a repeated PID's later row replaces the earlier one
                return;
            }
        }
    }

    /// Fibonacci hashing: the multiply spreads PIDs that differ only in their low bits (Windows
    /// PIDs are multiples of 4; Linux PIDs are mostly consecutive) across the table's top bits.
    [[nodiscard]] std::size_t slotOf(std::int32_t pid) const noexcept
    {
        constexpr std::uint64_t GOLDEN_RATIO_64 = 0x9E37'79B9'7F4A'7C15ULL;
        const std::uint64_t mixed = static_cast<std::uint64_t>(static_cast<std::uint32_t>(pid)) * GOLDEN_RATIO_64;
        return static_cast<std::size_t>(mixed >> m_Shift);
    }

    std::vector<Slot> m_Slots;
    std::size_t m_ActiveSlots = 0; // a power of two, or 0 before the first rebuild()
    unsigned m_Shift = 0;          // 64 - log2(m_ActiveSlots)
};

} // namespace Domain
