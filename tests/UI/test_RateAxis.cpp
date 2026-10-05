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

// ========== maxOfSeriesSince (#1145) ==========

TEST(RateAxisTest, MaxOfSeriesSinceLeavesOutTheAnchorBeforeTheWindow)
{
    // The trim keeps the sample just before the window's left edge (x = -61 for a 60 s window); its
    // peak is off-screen and must not scale the axis.
    const std::array<double, 4> x{-61.0, -40.0, -20.0, 0.0};
    const std::array<float, 4> s{500.0F, 1.0F, 3.0F, 2.0F};
    EXPECT_DOUBLE_EQ(maxOfSeriesSince(x, -60.0, s), 3.0);
    EXPECT_DOUBLE_EQ(maxOfSeries(s), 500.0); // What the axis used to be sized to
}

TEST(RateAxisTest, MaxOfSeriesSinceKeepsASampleExactlyOnTheLeftEdge)
{
    const std::array<double, 3> x{-60.0, -30.0, 0.0};
    const std::array<double, 3> s{7.0, 1.0, 2.0};
    EXPECT_DOUBLE_EQ(maxOfSeriesSince(x, -60.0, s), 7.0);
}

TEST(RateAxisTest, MaxOfSeriesSinceAlignsAShorterOrLongerSeriesToTheNewestSamples)
{
    const std::array<double, 4> x{-90.0, -50.0, -20.0, 0.0};
    // Shorter: its values are at x = -20 and 0, both in the window.
    const std::array<float, 2> shorter{4.0F, 6.0F};
    EXPECT_DOUBLE_EQ(maxOfSeriesSince(x, -60.0, shorter), 6.0);
    // Longer: its first value has no x at all, its second is at -90 (before the window).
    const std::array<float, 5> longer{99.0F, 98.0F, 1.0F, 2.0F, 3.0F};
    EXPECT_DOUBLE_EQ(maxOfSeriesSince(x, -60.0, longer), 3.0);
}

TEST(RateAxisTest, MaxOfSeriesSinceIsZeroWhenNothingIsInTheWindow)
{
    const std::array<double, 2> x{-200.0, -100.0};
    const std::array<float, 2> s{5.0F, 8.0F};
    EXPECT_DOUBLE_EQ(maxOfSeriesSince(x, -60.0, s), 0.0);
    EXPECT_DOUBLE_EQ(maxOfSeriesSince(std::span<const double>{}, -60.0, std::span<const float>{}), 0.0);
}

TEST(RateAxisTest, MaxOfSeveralSeriesSinceSpansAllOfThemInTheWindow)
{
    const std::array<double, 3> x{-70.0, -10.0, 0.0};
    const std::array<float, 3> a{100.0F, 2.0F, 1.0F};
    const std::array<float, 3> b{200.0F, 3.0F, 9.0F};
    EXPECT_DOUBLE_EQ(maxOfSeriesSince(x, -60.0, a, b), 9.0);
}

TEST(RateAxisTest, FirstIndexAtOrAfterFindsTheWindowsFirstSample)
{
    const std::array<double, 4> x{-61.0, -60.0, -1.0, 0.0};
    EXPECT_EQ(firstIndexAtOrAfter(x, -60.0), 1U);
    EXPECT_EQ(firstIndexAtOrAfter(x, -100.0), 0U);
    EXPECT_EQ(firstIndexAtOrAfter(x, 1.0), 4U);
}

// ========== withCurrentValues (#1145 review) ==========

TEST(RateAxisTest, ASmoothedValueAboveTheVisibleMaxRaisesTheTarget)
{
    // The peak (500) has just left the window, but the bar's smoothed value is still easing down
    // from it: the axis must cover the bar, or normalizeToUnitInterval() clamps it to full height.
    const std::array<double, 4> x{-61.0, -40.0, -20.0, 0.0};
    const std::array<float, 4> s{500.0F, 1.0F, 3.0F, 2.0F};
    const double target = withCurrentValues(maxOfSeriesSince(x, -60.0, s), {120.0, 2.0});
    EXPECT_DOUBLE_EQ(target, 120.0);
    EXPECT_GE(rateAxisUpperBound(target, 1.0), 120.0);
}

TEST(RateAxisTest, CurrentValuesBelowTheVisibleMaxLeaveItUnchanged)
{
    EXPECT_DOUBLE_EQ(withCurrentValues(9.0, {1.0, 8.5}), 9.0);
    EXPECT_DOUBLE_EQ(withCurrentValues(9.0, {}), 9.0);
}

TEST(RateAxisTest, NonFiniteCurrentValuesAreIgnored)
{
    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double INF = std::numeric_limits<double>::infinity();
    EXPECT_DOUBLE_EQ(withCurrentValues(4.0, {NaN}), 4.0);
    EXPECT_DOUBLE_EQ(withCurrentValues(4.0, {NaN, INF, -INF, 6.0}), 6.0);
    // A non-finite or negative visible max counts as 0, as maxOfSeries() would give.
    EXPECT_DOUBLE_EQ(withCurrentValues(NaN, {NaN}), 0.0);
    EXPECT_DOUBLE_EQ(withCurrentValues(-3.0, {2.0}), 2.0);
}

TEST(RateAxisTest, CurrentIfAvailableIsNaNForAnUnavailableReading)
{
    EXPECT_DOUBLE_EQ(currentIfAvailable(true, 42.0), 42.0);
    EXPECT_TRUE(std::isnan(currentIfAvailable(false, 42.0)));
    // An unavailable bar's stale value does not move the axis.
    EXPECT_DOUBLE_EQ(withCurrentValues(5.0, {currentIfAvailable(false, 1000.0)}), 5.0);
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

// ========== Nice axis steps (#1202) ==========

[[nodiscard]] bool isOneTwoFive(double step)
{
    const double mantissa = step / std::pow(10.0, std::floor(std::log10(step)));
    return std::abs(mantissa - 1.0) < 1e-9 || std::abs(mantissa - 2.0) < 1e-9 || std::abs(mantissa - 5.0) < 1e-9;
}

TEST(NiceAxisStepTest, PercentAxisStepsByTwenty)
{
    // 0-100 % with at most 8 labels: 20 (6 labels), where ImPlot printed every 5 % maximized.
    EXPECT_DOUBLE_EQ(niceAxisStep(100.0, AXIS_MAX_TICKS), 20.0);
    EXPECT_DOUBLE_EQ(niceAxisStep(5.0, AXIS_MAX_TICKS), 1.0);
    EXPECT_DOUBLE_EQ(niceAxisStep(1.0, AXIS_MAX_TICKS), 0.2);
}

TEST(NiceAxisStepTest, ExactNiceRawStepIsKept)
{
    // 0-14 over 7 intervals is exactly 2: not pushed up to 5.
    EXPECT_DOUBLE_EQ(niceAxisStep(14.0, 8), 2.0);
    EXPECT_DOUBLE_EQ(niceAxisStep(70.0, 8), 10.0);
}

TEST(NiceAxisStepTest, StepIsAlwaysOneTwoFiveAndRespectsTheCap)
{
    for (const double span : {0.003, 0.7, 1.3, 4.2, 9.5, 17.0, 33.3, 99.9, 762.9, 12'345.0, 3.7e9})
    {
        for (const int maxTicks : {2, 3, 5, 8})
        {
            const double step = niceAxisStep(span, maxTicks);
            ASSERT_GT(step, 0.0) << span;
            EXPECT_TRUE(isOneTwoFive(step)) << "span " << span << " step " << step;
            const auto ticks = axisTickRange(span, step);
            EXPECT_LE(ticks.count, maxTicks) << "span " << span << " maxTicks " << maxTicks;
            // A nice step is at most 2.5x the raw one, so from 5 labels up there are always two or more.
            // Below that a span can get only the 0 tick, and the chart keeps ImPlot's own ticks.
            if (maxTicks >= 5)
            {
                EXPECT_GE(ticks.count, 2) << "span " << span << " maxTicks " << maxTicks;
            }
        }
    }
}

TEST(NiceAxisStepTest, UnusableSpanHasNoStep)
{
    EXPECT_DOUBLE_EQ(niceAxisStep(0.0, 8), 0.0);
    EXPECT_DOUBLE_EQ(niceAxisStep(-5.0, 8), 0.0);
    EXPECT_DOUBLE_EQ(niceAxisStep(std::numeric_limits<double>::quiet_NaN(), 8), 0.0);
    EXPECT_DOUBLE_EQ(niceAxisStep(std::numeric_limits<double>::infinity(), 8), 0.0);
}

TEST(NiceAxisStepTest, BinaryStepIsNiceInTheAxisUnit)
{
    // The reported Network axis: 0-9.5 MB/s stepped by 1.9 MB/s. In MB it is 2 MB/s.
    constexpr double MIB = 1024.0 * 1024.0;
    EXPECT_DOUBLE_EQ(niceBinaryAxisStep(9.5 * MIB, MIB, AXIS_MAX_TICKS), 2.0 * MIB);
    // Disk: 0-762.9 MB/s stepped by 95.4 MB/s; now 200 MB/s.
    EXPECT_DOUBLE_EQ(niceBinaryAxisStep(762.9 * MIB, MIB, AXIS_MAX_TICKS), 200.0 * MIB);
    // A unit scale that is not usable falls back to bytes.
    EXPECT_DOUBLE_EQ(niceBinaryAxisStep(100.0, 0.0, AXIS_MAX_TICKS), 20.0);
}

TEST(AxisTickRangeTest, TicksRunFromZeroToTheLastMultipleBelowTheBound)
{
    const auto ticks = axisTickRange(9.5, 2.0);
    EXPECT_DOUBLE_EQ(ticks.last, 8.0);
    EXPECT_EQ(ticks.count, 5); // 0, 2, 4, 6, 8

    const auto exact = axisTickRange(100.0, 20.0);
    EXPECT_DOUBLE_EQ(exact.last, 100.0);
    EXPECT_EQ(exact.count, 6);

    EXPECT_EQ(axisTickRange(0.0, 1.0).count, 0);
    EXPECT_EQ(axisTickRange(10.0, 0.0).count, 0);
}

TEST(AxisMaxTicksForHeightTest, ShortChartsGetFewerLabels)
{
    EXPECT_EQ(axisMaxTicksForHeight(180.0F, 15.0F), 6);  // 180 / 30
    EXPECT_EQ(axisMaxTicksForHeight(1000.0F, 15.0F), 8); // capped
    EXPECT_EQ(axisMaxTicksForHeight(40.0F, 15.0F), 2);   // floor of 2
    EXPECT_EQ(axisMaxTicksForHeight(-1.0F, 15.0F), AXIS_MAX_TICKS);
    EXPECT_EQ(axisMaxTicksForHeight(180.0F, 0.0F), AXIS_MAX_TICKS);
}

} // namespace
} // namespace UI::Widgets
