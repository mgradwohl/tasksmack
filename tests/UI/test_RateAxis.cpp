#include "UI/RateAxis.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <span>

namespace UI::Widgets
{
namespace
{

constexpr double BYTES = RATE_AXIS_MIN_SPAN_BYTES_PER_SEC;

TEST(RateAxisTest, AllZeroSeriesGetsTheMinimumSpanNotASliverAroundZero)
{
    // The reported case (#920): an idle disk produced a +/-0.5 B/s axis, which rendered a negative
    // rate and eight identical "0.0B/s" tick labels.
    EXPECT_DOUBLE_EQ(rateAxisUpperBound(0.0, BYTES), BYTES);
}

TEST(RateAxisTest, SmallNonZeroMaxStillGetsTheMinimumSpan)
{
    EXPECT_DOUBLE_EQ(rateAxisUpperBound(12.0, BYTES), BYTES);
}

TEST(RateAxisTest, LargeMaxGetsHeadroomAboveIt)
{
    EXPECT_DOUBLE_EQ(rateAxisUpperBound(10'000.0, BYTES), 10'000.0 * RATE_AXIS_HEADROOM);
}

TEST(RateAxisTest, UpperBoundIsNeverBelowTheMinimumSpan)
{
    for (const double m : {-5.0, 0.0, 1.0, 100.0, BYTES / RATE_AXIS_HEADROOM})
    {
        EXPECT_GE(rateAxisUpperBound(m, BYTES), BYTES);
    }
}

TEST(RateAxisTest, NegativeAndNonFiniteMaxAreTreatedAsZero)
{
    // A negative rate is meaningless for these series; NaN would poison the axis entirely.
    EXPECT_DOUBLE_EQ(rateAxisUpperBound(-1'000.0, BYTES), BYTES);
    EXPECT_DOUBLE_EQ(rateAxisUpperBound(std::numeric_limits<double>::quiet_NaN(), BYTES), BYTES);
    EXPECT_DOUBLE_EQ(rateAxisUpperBound(std::numeric_limits<double>::infinity(), BYTES), BYTES);
}

// #1195: a process's CPU is a percent of the whole machine, so on a fixed 0-100 axis a typical
// process drew a flat line at zero. The percent axis scales to the data, between a floor and 100.
TEST(RateAxisTest, PercentAxisScalesDownToItsMinimumSpanForSmallValues)
{
    EXPECT_DOUBLE_EQ(percentAxisUpperBound(0.0), PERCENT_AXIS_MIN_SPAN);
    EXPECT_DOUBLE_EQ(percentAxisUpperBound(0.5), PERCENT_AXIS_MIN_SPAN);
}

TEST(RateAxisTest, PercentAxisGetsHeadroomAboveMidRangeValues)
{
    EXPECT_DOUBLE_EQ(percentAxisUpperBound(20.0), 20.0 * RATE_AXIS_HEADROOM);
}

TEST(RateAxisTest, PercentAxisNeverExceedsOneHundred)
{
    EXPECT_DOUBLE_EQ(percentAxisUpperBound(95.0), 100.0);
    EXPECT_DOUBLE_EQ(percentAxisUpperBound(100.0), 100.0);
    EXPECT_DOUBLE_EQ(percentAxisUpperBound(std::numeric_limits<double>::infinity()), PERCENT_AXIS_MIN_SPAN);
}

TEST(RateAxisTest, NonFiniteMinSpanFallsBackToOne)
{
    EXPECT_DOUBLE_EQ(rateAxisUpperBound(0.0, std::numeric_limits<double>::quiet_NaN()), 1.0);
    EXPECT_DOUBLE_EQ(rateAxisUpperBound(0.0, 0.0), 1.0);
}

TEST(RateAxisTest, MaxOfEmptySeriesIsZero)
{
    EXPECT_DOUBLE_EQ(maxOfSeries(std::span<const float>{}), 0.0);
}

TEST(RateAxisTest, MaxOfSeriesFindsTheLargestValue)
{
    const std::array<float, 4> s{1.0F, 42.5F, 3.0F, 7.0F};
    EXPECT_DOUBLE_EQ(maxOfSeries(s), 42.5);
}

TEST(RateAxisTest, MaxOfSeriesIgnoresNegativesAndNonFinite)
{
    const std::array<float, 4> s{-5.0F, std::numeric_limits<float>::quiet_NaN(), 2.0F, std::numeric_limits<float>::infinity()};
    EXPECT_DOUBLE_EQ(maxOfSeries(s), 2.0);
}

TEST(RateAxisTest, MaxOfTwoAndThreeSeriesSpansAllOfThem)
{
    const std::array<float, 2> a{1.0F, 2.0F};
    const std::array<float, 2> b{9.0F, 3.0F};
    const std::array<float, 2> c{4.0F, 20.0F};

    EXPECT_DOUBLE_EQ(maxOfSeries(a, b), 9.0);
    EXPECT_DOUBLE_EQ(maxOfSeries(a, b, c), 20.0);
}

// ========== easeAxisUpperBound (#1011) ==========

TEST(RateAxisTest, EasingMovesPartWayTowardTheTargetEachFrame)
{
    constexpr double frame = 1.0 / 60.0;
    const double up = easeAxisUpperBound(100.0, 200.0, frame);
    EXPECT_GT(up, 100.0);
    EXPECT_LT(up, 200.0);
    const double down = easeAxisUpperBound(200.0, 100.0, frame);
    EXPECT_LT(down, 200.0);
    EXPECT_GT(down, 100.0);
}

TEST(RateAxisTest, EasingRisesFasterThanItFalls)
{
    // A new peak should be clipped for as few frames as possible; a departing one can settle.
    constexpr double frame = 1.0 / 60.0;
    const double risen = easeAxisUpperBound(100.0, 200.0, frame) - 100.0;
    const double fallen = 200.0 - easeAxisUpperBound(200.0, 100.0, frame);
    EXPECT_GT(risen, fallen);
}

TEST(RateAxisTest, EasingSettlesExactlyOnTheTarget)
{
    double bound = 100.0;
    for (int i = 0; i < 600; ++i) // 10 s at 60 FPS
    {
        bound = easeAxisUpperBound(bound, 250.0, 1.0 / 60.0);
    }
    EXPECT_DOUBLE_EQ(bound, 250.0);
}

TEST(RateAxisTest, EasingWithNoPreviousBoundOrTimeIsTheTarget)
{
    EXPECT_DOUBLE_EQ(easeAxisUpperBound(0.0, 42.0, 0.016), 42.0);
    EXPECT_DOUBLE_EQ(easeAxisUpperBound(std::numeric_limits<double>::quiet_NaN(), 42.0, 0.016), 42.0);
    EXPECT_DOUBLE_EQ(easeAxisUpperBound(10.0, 42.0, 0.0), 42.0);
    EXPECT_DOUBLE_EQ(easeAxisUpperBound(10.0, 42.0, std::numeric_limits<double>::infinity()), 42.0);
}

// ========== stepEasedBound (#1003, #1011) ==========

TEST(RateAxisTest, EasedBoundStartsAtTheTargetTheFirstTime)
{
    EasedBound bound;
    EXPECT_DOUBLE_EQ(stepEasedBound(bound, 500.0, 10, 0.016), 500.0);
}

TEST(RateAxisTest, EasedBoundGivesTheSameAnswerTwiceInOneFrame)
{
    // A chart's axis and its NowBars both read the bound; a second read must not take a second step.
    EasedBound bound;
    (void) stepEasedBound(bound, 100.0, 1, 0.016);
    const double axis = stepEasedBound(bound, 200.0, 2, 0.016);
    const double bars = stepEasedBound(bound, 200.0, 2, 0.016);
    EXPECT_DOUBLE_EQ(axis, bars);
    EXPECT_GT(axis, 100.0);
    EXPECT_LT(axis, 200.0);
}

TEST(RateAxisTest, EasedBoundRestartsAtTheTargetAfterAGap)
{
    // Not drawn for a while (its tab was hidden): start at the target, not from the stale value.
    EasedBound bound;
    (void) stepEasedBound(bound, 100.0, 1, 0.016);
    EXPECT_DOUBLE_EQ(stepEasedBound(bound, 900.0, 50, 0.016), 900.0);
}

} // namespace
} // namespace UI::Widgets
