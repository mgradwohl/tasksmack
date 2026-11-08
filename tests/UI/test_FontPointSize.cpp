/// @file test_FontPointSize.cpp
/// @brief The Font size setting as a point size (#3375): its range and default, the heading and chart sizes that
/// follow from it, reading a hand-edited config value, and the named sizes of earlier versions it replaced.

#include "UI/DpiScale.h"
#include "UI/FontPointSize.h"

#include <gtest/gtest.h>

#include <limits>
#include <optional>

namespace UI
{
namespace
{

/// The old named sizes, as Theme.h had them before #3375: body, heading and chart sizes in points.
struct OldPreset
{
    const char* configName;
    float regularPt;
    float largePt;
    float chartPt;
};

constexpr OldPreset OLD_PRESETS[] = {
    {.configName = "small", .regularPt = 7.0F, .largePt = 9.0F, .chartPt = 7.0F},
    {.configName = "medium", .regularPt = 8.0F, .largePt = 10.0F, .chartPt = 8.0F},
    {.configName = "large", .regularPt = 10.0F, .largePt = 12.0F, .chartPt = 8.0F},
    {.configName = "extra-large", .regularPt = 12.0F, .largePt = 14.0F, .chartPt = 10.0F},
    {.configName = "huge", .regularPt = 14.0F, .largePt = 16.0F, .chartPt = 12.0F},
    {.configName = "even-huger", .regularPt = 16.0F, .largePt = 18.0F, .chartPt = 14.0F},
};

// The default is the old default, Medium, exactly: nothing changes for anyone who never picked a size.
TEST(FontPointSizeTest, TheDefaultIsTheOldMediumSize)
{
    EXPECT_EQ(FONT_POINT_SIZE_DEFAULT, 8);
    const FontPointSizes sizes = fontPointSizesFor(FONT_POINT_SIZE_DEFAULT);
    EXPECT_FLOAT_EQ(sizes.regularPt, 8.0F);
    EXPECT_FLOAT_EQ(sizes.largePt, 10.0F);
    EXPECT_FLOAT_EQ(sizes.chartPt, 8.0F);
    EXPECT_FLOAT_EQ(chartFontScale(FONT_POINT_SIZE_DEFAULT), 1.0F);
}

// Every old named size reads as its body size, and that size draws its headings and charts exactly as the name did.
TEST(FontPointSizeTest, EveryOldNamedSizeMapsToTheSameSizes)
{
    for (const OldPreset& preset : OLD_PRESETS)
    {
        const std::optional<int> points = legacyFontSizePoints(preset.configName);
        ASSERT_TRUE(points.has_value()) << preset.configName;
        const FontPointSizes sizes = fontPointSizesFor(*points);
        EXPECT_FLOAT_EQ(sizes.regularPt, preset.regularPt) << preset.configName;
        EXPECT_FLOAT_EQ(sizes.largePt, preset.largePt) << preset.configName;
        EXPECT_FLOAT_EQ(sizes.chartPt, preset.chartPt) << preset.configName;
    }
}

TEST(FontPointSizeTest, AnUnknownNameIsNoSize)
{
    EXPECT_FALSE(legacyFontSizePoints("").has_value());
    EXPECT_FALSE(legacyFontSizePoints("Medium").has_value());  // config.toml spelled them in lower case
    EXPECT_FALSE(legacyFontSizePoints("largest").has_value()); // Settings' name for even-huger, never written
    EXPECT_FALSE(legacyFontSizePoints("12").has_value());
}

// #1194: no size sets body text under 9 px at 100%, and the range is whole points from 7 to 32.
TEST(FontPointSizeTest, TheRangeKeepsBodyTextAtNinePixelsOrMore)
{
    EXPECT_EQ(FONT_POINT_SIZE_MIN, 7);
    EXPECT_EQ(FONT_POINT_SIZE_MAX, 32);
    EXPECT_GE(computePointsToPixels(static_cast<float>(FONT_POINT_SIZE_MIN), 1.0F), 9.0F);
}

TEST(FontPointSizeTest, ClampKeepsTheSizeInRange)
{
    EXPECT_EQ(clampFontPointSize(FONT_POINT_SIZE_MIN - 1), FONT_POINT_SIZE_MIN);
    EXPECT_EQ(clampFontPointSize(0), FONT_POINT_SIZE_MIN);
    EXPECT_EQ(clampFontPointSize(std::numeric_limits<int>::min()), FONT_POINT_SIZE_MIN);
    EXPECT_EQ(clampFontPointSize(FONT_POINT_SIZE_MIN), FONT_POINT_SIZE_MIN);
    EXPECT_EQ(clampFontPointSize(13), 13);
    EXPECT_EQ(clampFontPointSize(FONT_POINT_SIZE_MAX), FONT_POINT_SIZE_MAX);
    EXPECT_EQ(clampFontPointSize(FONT_POINT_SIZE_MAX + 1), FONT_POINT_SIZE_MAX);
    EXPECT_EQ(clampFontPointSize(std::numeric_limits<int>::max()), FONT_POINT_SIZE_MAX);
}

// A hand edit: rounded to a whole point and clamped; not a number at all is the default.
TEST(FontPointSizeTest, AConfigValueIsRoundedAndClamped)
{
    EXPECT_EQ(fontPointSizeFromConfig(12.0), 12);
    EXPECT_EQ(fontPointSizeFromConfig(11.4), 11);
    EXPECT_EQ(fontPointSizeFromConfig(11.5), 12);
    EXPECT_EQ(fontPointSizeFromConfig(2.0), FONT_POINT_SIZE_MIN);
    EXPECT_EQ(fontPointSizeFromConfig(-8.0), FONT_POINT_SIZE_MIN);
    EXPECT_EQ(fontPointSizeFromConfig(400.0), FONT_POINT_SIZE_MAX);
    EXPECT_EQ(fontPointSizeFromConfig(1e300), FONT_POINT_SIZE_MAX);
    EXPECT_EQ(fontPointSizeFromConfig(std::numeric_limits<double>::infinity()), FONT_POINT_SIZE_DEFAULT);
    EXPECT_EQ(fontPointSizeFromConfig(std::numeric_limits<double>::quiet_NaN()), FONT_POINT_SIZE_DEFAULT);
}

// Over the whole range: headings 2 points up; chart text never larger than the body, never below 8 pt (or the body,
// at 7), and growing with the body size.
TEST(FontPointSizeTest, HeadingAndChartSizesFollowTheBodySize)
{
    int previousChart = 0;
    for (int points = FONT_POINT_SIZE_MIN; points <= FONT_POINT_SIZE_MAX; ++points)
    {
        const FontPointSizes sizes = fontPointSizesFor(points);
        EXPECT_FLOAT_EQ(sizes.regularPt, static_cast<float>(points));
        EXPECT_FLOAT_EQ(sizes.largePt, static_cast<float>(points + FONT_HEADING_POINT_STEP));
        const int chart = chartFontPointSize(points);
        EXPECT_FLOAT_EQ(sizes.chartPt, static_cast<float>(chart));
        EXPECT_LE(chart, points) << points;
        EXPECT_GE(chart, points < CHART_FONT_POINT_FLOOR ? points : CHART_FONT_POINT_FLOOR) << points;
        EXPECT_GE(chart, previousChart) << points;
        EXPECT_LE(chartFontScale(points), 1.0F) << points;
        previousChart = chart;
    }
    EXPECT_EQ(chartFontPointSize(FONT_POINT_SIZE_MAX), FONT_POINT_SIZE_MAX - CHART_FONT_POINT_STEP);
}

// Chart text at the default is at least 10 px at 100% (#1194).
TEST(FontPointSizeTest, ChartTextIsAtLeastTenPixelsAtTheDefault)
{
    EXPECT_GE(computePointsToPixels(fontPointSizesFor(FONT_POINT_SIZE_DEFAULT).chartPt, 1.0F), 10.0F);
}

// Out-of-range input is clamped before the sizes are worked out.
TEST(FontPointSizeTest, SizesForAnOutOfRangeValueAreTheClampedOnes)
{
    EXPECT_FLOAT_EQ(fontPointSizesFor(1).regularPt, static_cast<float>(FONT_POINT_SIZE_MIN));
    EXPECT_FLOAT_EQ(fontPointSizesFor(99).regularPt, static_cast<float>(FONT_POINT_SIZE_MAX));
}

} // namespace
} // namespace UI
