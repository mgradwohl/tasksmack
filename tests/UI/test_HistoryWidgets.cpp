#include "UI/HistoryWidgets.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>

namespace UI::Widgets
{
namespace
{

TEST(HistoryWidgetsTest, ComputeAlphaClampsTauMin)
{
    const auto interval = std::chrono::milliseconds(10);
    const double alpha = computeAlpha(0.0, interval);

    const double expected = 1.0 - std::exp(-10.0 / 20.0);
    EXPECT_NEAR(alpha, expected, 1e-6);
}

TEST(HistoryWidgetsTest, ComputeAlphaClampsTauMax)
{
    const auto interval = std::chrono::milliseconds(2000);
    const double alpha = computeAlpha(0.0, interval);

    const double expected = 1.0 - std::exp(-2000.0 / 400.0);
    EXPECT_NEAR(alpha, expected, 1e-6);
}

TEST(HistoryWidgetsTest, ComputeAlphaUsesDeltaTimeWhenPositive)
{
    const auto interval = std::chrono::milliseconds(1000);
    const double alpha = computeAlpha(0.1, interval);

    const double expected = 1.0 - std::exp(-100.0 / 400.0);
    EXPECT_NEAR(alpha, expected, 1e-6);
}

TEST(HistoryWidgetsTest, ComputeAlphaFallsBackForNonPositiveDelta)
{
    const auto interval = std::chrono::milliseconds(1000);
    const double alphaZero = computeAlpha(0.0, interval);
    const double alphaNegative = computeAlpha(-0.05, interval);

    EXPECT_NEAR(alphaZero, alphaNegative, 1e-6);
}

TEST(HistoryWidgetsTest, SmoothTowardsInterpolates)
{
    constexpr double current = 10.0;
    constexpr double target = 20.0;

    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 0.0), current);
    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 1.0), target);
    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 0.25), 12.5);
}

} // namespace
} // namespace UI::Widgets
