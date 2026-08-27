#include "Domain/History.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace Domain
{
namespace
{

// Use a small capacity for easier testing
using TestHistory = History<int, 5>;
using FloatHistory = History<float, 10>;

// =============================================================================
// Shared History Utilities
// =============================================================================

TEST(HistoryUtilsTest, DiscardBeforeKeepsAlignedBuffersSynchronized)
{
    HistoryBuffer<double> timestamps(8);
    HistoryBuffer<int> first(8);
    HistoryBuffer<int> second(8);
    timestamps.push(1.0);
    timestamps.push(2.0);
    timestamps.push(3.0);
    first.push(10);
    first.push(20);
    first.push(30);
    second.push(100);
    second.push(200);
    second.push(300);

    EXPECT_EQ(HistoryUtils::discardBefore(timestamps, 2.5, first, second), 2);
    ASSERT_EQ(timestamps.size(), 1ULL);
    EXPECT_DOUBLE_EQ(timestamps[0], 3.0);
    ASSERT_EQ(first.size(), 1ULL);
    EXPECT_EQ(first[0], 30);
    ASSERT_EQ(second.size(), 1ULL);
    EXPECT_EQ(second[0], 300);
}

TEST(HistoryUtilsTest, DiscardBeforeWithFutureCutoffEmptiesBuffers)
{
    HistoryBuffer<double> timestamps(4);
    HistoryBuffer<int> aligned(4);
    timestamps.push(1.0);
    timestamps.push(2.0);
    aligned.push(10);
    aligned.push(20);

    EXPECT_EQ(HistoryUtils::discardBefore(timestamps, 100.0, aligned), 2);
    EXPECT_TRUE(timestamps.empty());
    EXPECT_TRUE(aligned.empty());
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
// Construction and Initial State
// =============================================================================

TEST(HistoryTest, DefaultConstructedIsEmpty)
{
    TestHistory history;
    EXPECT_TRUE(history.empty());
    EXPECT_FALSE(history.full());
    EXPECT_EQ(history.size(), 0);
}

TEST(HistoryTest, CapacityIsConstexpr)
{
    static_assert(TestHistory::capacity() == 5, "Capacity should be 5");
    EXPECT_EQ(TestHistory::capacity(), 5);
}

// =============================================================================
// Push Operations
// =============================================================================

TEST(HistoryTest, PushIncreasesSize)
{
    TestHistory history;

    history.push(10);
    EXPECT_EQ(history.size(), 1);
    EXPECT_FALSE(history.empty());

    history.push(20);
    EXPECT_EQ(history.size(), 2);

    history.push(30);
    EXPECT_EQ(history.size(), 3);
}

TEST(HistoryTest, PushUntilFull)
{
    TestHistory history;

    for (int i = 0; i < 5; ++i)
    {
        history.push(i * 10);
    }

    EXPECT_EQ(history.size(), 5);
    EXPECT_TRUE(history.full());
}

TEST(HistoryTest, PushOverwritesOldestWhenFull)
{
    TestHistory history;

    // Fill with 0, 10, 20, 30, 40
    for (int i = 0; i < 5; ++i)
    {
        history.push(i * 10);
    }

    // Push 50, should overwrite 0
    history.push(50);

    EXPECT_EQ(history.size(), 5); // Still full
    EXPECT_TRUE(history.full());

    // Oldest should now be 10, newest should be 50
    EXPECT_EQ(history[0], 10); // Oldest
    EXPECT_EQ(history[4], 50); // Newest
}

TEST(HistoryTest, PushMultipleWraparounds)
{
    TestHistory history;

    // Push 15 values into capacity-5 buffer (3 full wraparounds)
    for (int i = 0; i < 15; ++i)
    {
        history.push(i);
    }

    EXPECT_EQ(history.size(), 5);

    // Should contain 10, 11, 12, 13, 14
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(history[static_cast<size_t>(i)], 10 + i);
    }
}

// =============================================================================
// Element Access
// =============================================================================

TEST(HistoryTest, IndexAccessReturnsCorrectOrder)
{
    TestHistory history;

    history.push(1);
    history.push(2);
    history.push(3);

    // Index 0 = oldest, index size-1 = newest
    EXPECT_EQ(history[0], 1);
    EXPECT_EQ(history[1], 2);
    EXPECT_EQ(history[2], 3);
}

TEST(HistoryTest, LatestReturnsNewestValue)
{
    TestHistory history;

    history.push(100);
    EXPECT_EQ(history.latest(), 100);

    history.push(200);
    EXPECT_EQ(history.latest(), 200);

    history.push(300);
    EXPECT_EQ(history.latest(), 300);
}

TEST(HistoryTest, LatestReturnsDefaultWhenEmpty)
{
    TestHistory history;
    EXPECT_EQ(history.latest(), 0); // Default for int

    History<std::string, 3> stringHistory;
    EXPECT_EQ(stringHistory.latest(), ""); // Default for string
}

TEST(HistoryTest, IndexAccessAfterWraparound)
{
    TestHistory history;

    // Fill completely: 0, 1, 2, 3, 4
    for (int i = 0; i < 5; ++i)
    {
        history.push(i);
    }

    // Add two more: 5, 6 (overwrites 0, 1)
    history.push(5);
    history.push(6);

    // Should now contain: 2, 3, 4, 5, 6
    EXPECT_EQ(history[0], 2);
    EXPECT_EQ(history[1], 3);
    EXPECT_EQ(history[2], 4);
    EXPECT_EQ(history[3], 5);
    EXPECT_EQ(history[4], 6);
    EXPECT_EQ(history.latest(), 6);
}

// =============================================================================
// Clear Operation
// =============================================================================

TEST(HistoryTest, ClearResetsToEmpty)
{
    TestHistory history;

    history.push(1);
    history.push(2);
    history.push(3);

    history.clear();

    EXPECT_TRUE(history.empty());
    EXPECT_EQ(history.size(), 0);
    EXPECT_FALSE(history.full());
}

TEST(HistoryTest, ClearAllowsReuse)
{
    TestHistory history;

    // Fill and clear
    for (int i = 0; i < 5; ++i)
    {
        history.push(i);
    }
    history.clear();

    // Reuse
    history.push(100);
    history.push(200);

    EXPECT_EQ(history.size(), 2);
    EXPECT_EQ(history[0], 100);
    EXPECT_EQ(history[1], 200);
}

// =============================================================================
// CopyTo Operation
// =============================================================================

TEST(HistoryTest, CopyToEmptyHistoryReturnsZero)
{
    TestHistory history;
    std::array<int, 5> buffer{};

    size_t copied = history.copyTo(buffer.data(), buffer.size());

    EXPECT_EQ(copied, 0);
}

TEST(HistoryTest, CopyToPartialHistory)
{
    TestHistory history;
    history.push(10);
    history.push(20);
    history.push(30);

    std::array<int, 5> buffer{};
    size_t copied = history.copyTo(buffer.data(), buffer.size());

    EXPECT_EQ(copied, 3);
    EXPECT_EQ(buffer[0], 10);
    EXPECT_EQ(buffer[1], 20);
    EXPECT_EQ(buffer[2], 30);
}

TEST(HistoryTest, CopyToFullHistory)
{
    TestHistory history;
    for (int i = 0; i < 5; ++i)
    {
        history.push(i * 10);
    }

    std::array<int, 5> buffer{};
    size_t copied = history.copyTo(buffer.data(), buffer.size());

    EXPECT_EQ(copied, 5);
    for (size_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(buffer[i], static_cast<int>(i) * 10);
    }
}

TEST(HistoryTest, CopyToAfterWraparound)
{
    TestHistory history;

    // Fill: 0, 1, 2, 3, 4
    for (int i = 0; i < 5; ++i)
    {
        history.push(i);
    }

    // Overwrite: now contains 2, 3, 4, 5, 6
    history.push(5);
    history.push(6);

    std::array<int, 5> buffer{};
    size_t copied = history.copyTo(buffer.data(), buffer.size());

    EXPECT_EQ(copied, 5);
    EXPECT_EQ(buffer[0], 2);
    EXPECT_EQ(buffer[1], 3);
    EXPECT_EQ(buffer[2], 4);
    EXPECT_EQ(buffer[3], 5);
    EXPECT_EQ(buffer[4], 6);
}

TEST(HistoryTest, CopyToSmallerBuffer)
{
    TestHistory history;
    for (int i = 0; i < 5; ++i)
    {
        history.push(i);
    }

    std::array<int, 3> smallBuffer{};
    size_t copied = history.copyTo(smallBuffer.data(), smallBuffer.size());

    EXPECT_EQ(copied, 3);
    EXPECT_EQ(smallBuffer[0], 0); // Oldest 3 elements
    EXPECT_EQ(smallBuffer[1], 1);
    EXPECT_EQ(smallBuffer[2], 2);
}

// =============================================================================
// Different Types
// =============================================================================

TEST(HistoryTest, WorksWithFloats)
{
    FloatHistory history;

    history.push(1.5F);
    history.push(2.5F);
    history.push(3.5F);

    EXPECT_FLOAT_EQ(history[0], 1.5F);
    EXPECT_FLOAT_EQ(history[1], 2.5F);
    EXPECT_FLOAT_EQ(history[2], 3.5F);
    EXPECT_FLOAT_EQ(history.latest(), 3.5F);
}

TEST(HistoryTest, WorksWithStrings)
{
    History<std::string, 3> history;

    history.push("first");
    history.push("second");
    history.push("third");
    history.push("fourth"); // Overwrites "first"

    EXPECT_EQ(history.size(), 3);
    EXPECT_EQ(history[0], "second");
    EXPECT_EQ(history[1], "third");
    EXPECT_EQ(history[2], "fourth");
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(HistoryTest, SingleElementCapacity)
{
    History<int, 1> history;

    EXPECT_EQ(history.capacity(), 1);

    history.push(100);
    EXPECT_EQ(history.size(), 1);
    EXPECT_TRUE(history.full());
    EXPECT_EQ(history.latest(), 100);

    history.push(200);
    EXPECT_EQ(history.size(), 1);
    EXPECT_EQ(history.latest(), 200);
    EXPECT_EQ(history[0], 200);
}

TEST(HistoryTest, DataPointerIsValid)
{
    TestHistory history;
    history.push(1);
    history.push(2);

    const int* ptr = history.data();
    EXPECT_NE(ptr, nullptr);
    // Note: raw data is not in logical order, just checking it's accessible
}

// =============================================================================
// Stress Test
// =============================================================================

TEST(HistoryTest, LargeNumberOfPushes)
{
    History<int, 100> history;

    // Push 10,000 values
    for (int i = 0; i < 10000; ++i)
    {
        history.push(i);
    }

    EXPECT_EQ(history.size(), 100);
    EXPECT_TRUE(history.full());

    // Should contain 9900..9999
    EXPECT_EQ(history[0], 9900);
    EXPECT_EQ(history[99], 9999);
    EXPECT_EQ(history.latest(), 9999);
}

// =============================================================================
// HistoryBuffer (runtime-capacity ring buffer)
// =============================================================================

TEST(HistoryBufferTest, DefaultConstructedIsEmptyWithZeroCapacity)
{
    HistoryBuffer<int> history;
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
    HistoryBuffer<int> history(3);
    EXPECT_EQ(history.latest(), 0);

    HistoryBuffer<std::string> stringHistory(3);
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
