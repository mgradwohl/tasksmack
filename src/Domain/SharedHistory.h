#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Domain
{

// Shared history for model publications (#1412).
//
// A publication used to deep-copy every series it held, so each publish was O(history x series):
// 18k samples per series at the longest window and fastest refresh. Here a series' samples live in an
// append-only block that the model and every publication share. A publish copies one shared_ptr and
// two offsets per series -- O(series), whatever the history length -- and a reader still gets plain
// contiguous memory, so chart code keeps using std::span and the existing reduction path unchanged.
//
// Why this and not the chunked list #1412 proposed: a list of fixed-size chunks makes a publish copy
// one shared_ptr per chunk (still O(history / chunk) per series, ~70 refcount bumps per series at
// 18k with 256-sample chunks) and makes the data non-contiguous, so every chart's span-based
// reduction would need a chunk-aware path. A copy-on-write ring would need readers to copy to keep
// data across frames. An append-only block gives O(1) per series and contiguous reads.
//
// The one invariant everything rests on: a slot of a block is written once, before any view covers
// it, and never again. The writer only ever appends after the newest sample (trimming advances the
// start, clear() too), so a view's slots -- all older than the write position when the view was taken
// -- never change while the view exists, and readers on other threads need no lock to read them. When
// a block fills up, the live samples move to a new block (compaction) and the old one is freed when
// its last view goes. Blocks are sized at about twice the live samples (capped at twice the capacity),
// so each compaction's copy is paid for by at least as many appends: O(1) amortised per sample.

/// An immutable, contiguous view of one published history series, oldest sample first. Holding it
/// keeps its samples alive and unchanged, whatever the model appends or trims afterwards. Cheap to
/// copy (one shared_ptr). Converts to std::span<const T>, and is a contiguous range, so UI code takes
/// it as a span and never sees the storage behind it.
template<typename T> class HistoryView
{
  public:
    using Iterator = std::span<const T>::iterator;
    using ReverseIterator = std::reverse_iterator<Iterator>;

    HistoryView() noexcept = default;

    /// A view of @p values, owned by @p owner (normally from SharedHistoryBuffer::view()).
    HistoryView(std::shared_ptr<const std::vector<T>> owner, std::span<const T> values) noexcept
        : m_Owner(std::move(owner)), m_Values(values)
    {}

    [[nodiscard]] std::span<const T> span() const noexcept
    {
        return m_Values;
    }
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions) - a view is a span by design
    operator std::span<const T>() const noexcept
    {
        return m_Values;
    }

    [[nodiscard]] const T* data() const noexcept
    {
        return m_Values.data();
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Values.size();
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return m_Values.empty();
    }
    [[nodiscard]] Iterator begin() const noexcept
    {
        return m_Values.begin();
    }
    [[nodiscard]] Iterator end() const noexcept
    {
        return m_Values.end();
    }
    [[nodiscard]] ReverseIterator rbegin() const noexcept
    {
        return ReverseIterator(end());
    }
    [[nodiscard]] ReverseIterator rend() const noexcept
    {
        return ReverseIterator(begin());
    }
    [[nodiscard]] const T& operator[](std::size_t index) const noexcept
    {
        return m_Values[index];
    }
    [[nodiscard]] const T& front() const noexcept
    {
        return m_Values.front();
    }
    [[nodiscard]] const T& back() const noexcept
    {
        return m_Values.back();
    }

    /// Whether both views read the same block: consecutive publications share their samples (#1412).
    [[nodiscard]] bool sharesStorageWith(const HistoryView& other) const noexcept
    {
        return m_Owner != nullptr && m_Owner == other.m_Owner;
    }

  private:
    std::shared_ptr<const std::vector<T>> m_Owner;
    std::span<const T> m_Values;
};

/// The model's side of a shared history series: the HistoryBuffer interface (push, discardFront,
/// capacity, indexed reads), plus view() to publish the current samples without copying them.
///
/// Not thread-safe by itself: one writer, as the models already serialise. Views it hands out may be
/// read on any thread at any time, with no lock (see the invariant above).
template<typename T> class SharedHistoryBuffer
{
  public:
    SharedHistoryBuffer() = default;

    explicit SharedHistoryBuffer(std::size_t capacity) noexcept
    {
        setCapacity(capacity);
    }

    ~SharedHistoryBuffer() = default;

    // Not copyable: two writers appending to one block would write slots a view already covers.
    SharedHistoryBuffer(const SharedHistoryBuffer&) = delete;
    SharedHistoryBuffer& operator=(const SharedHistoryBuffer&) = delete;

    // Movable (containers of series grow): the source is left empty, owning no block.
    SharedHistoryBuffer(SharedHistoryBuffer&& other) noexcept
        : m_Block(std::move(other.m_Block)),
          m_Slots(std::exchange(other.m_Slots, {})),
          m_Start(std::exchange(other.m_Start, 0)),
          m_Size(std::exchange(other.m_Size, 0)),
          m_Capacity(other.m_Capacity)
    {}
    SharedHistoryBuffer& operator=(SharedHistoryBuffer&& other) noexcept
    {
        if (this != &other)
        {
            m_Block = std::move(other.m_Block);
            m_Slots = std::exchange(other.m_Slots, {});
            m_Start = std::exchange(other.m_Start, 0);
            m_Size = std::exchange(other.m_Size, 0);
            m_Capacity = other.m_Capacity;
        }
        return *this;
    }

    /// Change the most samples kept (at least 1), discarding the oldest beyond it. Never allocates:
    /// the block is resized at its next compaction.
    void setCapacity(std::size_t capacity) noexcept
    {
        m_Capacity = std::max<std::size_t>(capacity, 1);
        if (m_Size > m_Capacity)
        {
            discardFront(m_Size - m_Capacity);
        }
    }

    /// Make room so the next @p count appends don't allocate, compacting into a new block if this one
    /// is full. Throws std::bad_alloc with nothing changed. Callers appending to several aligned
    /// series reserve them all first, so the appends themselves cannot fail half way (#1412).
    void reserve(std::size_t count)
    {
        if (m_Start + m_Size + count <= m_Slots.size())
        {
            return;
        }
        // Twice the live samples, at least MIN_BLOCK, at most twice the capacity -- but always room
        // for the request. Live samples never exceed the capacity, so a block holds at least as many
        // appends as the samples copied into it.
        const std::size_t blockSize = std::max(std::min(std::max(m_Size * 2, MIN_BLOCK), m_Capacity * 2), m_Size + count);
        // A fixed-size vector, never resized: views hold spans into it.
        auto block = std::make_shared<std::vector<T>>(blockSize);
        const std::span<T> slots(*block);
        std::ranges::copy(live(), slots.begin());
        m_Block = std::move(block); // the old block lives on in any view that still holds it
        m_Slots = slots;
        m_Start = 0;
    }

    /// Append a value, discarding the oldest when full. Allocates only when the block is full (see
    /// reserve()); either appends or, on std::bad_alloc, changes nothing.
    void push(T value)
    {
        reserve(1);
        if (m_Size == m_Capacity)
        {
            discardFront(1);
        }
        m_Slots[m_Start + m_Size] = std::move(value);
        ++m_Size;
    }

    /// Discard the oldest @p count samples in O(1). Views already taken keep them.
    void discardFront(std::size_t count) noexcept
    {
        const std::size_t removeCount = std::min(count, m_Size);
        m_Start += removeCount;
        m_Size -= removeCount;
    }

    /// Discard every sample. The write position stays where it is: a slot is never written twice.
    void clear() noexcept
    {
        discardFront(m_Size);
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Size;
    }
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return m_Capacity;
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return m_Size == 0;
    }
    [[nodiscard]] bool full() const noexcept
    {
        return m_Size == m_Capacity;
    }

    /// Element by logical index (0 = oldest, size()-1 = newest).
    [[nodiscard]] T operator[](std::size_t index) const
    {
        return m_Slots[m_Start + index];
    }
    [[nodiscard]] const T& ref(std::size_t index) const noexcept
    {
        return m_Slots[m_Start + index];
    }

    /// The newest value, or T{} when empty.
    [[nodiscard]] T latest() const
    {
        return (m_Size == 0) ? T{} : m_Slots[m_Start + m_Size - 1];
    }

    /// Copy the oldest min(@p maxCount, size()) samples to @p buffer; returns how many.
    [[nodiscard]] std::size_t copyTo(T* buffer, std::size_t maxCount) const
    {
        const std::size_t count = std::min(maxCount, m_Size);
        if (count > 0)
        {
            std::ranges::copy(live().first(count), buffer);
        }
        return count;
    }

    /// The current samples, shared rather than copied: O(1). The view never changes afterwards.
    [[nodiscard]] HistoryView<T> view() const noexcept
    {
        if (m_Size == 0)
        {
            return {};
        }
        return HistoryView<T>(m_Block, live());
    }

  private:
    /// The smallest block allocated, so a short history doesn't compact every few samples.
    static constexpr std::size_t MIN_BLOCK = 64;

    /// The live samples, oldest first.
    [[nodiscard]] std::span<const T> live() const noexcept
    {
        return std::span<const T>(m_Slots).subspan(m_Start, m_Size);
    }

    std::shared_ptr<std::vector<T>> m_Block;
    std::span<T> m_Slots;    // all of m_Block
    std::size_t m_Start = 0; // first live slot
    std::size_t m_Size = 0;  // live samples; the next append goes to m_Start + m_Size
    std::size_t m_Capacity = 1;
};

} // namespace Domain
