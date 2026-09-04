/// @file test_DpiScale.cpp
/// @brief Tests for UI::computePointsToPixels(), extracted from UILayer.cpp's
/// pointsToPixels() (#770) so the points->pixels math is testable without a live SDL window
/// (which is where the scale factor itself comes from, via SDL_GetWindowDisplayScale()).

#include "UI/DpiScale.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace UI
