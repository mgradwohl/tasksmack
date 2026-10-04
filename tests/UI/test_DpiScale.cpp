/// @file test_DpiScale.cpp
/// @brief Tests for UI::computePointsToPixels(), extracted from UILayer.cpp's
/// pointsToPixels() (#770) so the points->pixels math is testable without a live SDL window
/// (which is where the scale factor itself comes from, via SDL_GetWindowDisplayScale()).

#include "UI/DpiScale.h"

#include <gtest/gtest.h>

#include <limits>

namespace UI
{
namespace
{

TEST(DpiScaleTest, UnscaledDisplayMatchesStandard96Dpi)
{
    // At scale 1.0 (96 DPI), 72pt should map to exactly 96px (72pt = 1 inch = 96px at 96 DPI).
    EXPECT_FLOAT_EQ(computePointsToPixels(72.0F, 1.0F), 96.0F);
}

TEST(DpiScaleTest, ZeroPointsIsZeroPixelsRegardlessOfScale)
{
    EXPECT_FLOAT_EQ(computePointsToPixels(0.0F, 1.0F), 0.0F);
    EXPECT_FLOAT_EQ(computePointsToPixels(0.0F, 2.0F), 0.0F);
}

TEST(DpiScaleTest, DoublingScaleDoublesPixels)
{
    const float base = computePointsToPixels(14.0F, 1.0F);
    const float doubled = computePointsToPixels(14.0F, 2.0F);
    EXPECT_FLOAT_EQ(doubled, base * 2.0F);
}

TEST(DpiScaleTest, FractionalScaleMatchesExpectedValue)
{
    // 10pt at 1.5x scale (144 effective DPI): 10 * 144 / 72 = 20px.
    EXPECT_FLOAT_EQ(computePointsToPixels(10.0F, 1.5F), 20.0F);
}

TEST(DpiScaleTest, IsConstexprEvaluable)
{
    constexpr float result = computePointsToPixels(12.0F, 1.0F);
    static_assert(result > 0.0F);
    EXPECT_GT(result, 0.0F);
}

TEST(DpiScaleTest, MovingToADifferentlyScaledMonitorIsAChange)
{
    // Dragging the window from a 100% to a 175% monitor and back (#943).
    EXPECT_TRUE(displayScaleChanged(1.0F, 1.75F));
    EXPECT_TRUE(displayScaleChanged(1.75F, 1.0F));
    EXPECT_TRUE(displayScaleChanged(1.0F, 1.25F));
}

TEST(DpiScaleTest, TheSameScaleIsNotAChange)
{
    // SDL reports the display-changed event for a move between monitors of the same scale too;
    // nothing should be rebuilt then. Floating-point noise below the epsilon is not a change.
    EXPECT_FALSE(displayScaleChanged(1.75F, 1.75F));
    EXPECT_FALSE(displayScaleChanged(1.5F, 1.5F + (DISPLAY_SCALE_EPSILON / 2.0F)));
}

TEST(DpiScaleTest, AnUnusableMeasurementIsNotAChange)
{
    // SDL_GetWindowDisplayScale() returns 0.0 on failure; rebuilding the fonts at that density
    // would make all text vanish.
    EXPECT_FALSE(displayScaleChanged(1.0F, 0.0F));
    EXPECT_FALSE(displayScaleChanged(1.0F, -1.0F));
    EXPECT_FALSE(displayScaleChanged(1.0F, std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(displayScaleChanged(1.0F, std::numeric_limits<float>::infinity()));
}

// windowUnitScale (#1096): the UI scale in window units, so it isn't applied on top of the pixel
// density ImGui already renders at.
TEST(DpiScaleTest, WindowUnitScaleDividesOutThePixelDensity)
{
    EXPECT_FLOAT_EQ(windowUnitScale(2.0F, 2.0F), 1.0F);   // Wayland/macOS 200%: density does it all
    EXPECT_FLOAT_EQ(windowUnitScale(1.5F, 1.0F), 1.5F);   // Windows/X11 150%: density is 1
    EXPECT_FLOAT_EQ(windowUnitScale(2.5F, 2.0F), 1.25F);  // a mixed case
    EXPECT_FLOAT_EQ(windowUnitScale(1.25F, 1.25F), 1.0F); // Wayland fractional 125%
}

TEST(DpiScaleTest, WindowUnitScaleFallsBackOnAnUnusableDensity)
{
    EXPECT_FLOAT_EQ(windowUnitScale(1.5F, 0.0F), 1.5F);
    EXPECT_FLOAT_EQ(windowUnitScale(1.5F, -1.0F), 1.5F);
    EXPECT_FLOAT_EQ(windowUnitScale(1.5F, std::numeric_limits<float>::quiet_NaN()), 1.5F);
    EXPECT_FLOAT_EQ(windowUnitScale(1.5F, std::numeric_limits<float>::infinity()), 1.5F);
}

} // namespace
} // namespace UI
