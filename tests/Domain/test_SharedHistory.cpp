// Tests for Domain::SharedHistoryBuffer / Domain::HistoryView: the append-only shared history behind
// model publications (#1412).

#include "Domain/History.h"
#include "Domain/SharedHistory.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Domain::HistoryView;
using Domain::SharedHistoryBuffer;

/// The view's samples, copied out for comparison.
template<typename T> std::vector<T> values(const HistoryView<T>& view)
{
    return {view.begin(), view.end()};
}

/// 0, 1, ..., count - 1 as doubles.
std::vector<double> iota(std::size_t from, std::size_t count)
{
    std::vector<double> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        out.push_back(static_cast<double>(from + i));
    }
    return out;
}

TEST(SharedHistoryTest, EmptyBufferHasAnEmptyView)
{
    const SharedHistoryBuffer<float> buffer(8);
    const auto view = buffer.view();
    EXPECT_TRUE(buffer.empty());
    EXPECT_TRUE(view.empty());
    EXPECT_EQ(view.size(), 0U);
    EXPECT_FLOAT_EQ(buffer.latest(), 0.0F);
    EXPECT_FALSE(view.sharesStorageWith(view)); // nothing to share
}

TEST(SharedHistoryTest, AppendKeepsOrderAndReadsLikeHistoryBuffer)
{
    SharedHistoryBuffer<double> shared(1000);
    Domain::HistoryBuffer<double> ring(1000);
    for (std::size_t i = 0; i < 300; ++i)
    {
        shared.push(static_cast<double>(i));
        ring.push(static_cast<double>(i));
    }
    ASSERT_EQ(shared.size(), ring.size());
    EXPECT_DOUBLE_EQ(shared.latest(), ring.latest());
    for (std::size_t i = 0; i < shared.size(); ++i)
    {
        EXPECT_DOUBLE_EQ(shared[i], ring[i]);
        EXPECT_DOUBLE_EQ(shared.ref(i), ring.ref(i));
    }
    EXPECT_EQ(values(shared.view()), Domain::HistoryUtils::toVector(ring));
    EXPECT_EQ(Domain::HistoryUtils::toVector(shared), Domain::HistoryUtils::toVector(ring));
}

TEST(SharedHistoryTest, FullBufferDropsTheOldestOnAppend)
{
    SharedHistoryBuffer<double> buffer(5);
    for (std::size_t i = 0; i < 12; ++i)
    {
        buffer.push(static_cast<double>(i));
    }
    EXPECT_TRUE(buffer.full());
    EXPECT_EQ(values(buffer.view()), iota(7, 5));
}

TEST(SharedHistoryTest, ViewIsAContiguousSpan)
{
    SharedHistoryBuffer<float> buffer(100);
    for (std::size_t i = 0; i < 10; ++i)
    {
        buffer.push(static_cast<float>(i));
    }
    const auto view = buffer.view();
    const std::span<const float> span = view; // implicit, as chart code takes it
    ASSERT_EQ(span.size(), 10U);
    EXPECT_EQ(span.data(), view.data());
    EXPECT_FLOAT_EQ(view.front(), 0.0F);
    EXPECT_FLOAT_EQ(view.back(), 9.0F);
    EXPECT_FLOAT_EQ(*view.rbegin(), 9.0F);
    EXPECT_FLOAT_EQ(view[3], 3.0F);
}

TEST(SharedHistoryTest, ConsecutiveViewsShareStorage)
{
    SharedHistoryBuffer<double> buffer(1000);
    buffer.push(0.0);
    const auto first = buffer.view();
    buffer.push(1.0);
    const auto second = buffer.view();
    EXPECT_TRUE(second.sharesStorageWith(first));
    EXPECT_EQ(second.data(), first.data()); // the same samples, not a copy
}

// An older view stays valid and unchanged however the buffer moves on: appends, trims, compactions
// into new blocks, a capacity change and a clear.
TEST(SharedHistoryTest, OlderViewIsUnchangedByLaterAppendsAndTrims)
{
    SharedHistoryBuffer<double> buffer(200);
    for (std::size_t i = 0; i < 50; ++i)
    {
        buffer.push(static_cast<double>(i));
    }
    const auto old = buffer.view();
    const std::vector<double> expected = iota(0, 50);

    std::size_t compactions = 0;
    const double* block = buffer.view().data();
    for (std::size_t i = 50; i < 2000; ++i)
    {
        buffer.push(static_cast<double>(i));
        if (i % 3 == 0)
        {
            buffer.discardFront(1);
        }
        const auto now = buffer.view();
        if (!now.sharesStorageWith(old) && now.data() != block)
        {
            ++compactions;
            block = now.data();
        }
        ASSERT_EQ(values(old), expected) << "after append " << i;
    }
    buffer.setCapacity(10);
    buffer.clear();
    buffer.push(-1.0);
    EXPECT_GT(compactions, 0U); // the loop did cross block boundaries
    EXPECT_EQ(values(old), expected);
    EXPECT_EQ(values(buffer.view()), std::vector<double>{-1.0});
}

// Trimming across a compaction boundary: the samples kept are the same as a ring buffer's, whichever
// side of the boundary the trim falls.
TEST(SharedHistoryTest, TrimAcrossBlockBoundariesMatchesTheRingBuffer)
{
    for (const std::size_t trimEvery : {1U, 2U, 7U, 63U, 64U, 65U, 200U})
    {
        SCOPED_TRACE("trim every " + std::to_string(trimEvery));
        SharedHistoryBuffer<double> shared(300);
        Domain::HistoryBuffer<double> ring(300);
        for (std::size_t i = 0; i < 3000; ++i)
        {
            shared.push(static_cast<double>(i));
            ring.push(static_cast<double>(i));
            if (i % trimEvery == 0)
            {
                shared.discardFront(2);
                ring.discardFront(2);
            }
            ASSERT_EQ(shared.size(), ring.size());
        }
        EXPECT_EQ(values(shared.view()), Domain::HistoryUtils::toVector(ring));
    }
}

// discardBefore() trims aligned shared series together, with the #1016 anchor rule, exactly as it
// trims ring buffers.
TEST(SharedHistoryTest, DiscardBeforeKeepsSharedSeriesAligned)
{
    SharedHistoryBuffer<double> timestamps(1000);
    SharedHistoryBuffer<float> first(1000);
    SharedHistoryBuffer<float> second(1000);
    Domain::HistoryBuffer<double> ringTimestamps(1000);
    for (std::size_t i = 0; i < 500; ++i)
    {
        const auto t = static_cast<double>(i);
        timestamps.push(t);
        first.push(static_cast<float>(i));
        second.push(static_cast<float>(i) * 2.0F);
        ringTimestamps.push(t);
        if (i % 10 == 9)
        {
            const double cutoff = t - 95.5;
            const std::size_t removed = Domain::HistoryUtils::discardBefore(timestamps, cutoff, first, second);
            EXPECT_EQ(removed, Domain::HistoryUtils::discardBefore(ringTimestamps, cutoff));
        }
        ASSERT_EQ(first.size(), timestamps.size());
        ASSERT_EQ(second.size(), timestamps.size());
    }
    EXPECT_EQ(values(timestamps.view()), Domain::HistoryUtils::toVector(ringTimestamps));
    // The anchor: the newest sample before the cutoff is kept (#1016).
    EXPECT_LT(timestamps.view().front(), 499.0 - 95.5);
    for (std::size_t i = 0; i < timestamps.size(); ++i)
    {
        EXPECT_FLOAT_EQ(first[i], static_cast<float>(timestamps[i]));
        EXPECT_FLOAT_EQ(second[i], static_cast<float>(timestamps[i]) * 2.0F);
    }
}

TEST(SharedHistoryTest, ShrinkingTheCapacityKeepsTheNewest)
{
    SharedHistoryBuffer<double> buffer(100);
    for (std::size_t i = 0; i < 100; ++i)
    {
        buffer.push(static_cast<double>(i));
    }
    const auto before = buffer.view();
    buffer.setCapacity(10);
    EXPECT_EQ(values(buffer.view()), iota(90, 10));
    EXPECT_EQ(values(before), iota(0, 100));
    EXPECT_TRUE(buffer.view().sharesStorageWith(before)); // trimming copies nothing
}

// After reserve(n), n appends never allocate: aligned series reserve together, then append, so a
// failed allocation can't leave them different lengths.
TEST(SharedHistoryTest, ReserveMakesTheNextAppendsAllocationFree)
{
    SharedHistoryBuffer<double> buffer(1000);
    for (const std::size_t count : {1U, 5U, 64U, 300U})
    {
        SCOPED_TRACE("reserve " + std::to_string(count));
        buffer.reserve(count);
        const auto block = buffer.view();
        for (std::size_t i = 0; i < count; ++i)
        {
            buffer.push(static_cast<double>(i));
            if (!block.empty())
            {
                ASSERT_TRUE(buffer.view().sharesStorageWith(block));
            }
        }
    }
}

// Compaction stays amortised O(1): over many appends at a steady length, blocks are replaced rarely.
TEST(SharedHistoryTest, CompactionIsAmortised)
{
    constexpr std::size_t LIVE = 1000;
    constexpr std::size_t APPENDS = 100'000;
    SharedHistoryBuffer<double> buffer(LIVE);
    std::size_t blocks = 0;
    const double* last = nullptr;
    for (std::size_t i = 0; i < APPENDS; ++i)
    {
        buffer.push(static_cast<double>(i));
        const double* first = buffer.view().data();
        if (first != last && (last == nullptr || first != last + 1))
        {
            ++blocks;
        }
        last = first;
    }
    // A new block at most once per LIVE appends at a steady length, plus the growth to it.
    EXPECT_LE(blocks, (APPENDS / LIVE) + 16);
}

// A reader on another thread walks the newest published view while the writer appends, trims and
// compacts. Run under TSan: the writer never writes a slot a view covers, so there is no race.
TEST(SharedHistoryTest, ViewsAreSafeToReadWhileTheWriterAppends)
{
    std::mutex slotMutex;
    HistoryView<double> published;
    std::atomic<bool> done{false};

    std::thread reader(
        [&]
        {
            while (!done.load(std::memory_order_acquire))
            {
                HistoryView<double> view;
                {
                    const std::scoped_lock lock(slotMutex);
                    view = published;
                }
                for (std::size_t i = 1; i < view.size(); ++i)
                {
                    ASSERT_DOUBLE_EQ(view[i], view[i - 1] + 1.0); // consecutive, never overwritten
                }
            }
        });

    SharedHistoryBuffer<double> buffer(500);
    for (std::size_t i = 0; i < 20'000; ++i)
    {
        buffer.push(static_cast<double>(i));
        if (i % 4 == 0)
        {
            buffer.discardFront(1);
        }
        const std::scoped_lock lock(slotMutex);
        published = buffer.view();
    }
    done.store(true, std::memory_order_release);
    reader.join();
}

} // namespace
