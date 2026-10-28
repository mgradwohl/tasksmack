/// @file test_TimingHistogram.cpp
/// @brief Domain::TimingHistogram (#3153): a sampler's duration distribution in fixed memory -- its
/// bucket edges, the exact count/mean/min/max, and percentiles within the buckets' x1.41.

#include "Domain/TimingHistogram.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <type_traits>

using namespace std::chrono_literals;

namespace
{

using Domain::TimingHistogram;

// Fixed memory: the histogram is a value with no heap behind it, the same size however many durations
// it has counted, so recording one per pass never allocates and a copy to publish it is a memcpy.
static_assert(std::is_trivially_copyable_v<TimingHistogram>);
static_assert(sizeof(TimingHistogram) == (TimingHistogram::BUCKET_COUNT * sizeof(std::uint64_t)) + (4 * sizeof(std::int64_t)));

TEST(TimingHistogramTest, BucketsAreLog2SpacedFromOneMicrosecond)
{
    EXPECT_EQ(TimingHistogram::bucketIndex(0us), 0U);
    EXPECT_EQ(TimingHistogram::bucketIndex(-5us), 0U);
    EXPECT_EQ(TimingHistogram::bucketIndex(1us), 1U);
    EXPECT_EQ(TimingHistogram::bucketIndex(2us), 2U);
    EXPECT_EQ(TimingHistogram::bucketIndex(3us), 2U);
    EXPECT_EQ(TimingHistogram::bucketIndex(4us), 3U);
    EXPECT_EQ(TimingHistogram::bucketIndex(1023us), 10U);
    EXPECT_EQ(TimingHistogram::bucketIndex(1024us), 11U);
    // The last bounded bucket ends at 2^24 us (about 16.8 s); everything longer shares the final one.
    EXPECT_EQ(TimingHistogram::bucketIndex(std::chrono::microseconds{(1 << 24) - 1}), TimingHistogram::BUCKET_COUNT - 2);
    EXPECT_EQ(TimingHistogram::bucketIndex(std::chrono::microseconds{1 << 24}), TimingHistogram::BUCKET_COUNT - 1);
    EXPECT_EQ(TimingHistogram::bucketIndex(std::chrono::hours{24}), TimingHistogram::BUCKET_COUNT - 1);

    EXPECT_EQ(TimingHistogram::bucketLowerBound(0), 0us);
    EXPECT_EQ(TimingHistogram::bucketLowerBound(1), 1us);
    EXPECT_EQ(TimingHistogram::bucketLowerBound(11), 1024us);
    for (std::size_t index = 1; index < TimingHistogram::BUCKET_COUNT; ++index)
    {
        EXPECT_EQ(TimingHistogram::bucketIndex(TimingHistogram::bucketLowerBound(index)), index);
    }
}

TEST(TimingHistogramTest, AnEmptyHistogramReportsZero)
{
    constexpr TimingHistogram empty;
    EXPECT_EQ(empty.count(), 0U);
    EXPECT_EQ(empty.min(), 0us);
    EXPECT_EQ(empty.max(), 0us);
    EXPECT_EQ(empty.mean(), 0us);
    EXPECT_EQ(empty.percentile(0.5), 0us);
    EXPECT_EQ(std::accumulate(empty.buckets().begin(), empty.buckets().end(), std::uint64_t{0}), 0U);
}

TEST(TimingHistogramTest, CountMeanMinAndMaxAreExact)
{
    TimingHistogram histogram;
    histogram.record(300us);
    histogram.record(100us);
    histogram.record(200us);
    histogram.record(-7us); // a clock that went backwards counts as 0

    EXPECT_EQ(histogram.count(), 4U);
    EXPECT_EQ(histogram.min(), 0us);
    EXPECT_EQ(histogram.max(), 300us);
    EXPECT_EQ(histogram.mean(), 150us);
    EXPECT_EQ(histogram.buckets()[0], 1U);
    EXPECT_EQ(histogram.buckets()[TimingHistogram::bucketIndex(100us)], 1U);
    EXPECT_EQ(histogram.buckets()[TimingHistogram::bucketIndex(200us)], 1U);
    EXPECT_EQ(histogram.buckets()[TimingHistogram::bucketIndex(300us)], 1U);
}

TEST(TimingHistogramTest, PercentilesOfIdenticalDurationsAreExact)
{
    // Clamped to [min, max]: every duration the same gives that duration, whatever the bucket's middle.
    TimingHistogram histogram;
    for (int i = 0; i < 50; ++i)
    {
        histogram.record(1500us);
    }
    EXPECT_EQ(histogram.percentile(0.0), 1500us);
    EXPECT_EQ(histogram.percentile(0.5), 1500us);
    EXPECT_EQ(histogram.percentile(0.95), 1500us);
    EXPECT_EQ(histogram.percentile(1.0), 1500us);
}

TEST(TimingHistogramTest, PercentilesFindTheRankAcrossTheBuckets)
{
    // A fake clock's durations: 90 quick passes of 100 us and 10 slow ones of 10 ms. p50 and p85 fall in
    // the quick bucket, p91 and up in the slow one; each answer is clamped to the extremes, so exact.
    TimingHistogram histogram;
    for (int i = 0; i < 90; ++i)
    {
        histogram.record(100us);
    }
    for (int i = 0; i < 10; ++i)
    {
        histogram.record(10ms);
    }
    EXPECT_EQ(histogram.percentile(0.50), 100us);
    EXPECT_EQ(histogram.percentile(0.85), 100us);
    EXPECT_EQ(histogram.percentile(0.91), 10ms);
    EXPECT_EQ(histogram.percentile(0.95), 10ms);
    EXPECT_EQ(histogram.percentile(-1.0), 100us); // clamped to the first rank
    EXPECT_EQ(histogram.percentile(2.0), 10ms);   // clamped to the last
    EXPECT_EQ(histogram.mean(), 1090us);
}

TEST(TimingHistogramTest, APercentileInsideABucketIsItsGeometricMiddle)
{
    // Three durations in [1024, 2048) us and one far beyond: p50 is that bucket's lower bound x1.41.
    TimingHistogram histogram;
    histogram.record(1100us);
    histogram.record(1500us);
    histogram.record(2000us);
    histogram.record(1s);
    EXPECT_EQ(histogram.percentile(0.5), 1024us + (1024us * 41 / 100));
}

TEST(TimingHistogramTest, PercentilesStayWithinTheBucketResolution)
{
    // One of every duration from 1 us to 20 ms: each estimate is within x1.42 of the exact percentile.
    constexpr std::int64_t COUNT = 20'000;
    TimingHistogram histogram;
    for (std::int64_t us = 1; us <= COUNT; ++us)
    {
        histogram.record(std::chrono::microseconds{us});
    }
    for (const double fraction : {0.01, 0.10, 0.25, 0.50, 0.75, 0.90, 0.95, 0.99})
    {
        const auto exact = static_cast<double>(fraction * COUNT);
        const auto estimate = static_cast<double>(histogram.percentile(fraction).count());
        EXPECT_LE(estimate, exact * 1.42) << "p" << fraction * 100;
        EXPECT_GE(estimate, exact / 1.42) << "p" << fraction * 100;
    }
    EXPECT_EQ(histogram.percentile(1.0), std::chrono::microseconds{COUNT});
}

TEST(TimingHistogramTest, DurationsBeyondTheLastBoundedBucketAnswerTheMax)
{
    // A stalled pass of a minute: it lands in the unbounded final bucket, and a percentile there is
    // the longest recorded rather than a guess.
    TimingHistogram histogram;
    histogram.record(1ms);
    histogram.record(std::chrono::minutes{1});
    histogram.record(std::chrono::minutes{2});
    EXPECT_EQ(histogram.buckets()[TimingHistogram::BUCKET_COUNT - 1], 2U);
    EXPECT_EQ(histogram.percentile(0.95), std::chrono::minutes{2});
    EXPECT_EQ(histogram.percentile(0.10), 1ms);
}

TEST(TimingHistogramTest, MemoryStaysBoundedOverAMillionDurations)
{
    // A long run only grows the counters: the same object, every duration (up to 97 s) counted in some bucket.
    TimingHistogram histogram;
    constexpr std::uint64_t COUNT = 1'000'000;
    for (std::uint64_t i = 0; i < COUNT; ++i)
    {
        histogram.record(std::chrono::microseconds{static_cast<std::int64_t>(i * 97)});
    }
    EXPECT_EQ(histogram.count(), COUNT);
    EXPECT_EQ(std::accumulate(histogram.buckets().begin(), histogram.buckets().end(), std::uint64_t{0}), COUNT);
}

} // namespace
