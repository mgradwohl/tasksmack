#include "Domain/History.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace Domain
{
namespace
{

// =============================================================================
// Shared History Utilities
// =============================================================================

TEST(HistoryUtilsTest, DiscardBeforeKeepsAlignedBuffersSynchronized)
{
    // One sample a second, t = 1..10; a 4.5 s window ending at 10 has its cutoff at 5.5.
    HistoryBuffer<double> timestamps(16);
    HistoryBuffer<int> first(16);
    HistoryBuffer<int> second(16);
    for (int t = 1; t <= 10; ++t)
    {
        timestamps.push(static_cast<double>(t));
        first.push(t * 10);
        second.push(t * 100);
    }

    // 1..5 are before the cutoff; 5, the newest of them, is kept as the anchor that lets a chart's
    // line run off the window's left edge (#1016).
    EXPECT_EQ(HistoryUtils::discardBefore(timestamps, 5.5, first, second), 4);
    ASSERT_EQ(timestamps.size(), 6ULL);
    EXPECT_DOUBLE_EQ(timestamps[0], 5.0);
    ASSERT_EQ(first.size(), 6ULL);
    EXPECT_EQ(first[0], 50);
    ASSERT_EQ(second.size(), 6ULL);
    EXPECT_EQ(second[0], 500);
}

TEST(HistoryUtilsTest, DiscardBeforeDropsTheAnchorAcrossAGap)
{
    // Sampling paused for longer than the window: [1, 20] with the cutoff at 15. Keeping 1 would draw
    // it connected to 20 across the whole pause (#1060 review), so it goes.
    HistoryBuffer<double> timestamps(4);
    HistoryBuffer<int> aligned(4);
    timestamps.push(1.0);
    timestamps.push(20.0);
    aligned.push(10);
    aligned.push(200);

    EXPECT_EQ(HistoryUtils::discardBefore(timestamps, 15.0, aligned), 1);
    ASSERT_EQ(timestamps.size(), 1ULL);
    EXPECT_DOUBLE_EQ(timestamps[0], 20.0);
    EXPECT_EQ(aligned[0], 200);
}

TEST(HistoryUtilsTest, KeepTrimAnchorOnlyForAnAdjacentSample)
{
    // Window = newest - cutoff = 10. A step of 1 s is an adjacent sample; 30 s is a gap.
    EXPECT_TRUE(HistoryUtils::keepTrimAnchor(4.5, 5.5, 5.0, 15.0));
    EXPECT_FALSE(HistoryUtils::keepTrimAnchor(-20.0, 10.0, 5.0, 15.0));
    // A zero-length window keeps no anchor: only the current sample remains.
    EXPECT_FALSE(HistoryUtils::keepTrimAnchor(9.0, 10.0, 10.0, 10.0));
}

TEST(HistoryUtilsTest, DiscardBeforeWithFutureCutoffEmptiesBuffers)
{
    HistoryBuffer<double> timestamps(4);
    HistoryBuffer<int> aligned(4);
    timestamps.push(1.0);
    timestamps.push(2.0);
    aligned.push(10);
    aligned.push(20);

    // Every entry is before the cutoff: no anchor is kept, since nothing newer remains to draw it
    // to -- it would be joined to the next sample across the gap.
    EXPECT_EQ(HistoryUtils::discardBefore(timestamps, 100.0, aligned), 2);
    EXPECT_TRUE(timestamps.empty());
    EXPECT_TRUE(aligned.empty());
}

TEST(HistoryUtilsTest, TrimCountBeforeKeepsTheAdjacentAnchor)
{
    // Samples 1 s apart, cutoff at 2.5: 0 and 1 go, 2 stays as the anchor just before the window.
    const std::vector<double> timestamps = {0.0, 1.0, 2.0, 3.0, 4.0};
    EXPECT_EQ(HistoryUtils::trimCountBefore(timestamps, 2.5), 2U);
    // Nothing before the cutoff: nothing goes.
    EXPECT_EQ(HistoryUtils::trimCountBefore(timestamps, 0.0), 0U);
    // The cutoff exactly on a sample: that sample is in the window, its predecessor is the anchor.
    EXPECT_EQ(HistoryUtils::trimCountBefore(timestamps, 3.0), 2U);
}

TEST(HistoryUtilsTest, TrimCountBeforeDropsTheAnchorAcrossAGap)
{
    // The same case as DiscardBeforeDropsTheAnchorAcrossAGap, for a contiguous history.
    const std::vector<double> timestamps = {1.0, 20.0};
    EXPECT_EQ(HistoryUtils::trimCountBefore(timestamps, 15.0), 1U);
}

TEST(HistoryUtilsTest, TrimCountBeforeRemovesEverythingWhenAllAreBeforeTheCutoff)
{
    const std::vector<double> timestamps = {1.0, 2.0};
    EXPECT_EQ(HistoryUtils::trimCountBefore(timestamps, 100.0), 2U);
    EXPECT_EQ(HistoryUtils::trimCountBefore(std::vector<double>{}, 100.0), 0U);
}

TEST(HistoryUtilsTest, ToVectorCopiesHistoryBufferChronologically)
{
    HistoryBuffer<int> history(3);
    history.push(1);
    history.push(2);
    history.push(3);
    history.push(4); // Evicts 1, wraps the ring

    EXPECT_EQ(HistoryUtils::toVector(history), std::vector<int>({2, 3, 4}));
}

// =============================================================================
// HistoryBuffer (runtime-capacity ring buffer)
// =============================================================================

TEST(HistoryBufferTest, DefaultConstructedIsEmptyWithZeroCapacity)
{
    const HistoryBuffer<int> history;
    EXPECT_TRUE(history.empty());
    EXPECT_FALSE(history.full());
    EXPECT_EQ(history.size(), 0ULL);
    EXPECT_EQ(history.capacity(), 0ULL);
}

TEST(HistoryBufferTest, PushEvictsOldestWhenFull)
{
    HistoryBuffer<int> history(3);
    history.push(1);
    history.push(2);
    history.push(3);
    EXPECT_TRUE(history.full());

    history.push(4); // Evicts 1

    EXPECT_EQ(history.size(), 3ULL);
    EXPECT_EQ(history[0], 2);
    EXPECT_EQ(history[1], 3);
    EXPECT_EQ(history[2], 4);
    EXPECT_EQ(history.latest(), 4);
}

TEST(HistoryBufferTest, MultipleWraparoundsKeepNewest)
{
    HistoryBuffer<int> history(5);
    for (int i = 0; i < 23; ++i)
    {
        history.push(i);
    }

    EXPECT_EQ(history.size(), 5ULL);
    for (std::size_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(history[i], 18 + static_cast<int>(i));
    }
}

TEST(HistoryBufferTest, DiscardFrontRemovesOldest)
{
    HistoryBuffer<int> history(5);
    for (int i = 0; i < 5; ++i)
    {
        history.push(i * 10);
    }

    history.discardFront(2);

    EXPECT_EQ(history.size(), 3ULL);
    EXPECT_EQ(history[0], 20);
    EXPECT_EQ(history[1], 30);
    EXPECT_EQ(history[2], 40);
}

TEST(HistoryBufferTest, DiscardFrontClampsToSize)
{
    HistoryBuffer<int> history(4);
    history.push(1);
    history.push(2);

    history.discardFront(10);

    EXPECT_TRUE(history.empty());

    // Buffer remains usable after over-discard
    history.push(7);
    EXPECT_EQ(history.size(), 1ULL);
    EXPECT_EQ(history.latest(), 7);
}

TEST(HistoryBufferTest, DiscardFrontZeroIsNoOp)
{
    HistoryBuffer<int> history(3);
    history.push(1);
    history.discardFront(0);
    EXPECT_EQ(history.size(), 1ULL);
    EXPECT_EQ(history[0], 1);
}

TEST(HistoryBufferTest, DiscardFrontOnWrappedBuffer)
{
    // Push more than capacity to force internal wraparound, then discard from front.
    HistoryBuffer<int> history(5);
    for (int i = 0; i < 8; ++i) // write index wraps; contains 3..7
    {
        history.push(i);
    }
    ASSERT_EQ(history.size(), 5ULL);

    history.discardFront(2); // drop 3, 4 → should contain 5, 6, 7

    EXPECT_EQ(history.size(), 3ULL);
    EXPECT_EQ(history[0], 5);
    EXPECT_EQ(history[1], 6);
    EXPECT_EQ(history[2], 7);
}

TEST(HistoryBufferTest, SetCapacityPreservesNewestElements)
{
    HistoryBuffer<int> history(6);
    for (int i = 0; i < 6; ++i)
    {
        history.push(i);
    }

    history.setCapacity(3); // Keeps newest 3: 3, 4, 5

    EXPECT_EQ(history.capacity(), 3ULL);
    EXPECT_EQ(history.size(), 3ULL);
    EXPECT_EQ(history[0], 3);
    EXPECT_EQ(history[1], 4);
    EXPECT_EQ(history[2], 5);
}

TEST(HistoryBufferTest, SetCapacityGrowKeepsAllElements)
{
    HistoryBuffer<int> history(2);
    history.push(1);
    history.push(2);
    history.push(3); // Evicts 1

    history.setCapacity(5);

    EXPECT_EQ(history.capacity(), 5ULL);
    EXPECT_EQ(history.size(), 2ULL);
    EXPECT_EQ(history[0], 2);
    EXPECT_EQ(history[1], 3);

    history.push(4);
    history.push(5);
    history.push(6);
    EXPECT_TRUE(history.full());
    EXPECT_EQ(history[0], 2);
    EXPECT_EQ(history.latest(), 6);
}

TEST(HistoryBufferTest, SetCapacityMinimumIsOne)
{
    HistoryBuffer<int> history;
    history.setCapacity(0);
    EXPECT_EQ(history.capacity(), 1ULL);

    history.push(42);
    history.push(43);
    EXPECT_EQ(history.size(), 1ULL);
    EXPECT_EQ(history.latest(), 43);
}

TEST(HistoryBufferTest, CopyToAfterWraparoundIsChronological)
{
    HistoryBuffer<int> history(5);
    for (int i = 0; i < 8; ++i) // Contains 3..7, wrapped
    {
        history.push(i);
    }

    std::array<int, 5> buffer{};
    const std::size_t copied = history.copyTo(buffer.data(), buffer.size());

    EXPECT_EQ(copied, 5ULL);
    for (std::size_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(buffer[i], 3 + static_cast<int>(i));
    }
}

TEST(HistoryBufferTest, ClearResetsAndAllowsReuse)
{
    HistoryBuffer<double> history(4);
    history.push(1.5);
    history.push(2.5);

    history.clear();
    EXPECT_TRUE(history.empty());
    EXPECT_EQ(history.capacity(), 4ULL);

    history.push(9.5);
    EXPECT_EQ(history.size(), 1ULL);
    EXPECT_DOUBLE_EQ(history.latest(), 9.5);
}

TEST(HistoryBufferTest, LatestReturnsDefaultWhenEmpty)
{
    const HistoryBuffer<int> history(3);
    EXPECT_EQ(history.latest(), 0);

    const HistoryBuffer<std::string> stringHistory(3);
    EXPECT_EQ(stringHistory.latest(), "");
}

TEST(HistoryBufferTest, WorksWithNonTrivialTypes)
{
    HistoryBuffer<std::string> history(3);
    history.push("first");
    history.push("second");
    history.push("third");
    history.push("fourth"); // Evicts "first"

    EXPECT_EQ(history.size(), 3ULL);
    EXPECT_EQ(history.ref(0), "second");
    EXPECT_EQ(history.ref(1), "third");
    EXPECT_EQ(history.ref(2), "fourth");
}

} // namespace
} // namespace Domain
