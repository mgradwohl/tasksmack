#include "UI/ChartWidgets.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

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

TEST(ChartWidgetsTest, ComputeAlphaFloatOverloadMatchesDoubleOverload)
{
    const auto interval = std::chrono::milliseconds(750);
    const float deltaTime = 0.123F;

    const double fromFloat = computeAlpha(deltaTime, interval);
    const double fromDouble = computeAlpha(static_cast<double>(deltaTime), interval);

    EXPECT_NEAR(fromFloat, fromDouble, 1e-9);
}

TEST(ChartWidgetsTest, SmoothTowardsInterpolates)
{
    constexpr double current = 10.0;
    constexpr double target = 20.0;

    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 0.0), current);
    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 1.0), target);
    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 0.25), 12.5);
}

TEST(ChartWidgetsTest, InitializeOrSmoothReturnsTargetWhenUninitialized)
{
    EXPECT_DOUBLE_EQ(initializeOrSmooth(10.0, 25.0, 0.5, false), 25.0);
}

TEST(ChartWidgetsTest, InitializeOrSmoothAppliesSmoothingWhenInitialized)
{
    EXPECT_DOUBLE_EQ(initializeOrSmooth(10.0, 30.0, 0.25, true), 15.0);
}

// ========== NowBar ==========

TEST(NowBarTest, ExplicitEmptyTooltipTextIsEmpty)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = {}, .color = {}};
    EXPECT_TRUE(bar.tooltipText.empty());
}

TEST(NowBarTest, DefaultValue01IsZero)
{
    const NowBar bar{.valueText = "0%", .label = "CPU", .tooltipText = {}, .color = {}};
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

TEST(NowBarTest, SelectTooltipFallsBackToLabelColonValueWhenTooltipTextEmpty)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = {}, .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "CPU: 50%");
}

TEST(NowBarTest, SelectTooltipFallsBackToValueTextWhenBothEmpty)
{
    const NowBar bar{.valueText = "50%", .label = {}, .tooltipText = {}, .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "50%");
}

TEST(NowBarTest, SelectTooltipFallsBackToLabelWhenValueTextEmpty)
{
    const NowBar bar{.valueText = {}, .label = "CPU", .tooltipText = {}, .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "CPU");
}

// ========== Axis formatters ==========

TEST(ChartWidgetsFormattersTest, FormatAxisLocalizedHandlesSuffixes)
{
    char buf[32]{};
    int len = formatAxisLocalized(1500.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "1.5K");

    len = formatAxisLocalized(2'000'000.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "2.0M");
}

TEST(ChartWidgetsFormattersTest, FormatAxisLocalizedReturnsZeroWhenBufferTooSmall)
{
    char buf[2]{};
    const int len = formatAxisLocalized(999.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_EQ(len, 0);
}

TEST(ChartWidgetsFormattersTest, FormatAxisLocalizedClampsTinyValueToZero)
{
    char buf[32]{};
    const int len = formatAxisLocalized(0.1, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0");
}

TEST(ChartWidgetsFormattersTest, FormatAxisBytesPerSecScalesUnits)
{
    char buf[32]{};
    int len = formatAxisBytesPerSec(100.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "100.0B/s");

    len = formatAxisBytesPerSec(2048.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "2.0KB/s");
}

TEST(ChartWidgetsFormattersTest, FormatAxisBytesPerSecClampsTinyNegativeToZero)
{
    char buf[32]{};
    const int len = formatAxisBytesPerSec(-0.1, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0B/s");
}

TEST(ChartWidgetsFormattersTest, FormatAxisWattsUsesWAndMilliwatts)
{
    char buf[32]{};
    int len = formatAxisWatts(10.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "10.0W");

    len = formatAxisWatts(0.5, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "500.0mW");
}

TEST(ChartWidgetsFormattersTest, FormatAxisWattsClampsTinyNegativeToZeroMilliwatts)
{
    char buf[32]{};
    const int len = formatAxisWatts(-0.00001, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0mW");
}

TEST(ChartWidgetsFormattersTest, FormatAxisPercentFormatsOneDecimal)
{
    char buf[32]{};
    const int len = formatAxisPercent(12.34, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "12.3%");
}

// ========== Time-axis helpers ==========

TEST(ChartWidgetsTimeAxisTest, MakeTimeAxisConfigClampsOffset)
{
    const std::vector<double> timestamps{10.0, 20.0, 30.0, 40.0};
    const auto cfg = makeTimeAxisConfig(timestamps, 5.0, 100.0);

    EXPECT_DOUBLE_EQ(cfg.span, 30.0);
    EXPECT_DOUBLE_EQ(cfg.maxOffset, 25.0);
    EXPECT_DOUBLE_EQ(cfg.clampedOffset, 25.0);
    EXPECT_DOUBLE_EQ(cfg.xMin, -30.0);
    EXPECT_DOUBLE_EQ(cfg.xMax, -25.0);
}

TEST(ChartWidgetsTimeAxisTest, MakeTimeAxisConfigWithEmptyTimestampsUsesDefaultWindow)
{
    const std::vector<double> timestamps{};
    const auto cfg = makeTimeAxisConfig(timestamps, 12.0, 4.0);

    EXPECT_DOUBLE_EQ(cfg.span, 0.0);
    EXPECT_DOUBLE_EQ(cfg.maxOffset, 0.0);
    EXPECT_DOUBLE_EQ(cfg.clampedOffset, 0.0);
    EXPECT_DOUBLE_EQ(cfg.xMin, -12.0);
    EXPECT_DOUBLE_EQ(cfg.xMax, 0.0);
}

TEST(ChartWidgetsTimeAxisTest, MakeTimeAxisConfigClampsNegativeOffsetToZero)
{
    const std::vector<double> timestamps{5.0, 15.0};
    const auto cfg = makeTimeAxisConfig(timestamps, 6.0, -3.0);

    EXPECT_DOUBLE_EQ(cfg.clampedOffset, 0.0);
    EXPECT_DOUBLE_EQ(cfg.xMin, -6.0);
    EXPECT_DOUBLE_EQ(cfg.xMax, 0.0);
}

TEST(ChartWidgetsTimeAxisTest, BuildTimeAxisReturnsRelativeTimes)
{
    const std::vector<double> timestamps{10.0, 20.0, 30.0};
    const auto axis = buildTimeAxis(timestamps, 2, 30.0);

    ASSERT_EQ(axis.size(), 2U);
    EXPECT_FLOAT_EQ(axis[0], -10.0F);
    EXPECT_FLOAT_EQ(axis[1], 0.0F);
}

TEST(ChartWidgetsTimeAxisTest, BuildTimeAxisReturnsEmptyWhenInputEmpty)
{
    const std::vector<double> timestamps{};
    const auto axis = buildTimeAxis(timestamps, 5, 30.0);
    EXPECT_TRUE(axis.empty());
}

TEST(ChartWidgetsTimeAxisTest, BuildTimeAxisDoublesReturnsRelativeTimes)
{
    const std::vector<double> timestamps{10.0, 20.0, 30.0};
    const auto axis = buildTimeAxisDoubles(timestamps, 3, 25.0);

    ASSERT_EQ(axis.size(), 3U);
    EXPECT_DOUBLE_EQ(axis[0], -15.0);
    EXPECT_DOUBLE_EQ(axis[1], -5.0);
    EXPECT_DOUBLE_EQ(axis[2], 5.0);
}

TEST(ChartWidgetsTimeAxisTest, BuildTimeAxisDoublesRespectsDesiredCount)
{
    const std::vector<double> timestamps{1.0, 3.0, 7.0, 9.0};
    const auto axis = buildTimeAxisDoubles(timestamps, 2, 10.0);

    ASSERT_EQ(axis.size(), 2U);
    EXPECT_DOUBLE_EQ(axis[0], -3.0);
    EXPECT_DOUBLE_EQ(axis[1], -1.0);
}

TEST(ChartWidgetsTimeAxisTest, HoveredIndexFromPlotXHandlesBoundsAndMiddle)
{
    const std::vector<float> axisF{-10.0F, -5.0F, 0.0F};
    EXPECT_EQ(hoveredIndexFromPlotX(axisF, -99.0).value(), 0U);
    EXPECT_EQ(hoveredIndexFromPlotX(axisF, 99.0).value(), 2U);
    EXPECT_EQ(hoveredIndexFromPlotX(axisF, -4.2).value(), 1U);

    const std::vector<double> axisD{-10.0, -5.0, 0.0};
    EXPECT_EQ(hoveredIndexFromPlotX(axisD, -9.9).value(), 0U);
    EXPECT_EQ(hoveredIndexFromPlotX(axisD, -2.5).value(), 1U);
}

TEST(ChartWidgetsTimeAxisTest, HoveredIndexFromPlotXTieSelectsLowerNeighbor)
{
    const std::vector<float> axisF{-10.0F, -5.0F};
    EXPECT_EQ(hoveredIndexFromPlotX(axisF, -7.5).value(), 0U);

    const std::vector<double> axisD{-10.0, -5.0};
    EXPECT_EQ(hoveredIndexFromPlotX(axisD, -7.5).value(), 0U);
}

TEST(ChartWidgetsTimeAxisTest, HoveredIndexFromPlotXReturnsNulloptForEmptyInput)
{
    const std::vector<float> axisF{};
    const std::vector<double> axisD{};
    EXPECT_FALSE(hoveredIndexFromPlotX(axisF, 0.0).has_value());
    EXPECT_FALSE(hoveredIndexFromPlotX(axisD, 0.0).has_value());
}

// ========== Generic helpers ==========

TEST(ChartWidgetsHelpersTest, FormatAgeSecondsUsesAbsoluteValue)
{
    EXPECT_EQ(formatAgeSeconds(2.5), "Age: 2.5s");
    EXPECT_EQ(formatAgeSeconds(-2.5), "Age: 2.5s");
}

TEST(ChartWidgetsHelpersTest, CropFrontToSizeRemovesOldestElements)
{
    std::vector<int> data{1, 2, 3, 4, 5};
    cropFrontToSize(data, 3);
    ASSERT_EQ(data.size(), 3U);
    EXPECT_EQ(data[0], 3);
    EXPECT_EQ(data[1], 4);
    EXPECT_EQ(data[2], 5);
}

TEST(ChartWidgetsHelpersTest, CropFrontToSizeNoOpWhenAlreadySmall)
{
    std::vector<int> data{1, 2};
    cropFrontToSize(data, 4);
    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(data[0], 1);
    EXPECT_EQ(data[1], 2);
}

} // namespace
} // namespace UI::Widgets
