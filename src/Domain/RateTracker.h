#pragma once

// Rates from counters, in one place (#3344). A rate is a counter's change over the time between two of its
// readings, and every reading carries the time its counters were read (Sample::acquiredAt, taken on the thread
// that read them), never the time something took the result in. So a reading adopted late (a hidden window, a
// stalled frame, a late poll) divides by the same interval as one adopted at once, and UI timing never changes a
// measurement (#2731, #3337).
//
// RateTracker keeps each identity's last reading and hands back the interval since it: an identity is whatever
// tells two readings of the same thing apart from a reused number (PID plus start time, TID plus start time, a
// device ID). It gives no interval for a first reading, for time that didn't move forward (or moved less than the
// tracker's minimum), and it forgets identities that a round of readings didn't see. counterDelta() and
// counterRate() turn an interval's two counter values into a change or a rate, and read a counter that went
// backwards (a reset, or a wrap of a counter wider than its source) as no change known, rather than a huge one.
//
// Pure: no clock, no threads, no OS. Tested in tests/Domain/test_RateTracker.cpp with fixed timestamps.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>

namespace Domain
{

/// When a reading's counters were read: on a steady clock, so it never steps back.
using AcquiredAt = std::chrono::steady_clock::time_point;

/// @p seconds since the steady clock's epoch (Platform::threadsReadClockSeconds()'s clock) as an AcquiredAt.
[[nodiscard]] inline AcquiredAt acquiredAtFromSeconds(double seconds) noexcept
{
    return AcquiredAt{std::chrono::round<AcquiredAt::duration>(std::chrono::duration<double>(seconds))};
}

/// A reading and when it was read.
template<typename T> struct Sample
{
    T value{};
    AcquiredAt acquiredAt; ///< Its default constructor is the clock's epoch.
};

/// A process or thread as a rate is kept for: its number and when it started, so a reused number is a new one.
struct StartedIdentity
{
    std::int64_t id = 0;          ///< The PID or TID.
    std::uint64_t startTicks = 0; ///< When it started, in the OS's units (0 when unknown).

    friend bool operator==(const StartedIdentity&, const StartedIdentity&) = default;
};

struct StartedIdentityHash
{
    [[nodiscard]] std::size_t operator()(const StartedIdentity& identity) const noexcept
    {
        // A 64-bit mix of both (splitmix64's finalizer), so IDs that differ only in their start time spread too.
        std::uint64_t mixed = static_cast<std::uint64_t>(identity.id) ^ (identity.startTicks * 0x9E3779B97F4A7C15ULL);
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        return static_cast<std::size_t>(mixed ^ (mixed >> 31U));
    }
};

/// The change in a counter from @p previous to @p current; nullopt when it went backwards (a reset or a wrap).
[[nodiscard]] constexpr std::optional<std::uint64_t> counterDelta(std::uint64_t current, std::uint64_t previous) noexcept
{
    if (current < previous)
    {
        return std::nullopt;
    }
    return current - previous;
}

/// A counter's change per second from @p previous to @p current over @p seconds; nullopt when it went backwards
/// or no time passed.
[[nodiscard]] constexpr std::optional<double> counterRate(std::uint64_t current, std::uint64_t previous, double seconds) noexcept
{
    const std::optional<std::uint64_t> delta = counterDelta(current, previous);
    if (!delta.has_value() || !(seconds > 0.0))
    {
        return std::nullopt;
    }
    return static_cast<double>(*delta) / seconds;
}

/// Each identity's last reading, and the interval from it to the next.
/// @tparam Key    An identity that a reused number can't share (PID plus start time, not the PID alone).
/// @tparam Value  What one reading holds: a counter, or a struct of them.
template<typename Key, typename Value, typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>> class RateTracker
{
  public:
    /// From one reading of an identity to the next.
    struct Interval
    {
        Value previous{};     ///< The reading before.
        double seconds = 0.0; ///< The time between the two readings' acquisitions: more than the minimum.
    };

    /// @p minimumSeconds: an interval this short or shorter gives no rate (a seed reading taken just after the
    /// first one would divide a few counts by almost nothing). The reading still becomes the identity's last.
    explicit RateTracker(double minimumSeconds = 0.0) noexcept : m_MinimumSeconds(minimumSeconds > 0.0 ? minimumSeconds : 0.0)
    {}

    /// Records @p value, read at @p acquiredAt, as @p key's last reading, and marks @p key seen this round. Returns
    /// the interval from its reading before, or nullopt when there was none, or time didn't move forward by more
    /// than the minimum. Throws only what inserting a new identity or copying a Value throws.
    std::optional<Interval> observe(const Key& key, Value value, AcquiredAt acquiredAt)
    {
        auto [entry, inserted] = m_Entries.try_emplace(key);
        Entry& last = entry->second;
        last.round = m_Round;
        if (inserted)
        {
            last.reading = {.value = std::move(value), .acquiredAt = acquiredAt};
            return std::nullopt;
        }
        const double seconds = std::chrono::duration<double>(acquiredAt - last.reading.acquiredAt).count();
        Value previous = std::exchange(last.reading.value, std::move(value));
        last.reading.acquiredAt = acquiredAt;
        if (!(seconds > m_MinimumSeconds))
        {
            return std::nullopt;
        }
        return Interval{.previous = std::move(previous), .seconds = seconds};
    }

    /// Starts a round of readings: the identities observe() sees before the next endRound() are kept.
    void beginRound() noexcept
    {
        ++m_Round;
    }

    /// Ends the round: forgets every identity it didn't see (one that has gone, or whose number was reused).
    void endRound()
    {
        std::erase_if(m_Entries, [round = m_Round](const auto& entry) { return entry.second.round != round; });
    }

    /// Forgets every identity: the next reading of each is a first one.
    void clear() noexcept
    {
        m_Entries.clear();
    }

    /// How many identities it holds a reading for.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Entries.size();
    }

    /// @p key's last reading, if it has one.
    [[nodiscard]] const Sample<Value>* last(const Key& key) const
    {
        const auto found = m_Entries.find(key);
        return found != m_Entries.end() ? &found->second.reading : nullptr;
    }

  private:
    struct Entry
    {
        Sample<Value> reading;
        std::uint64_t round = 0; ///< The round it was last seen in.
    };

    std::unordered_map<Key, Entry, Hash, Equal> m_Entries;
    std::uint64_t m_Round = 0;
    double m_MinimumSeconds = 0.0;
};

} // namespace Domain
