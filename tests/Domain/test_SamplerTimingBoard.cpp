/// @file test_SamplerTimingBoard.cpp
/// @brief Domain::SamplerTimingBoard (#3153): the per-sampler pass timing the samplers publish and the
/// Render Metrics overlay reads -- one row per sampler, replaced in place, and the overlay's line.

#include "AllocationFailureHook.h"
#include "Domain/SamplerTimingBoard.h"
#include "Domain/TimingHistogram.h"

#include <gtest/gtest.h>

#include <chrono>
#include <initializer_list>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace
{

using Domain::SamplerTimingBoard;
using Domain::SamplerTimingRow;
using Domain::TimingHistogram;

[[nodiscard]] TimingHistogram histogramOf(std::initializer_list<std::chrono::microseconds> durations)
{
    TimingHistogram histogram;
    for (const auto duration : durations)
    {
        histogram.record(duration);
    }
    return histogram;
}

TEST(SamplerTimingBoardTest, EachSamplerHasOneRowInTheOrderItFirstPublished)
{
    SamplerTimingBoard board;
    EXPECT_TRUE(board.snapshot().empty());

    board.publish("ts-sampler-disk", histogramOf({2ms}), 0, 0);
    board.publish("ts-sampler-proc", histogramOf({5ms, 7ms}), 1, 1);
    // A later pass replaces the disk sampler's row rather than adding another.
    board.publish("ts-sampler-disk", histogramOf({2ms, 3ms, 4ms}), 2, 1);

    const std::vector<SamplerTimingRow> rows = board.snapshot();
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].sampler, "ts-sampler-disk");
    EXPECT_EQ(rows[0].pass.count(), 3U);
    EXPECT_EQ(rows[0].pass.max(), 4ms);
    EXPECT_EQ(rows[0].overruns, 2U);
    EXPECT_EQ(rows[0].backoffs, 1U);
    EXPECT_EQ(rows[1].sampler, "ts-sampler-proc");
    EXPECT_EQ(rows[1].pass.count(), 2U);
    EXPECT_EQ(rows[1].overruns, 1U);
}

TEST(SamplerTimingBoardTest, TheGlobalBoardIsOneInstance)
{
    EXPECT_EQ(&SamplerTimingBoard::global(), &SamplerTimingBoard::global());
}

TEST(SamplerTimingBoardTest, ARowThatCannotBeAddedIsDroppedWithoutThrowing)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    // publish() runs on a sampler thread after every pass: a std::bad_alloc adding a new row (its name,
    // longer than any small-string buffer, or the vector's growth) must not escape it.
    SamplerTimingBoard board;
    const std::string longName(48, 's');
    {
        const TestSupport::FailAllocationAfter failure(0);
        board.publish(longName, histogramOf({1ms}), 0, 0);
    }
    EXPECT_TRUE(board.snapshot().empty());

    board.publish(longName, histogramOf({1ms}), 0, 0); // a later pass adds it
    ASSERT_EQ(board.snapshot().size(), 1U);
    EXPECT_EQ(board.snapshot()[0].sampler, longName);
#endif
}

TEST(SamplerTimingBoardTest, TheRowLineNamesTheSamplerAndItsDistributionInMilliseconds)
{
    SamplerTimingRow row{.sampler = "ts-sampler-gpu", .overruns = 3, .backoffs = 2};
    for (int i = 0; i < 99; ++i)
    {
        row.pass.record(1600us);
    }
    row.pass.record(40ms);

    EXPECT_EQ(Domain::formatSamplerTimingRow(row),
              "ts-sampler-gpu: 100 passes, mean 2.0 / p50 1.6 / p95 1.6 / max 40.0 ms, 3 overruns, 2 backoffs");
}

} // namespace
