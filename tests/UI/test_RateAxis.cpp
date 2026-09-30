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

} // namespace
} // namespace UI::Widgets
