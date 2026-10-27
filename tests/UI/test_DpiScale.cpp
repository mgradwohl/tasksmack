/// @file test_DpiScale.cpp
/// @brief Tests for UI::computePointsToPixels(), extracted from UILayer.cpp's
/// pointsToPixels() (#770) so the points->pixels math is testable without a live SDL window
/// (which is where the scale factor itself comes from, via SDL_GetWindowDisplayScale()).

#include "UI/DpiScale.h"

#include <SDL3/SDL_events.h>
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

// #3150: a window minimized before its first frame reported a pixel density of 0.197 on Windows,
// which made the UI scale 8.88 at 175 % and rebuilt the fonts at 213 px.
TEST(DpiScaleTest, WindowUnitScaleIgnoresTheDensityOfAWindowWithNoRealSize)
{
    EXPECT_FLOAT_EQ(windowUnitScale(1.75F, 0.197F), 1.75F);
    EXPECT_FLOAT_EQ(windowUnitScale(1.0F, 0.01F), 1.0F);
    EXPECT_FLOAT_EQ(windowUnitScale(1.0F, 50.0F), 1.0F);
}

// ---- When the UI scale is measured again (#943, #3150) ----

TEST(DpiScaleTest, ADisplayChangeOrARestoreAsksForAScaleCheck)
{
    EXPECT_TRUE(requestsDisplayScaleCheck(SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED));
    EXPECT_TRUE(requestsDisplayScaleCheck(SDL_EVENT_WINDOW_DISPLAY_CHANGED));
    EXPECT_TRUE(requestsDisplayScaleCheck(SDL_EVENT_WINDOW_RESTORED)); // a check that waited while minimized
    EXPECT_FALSE(requestsDisplayScaleCheck(SDL_EVENT_WINDOW_MINIMIZED));
    EXPECT_FALSE(requestsDisplayScaleCheck(SDL_EVENT_WINDOW_RESIZED));
    EXPECT_FALSE(requestsDisplayScaleCheck(SDL_EVENT_MOUSE_MOTION));
}

TEST(DpiScaleTest, AScaleCheckRunsOnceAtTheNextFrame)
{
    DisplayScaleCheck check;
    EXPECT_FALSE(check.pending());
    EXPECT_FALSE(check.takeDue(false)); // nothing asked for

    check.request();
    check.request(); // two events before a frame: still one measurement
    EXPECT_TRUE(check.pending());
    EXPECT_TRUE(check.takeDue(false));
    EXPECT_FALSE(check.pending());
    EXPECT_FALSE(check.takeDue(false));
}

TEST(DpiScaleTest, AScaleCheckWaitsWhileTheWindowIsMinimized)
{
    DisplayScaleCheck check;
    check.request(); // the display change SDL sends at startup, with the window already minimized
    EXPECT_FALSE(check.takeDue(true));
    EXPECT_FALSE(check.takeDue(true)); // every minimized frame keeps it
    EXPECT_TRUE(check.pending());

    check.request(); // SDL_EVENT_WINDOW_RESTORED
    EXPECT_TRUE(check.takeDue(false));
    EXPECT_FALSE(check.pending());
}

TEST(DpiScaleTest, AScaleCheckIsConstexprEvaluable)
{
    constexpr bool measuredAfterRestore = []
    {
        DisplayScaleCheck check;
        check.request();
        const bool whileMinimized = check.takeDue(true);
        return !whileMinimized && check.takeDue(false);
    }();
    static_assert(measuredAfterRestore);
    EXPECT_TRUE(measuredAfterRestore);
}

// ---- Title-bar height (#1169) ----

TEST(DpiScaleTest, TitleBarHeightIsRecomputedForEachScale)
{
    // The fallback font path shares this, so a failed rebuild still resizes the bar.
    EXPECT_FLOAT_EQ(computeTitleBarHeightPx(1.0F), 32.0F);
    EXPECT_FLOAT_EQ(computeTitleBarHeightPx(1.5F), 48.0F);
    EXPECT_FLOAT_EQ(computeTitleBarHeightPx(1.75F), 56.0F);
    EXPECT_FLOAT_EQ(computeTitleBarHeightPx(2.0F), 64.0F);
    EXPECT_FLOAT_EQ(computeTitleBarHeightPx(1.25F), 40.0F);
    EXPECT_FLOAT_EQ(computeTitleBarHeightPx(1.1F), 35.0F); // 35.2, rounded to a whole unit
}

TEST(DpiScaleTest, ChromeIconIsAShareOfTheBar)
{
    EXPECT_FLOAT_EQ(computeChromeIconPx(32.0F), 18.0F); // 17.6
    EXPECT_FLOAT_EQ(computeChromeIconPx(56.0F), 31.0F); // 30.8
}
} // namespace
} // namespace UI
