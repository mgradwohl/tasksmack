#pragma once

// A fixed-size distribution of durations (#3153): count, mean, min, max and percentiles, in
// log2-spaced microsecond buckets. A sampler keeps one per samplable and one for its whole pass, so
// its p50/p95 can be read from a log line or the Render Metrics overlay rather than inferred from a
// last/max pair. Recording is a bucket increment: no allocation and no growth however long the
// sampler runs, and the type is trivially copyable, so a copy to publish it is a memcpy.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Domain
{

class TimingHistogram
{
  public:
    /// Bucket 0 holds durations under 1 us; bucket i (1..BUCKET_COUNT-2) holds [2^(i-1), 2^i) us, so
    /// the last bounded one ends at 2^24 us, about 16.8 s; the final bucket takes everything longer.
    static constexpr std::size_t BUCKET_COUNT = 26;
    using Buckets = std::array<std::uint64_t, BUCKET_COUNT>;

    /// The bucket a duration falls in. Negative durations count as 0.
    [[nodiscard]] static constexpr std::size_t bucketIndex(std::chrono::microseconds duration) noexcept
    {
        if (duration.count() <= 0)
        {
            return 0;
        }
        const auto width = static_cast<std::size_t>(std::bit_width(static_cast<std::uint64_t>(duration.count())));
        return width < BUCKET_COUNT ? width : BUCKET_COUNT - 1;
    }

    /// The shortest duration bucket @p index holds (0 for bucket 0).
    [[nodiscard]] static constexpr std::chrono::microseconds bucketLowerBound(std::size_t index) noexcept
    {
        return index == 0 ? std::chrono::microseconds{0} : std::chrono::microseconds{std::int64_t{1} << (index - 1)};
    }

    constexpr void record(std::chrono::microseconds duration) noexcept
    {
        const std::chrono::microseconds clamped = duration.count() < 0 ? std::chrono::microseconds{0} : duration;
        ++m_Buckets[bucketIndex(clamped)];
        m_Min = (m_Count == 0 || clamped < m_Min) ? clamped : m_Min;
        m_Max = (clamped > m_Max) ? clamped : m_Max;
        m_Total += clamped;
        ++m_Count;
    }

    [[nodiscard]] constexpr std::uint64_t count() const noexcept
    {
        return m_Count;
    }

    /// The shortest and longest durations recorded; 0 before the first.
    [[nodiscard]] constexpr std::chrono::microseconds min() const noexcept
    {
        return m_Min;
    }

    [[nodiscard]] constexpr std::chrono::microseconds max() const noexcept
    {
        return m_Max;
    }

    /// The exact mean of every duration recorded; 0 before the first.
    [[nodiscard]] constexpr std::chrono::microseconds mean() const noexcept
    {
        return m_Count == 0 ? std::chrono::microseconds{0} : m_Total / static_cast<std::int64_t>(m_Count);
    }

    /// The duration @p fraction (0..1, clamped) of the recorded ones are at or under, estimated from
    /// the buckets: the geometric middle of the bucket that holds that rank (its lower bound x ~1.41),
    /// so within about x1.41 of the true value, and clamped to [min(), max()] -- exact when every
    /// duration in that bucket was the same, or the rank falls on the shortest or longest. The final,
    /// unbounded bucket answers max(). 0 before the first duration.
    [[nodiscard]] constexpr std::chrono::microseconds percentile(double fraction) const noexcept
    {
        if (m_Count == 0)
        {
            return std::chrono::microseconds{0};
        }
        const double clampedFraction = std::clamp(fraction, 0.0, 1.0);
        // The 1-based rank of the duration asked for: ceil(fraction * count), at least the first.
        const double exactRank = clampedFraction * static_cast<double>(m_Count);
        auto rank = static_cast<std::uint64_t>(exactRank);
        rank += (static_cast<double>(rank) < exactRank) ? 1U : 0U;
        rank = rank == 0 ? 1 : rank;

        std::uint64_t seen = 0;
        std::size_t index = 0;
        for (; index < BUCKET_COUNT - 1; ++index)
        {
            seen += m_Buckets[index];
            if (seen >= rank)
            {
                break;
            }
        }
        if (index == BUCKET_COUNT - 1)
        {
            return m_Max;
        }
        const std::chrono::microseconds lower = bucketLowerBound(index);
        // lower * ~sqrt(2), in integers: 41/100 of the lower bound again.
        const std::chrono::microseconds middle = lower + ((lower * 41) / 100);
        // m_Min <= m_Max once a duration is recorded, which the early return above ensures.
        return std::clamp(middle, m_Min, m_Max);
    }

    [[nodiscard]] constexpr const Buckets& buckets() const noexcept
    {
        return m_Buckets;
    }

  private:
    Buckets m_Buckets{};
    std::uint64_t m_Count = 0;
    std::chrono::microseconds m_Total{0};
    std::chrono::microseconds m_Min{0};
    std::chrono::microseconds m_Max{0};
};

} // namespace Domain
