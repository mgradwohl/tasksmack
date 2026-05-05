#include "UI/ChartWidgets.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>

namespace UI::Widgets
{
namespace
{

TEST(ChartWidgetsTest, ComputeAlphaClampsTauMin)
{
    const auto interval = std::chrono::milliseconds(10);
    const double alpha = computeAlpha(0.0, interval);

    const double expected = 1.0 - std::exp(-10.0 / 20.0);
    EXPECT_NEAR(alpha, expected, 1e-6);
}

TEST(ChartWidgetsTest, ComputeAlphaClampsTauMax)
{
    const auto interval = std::chrono::milliseconds(2000);
    const double alpha = computeAlpha(0.0, interval);

    const double expected = 1.0 - std::exp(-2000.0 / 400.0);
    EXPECT_NEAR(alpha, expected, 1e-6);
}

TEST(ChartWidgetsTest, ComputeAlphaUsesDeltaTimeWhenPositive)
{
    const auto interval = std::chrono::milliseconds(1000);
    const double alpha = computeAlpha(0.1, interval);

    const double expected = 1.0 - std::exp(-100.0 / 400.0);
    EXPECT_NEAR(alpha, expected, 1e-6);
}

TEST(ChartWidgetsTest, ComputeAlphaFallsBackForNonPositiveDelta)
{
    const auto interval = std::chrono::milliseconds(1000);
    const double alphaZero = computeAlpha(0.0, interval);
    const double alphaNegative = computeAlpha(-0.05, interval);

    EXPECT_NEAR(alphaZero, alphaNegative, 1e-6);
}

TEST(ChartWidgetsTest, SmoothTowardsInterpolates)
{
    constexpr double current = 10.0;
    constexpr double target = 20.0;

    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 0.0), current);
    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 1.0), target);
    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 0.25), 12.5);
}

// ========== NowBar ==========

TEST(NowBarTest, DefaultTooltipTextIsEmpty)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = {}, .value01 = 0.5, .color = {}};
    EXPECT_TRUE(bar.tooltipText.empty());
}

TEST(NowBarTest, DefaultValue01IsZero)
{
    const NowBar bar{.valueText = "0%", .label = "CPU", .tooltipText = {}, .value01 = 0.0, .color = {}};
    EXPECT_DOUBLE_EQ(bar.value01, 0.0);
}

TEST(NowBarTest, TooltipTextStoresArbitraryContent)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = "CPU Total: 50% (4 cores)", .value01 = 0.5, .color = {}};
    EXPECT_EQ(bar.tooltipText, "CPU Total: 50% (4 cores)");
}

// ========== selectNowBarTooltip ==========

TEST(NowBarTest, SelectTooltipPrefersTooltipText)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = "CPU Total: 50%", .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "CPU Total: 50%");
}

TEST(NowBarTest, SelectTooltipFallsBackToLabelWhenTooltipTextEmpty)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = {}, .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "CPU");
}

TEST(NowBarTest, SelectTooltipFallsBackToValueTextWhenBothEmpty)
{
    const NowBar bar{.valueText = "50%", .label = {}, .tooltipText = {}, .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "50%");
}

} // namespace
} // namespace UI::Widgets
