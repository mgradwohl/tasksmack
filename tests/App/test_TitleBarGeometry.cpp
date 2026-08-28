#include "App/TitleBarGeometry.h"

#include <gtest/gtest.h>

namespace App
{
namespace
{

constexpr int MIN = Core::WINDOW_MIN_DIMENSION;
constexpr int MAX = Core::WINDOW_MAX_DIMENSION;

// ========== None / zero-delta ==========

TEST(ComputeResizeGeometryTest, NoneEdge_ReturnsOriginalRect)
{
    const auto r = computeResizeGeometry(ResizeEdge::None, 100, 200, 800, 600, 50, 30);
    EXPECT_EQ(r.x, 100);
    EXPECT_EQ(r.y, 200);
    EXPECT_EQ(r.width, 800);
    EXPECT_EQ(r.height, 600);
}

TEST(ComputeResizeGeometryTest, PureFunction_DeterministicAcrossRepeatedCalls)
{
    const auto first = computeResizeGeometry(ResizeEdge::TopLeft, 321, 654, 1024, 768, 37, 19);
    const auto second = computeResizeGeometry(ResizeEdge::TopLeft, 321, 654, 1024, 768, 37, 19);
    EXPECT_EQ(first.x, second.x);
    EXPECT_EQ(first.y, second.y);
    EXPECT_EQ(first.width, second.width);
    EXPECT_EQ(first.height, second.height);
}

TEST(ComputeResizeGeometryTest, ZeroDelta_ReturnsOriginalRect)
{
    const auto r = computeResizeGeometry(ResizeEdge::BottomRight, 10, 20, 800, 600, 0, 0);
    EXPECT_EQ(r.x, 10);
    EXPECT_EQ(r.y, 20);
    EXPECT_EQ(r.width, 800);
    EXPECT_EQ(r.height, 600);
}

// ========== Right edge — x/y unchanged, width changes ==========

TEST(ComputeResizeGeometryTest, RightEdge_GrowsWidth)
{
    const auto r = computeResizeGeometry(ResizeEdge::Right, 0, 0, 800, 600, 100, 0);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.y, 0);
    EXPECT_EQ(r.width, 900);
    EXPECT_EQ(r.height, 600);
}

TEST(ComputeResizeGeometryTest, RightEdge_ShrinksWidth)
{
    const auto r = computeResizeGeometry(ResizeEdge::Right, 0, 0, 800, 600, -200, 0);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.width, 600);
    EXPECT_EQ(r.height, 600);
}

// ========== Left edge — x moves, right edge stays anchored ==========

TEST(ComputeResizeGeometryTest, LeftEdge_MovesXAndShrinksWidth)
{
    const auto r = computeResizeGeometry(ResizeEdge::Left, 100, 200, 800, 600, 50, 0);
    EXPECT_EQ(r.x, 150);
    EXPECT_EQ(r.y, 200);
    EXPECT_EQ(r.width, 750);
    EXPECT_EQ(r.height, 600);
}

TEST(ComputeResizeGeometryTest, LeftEdge_RightEdgeStaysAnchored)
{
    const int origRight = 100 + 800;
    const auto r = computeResizeGeometry(ResizeEdge::Left, 100, 200, 800, 600, 50, 0);
    EXPECT_EQ(r.x + r.width, origRight);
}

TEST(ComputeResizeGeometryTest, LeftEdge_MinClamp_PreservesRightUnderExtremeDrag)
{
    const int startX = 145;
    const int startY = 260;
    const int startWidth = 740;
    const int startHeight = 560;
    const int origRight = startX + startWidth;
    const auto r = computeResizeGeometry(ResizeEdge::Left, startX, startY, startWidth, startHeight, 5000, 0);

    EXPECT_EQ(r.width, MIN);
    EXPECT_EQ(r.x + r.width, origRight);
    EXPECT_EQ(r.y, startY);
    EXPECT_EQ(r.height, startHeight);
}

// ========== Bottom edge — x/y unchanged, height changes ==========

TEST(ComputeResizeGeometryTest, BottomEdge_GrowsHeight)
{
    const auto r = computeResizeGeometry(ResizeEdge::Bottom, 0, 0, 800, 600, 0, 100);
    EXPECT_EQ(r.y, 0);
    EXPECT_EQ(r.height, 700);
}

TEST(ComputeResizeGeometryTest, BottomEdge_ShrinksHeight)
{
    const auto r = computeResizeGeometry(ResizeEdge::Bottom, 0, 0, 800, 600, 0, -200);
    EXPECT_EQ(r.y, 0);
    EXPECT_EQ(r.height, 400);
}

// ========== Top edge — y moves, bottom edge stays anchored ==========

TEST(ComputeResizeGeometryTest, TopEdge_MovesYAndShrinksHeight)
{
    const auto r = computeResizeGeometry(ResizeEdge::Top, 100, 200, 800, 600, 0, 30);
    EXPECT_EQ(r.x, 100);
    EXPECT_EQ(r.y, 230);
    EXPECT_EQ(r.width, 800);
    EXPECT_EQ(r.height, 570);
}

TEST(ComputeResizeGeometryTest, TopEdge_BottomEdgeStaysAnchored)
{
    const int origBottom = 200 + 600;
    const auto r = computeResizeGeometry(ResizeEdge::Top, 100, 200, 800, 600, 0, 30);
    EXPECT_EQ(r.y + r.height, origBottom);
}

TEST(ComputeResizeGeometryTest, TopEdge_MinClamp_PreservesBottomUnderExtremeDrag)
{
    const int startX = 75;
    const int startY = 110;
    const int startWidth = 900;
    const int startHeight = 700;
    const int origBottom = startY + startHeight;
    const auto r = computeResizeGeometry(ResizeEdge::Top, startX, startY, startWidth, startHeight, 0, 5000);

    EXPECT_EQ(r.height, MIN);
    EXPECT_EQ(r.y + r.height, origBottom);
    EXPECT_EQ(r.x, startX);
    EXPECT_EQ(r.width, startWidth);
}

// ========== Corner edges ==========

TEST(ComputeResizeGeometryTest, TopLeft_BothAxesChange)
{
    const auto r = computeResizeGeometry(ResizeEdge::TopLeft, 100, 200, 800, 600, 20, 30);
    EXPECT_EQ(r.x, 120);
    EXPECT_EQ(r.y, 230);
    EXPECT_EQ(r.width, 780);
    EXPECT_EQ(r.height, 570);
}

TEST(ComputeResizeGeometryTest, TopRight_XUnchanged_HeightChanges)
{
    const auto r = computeResizeGeometry(ResizeEdge::TopRight, 100, 200, 800, 600, 50, 30);
    EXPECT_EQ(r.x, 100);
    EXPECT_EQ(r.y, 230);
    EXPECT_EQ(r.width, 850);
    EXPECT_EQ(r.height, 570);
}

TEST(ComputeResizeGeometryTest, BottomLeft_YUnchanged_WidthChanges)
{
    const auto r = computeResizeGeometry(ResizeEdge::BottomLeft, 100, 200, 800, 600, 20, 50);
    EXPECT_EQ(r.x, 120);
    EXPECT_EQ(r.y, 200);
    EXPECT_EQ(r.width, 780);
    EXPECT_EQ(r.height, 650);
}

TEST(ComputeResizeGeometryTest, BottomRight_PositionUnchanged_BothSizesGrow)
{
    const auto r = computeResizeGeometry(ResizeEdge::BottomRight, 50, 60, 800, 600, 100, 100);
    EXPECT_EQ(r.x, 50);
    EXPECT_EQ(r.y, 60);
    EXPECT_EQ(r.width, 900);
    EXPECT_EQ(r.height, 700);
}

// ========== Width clamp — right edge (non-moving, x stays fixed) ==========

TEST(ComputeResizeGeometryTest, RightEdge_ClampAtMin)
{
    const auto r = computeResizeGeometry(ResizeEdge::Right, 0, 0, 800, 600, -700, 0);
    EXPECT_EQ(r.width, MIN);
    EXPECT_EQ(r.x, 0);
}

TEST(ComputeResizeGeometryTest, RightEdge_ClampAtMax)
{
    const auto r = computeResizeGeometry(ResizeEdge::Right, 0, 0, 800, 600, MAX + 100, 0);
    EXPECT_EQ(r.width, MAX);
    EXPECT_EQ(r.x, 0);
}

// ========== Width clamp — left edge (x pinned so right edge stays anchored) ==========

TEST(ComputeResizeGeometryTest, LeftEdge_MinClamp_PinsRightEdge)
{
    // dx large enough to push width below MIN
    const auto r = computeResizeGeometry(ResizeEdge::Left, 100, 0, 800, 600, 700, 0);
    EXPECT_EQ(r.width, MIN);
    EXPECT_EQ(r.x, 100 + (800 - MIN));
    // Right edge must remain at original 100 + 800 = 900
    EXPECT_EQ(r.x + r.width, 100 + 800);
}

TEST(ComputeResizeGeometryTest, LeftEdge_MaxClamp_PinsRightEdge)
{
    // dx large negative enough to push width above MAX
    const auto r = computeResizeGeometry(ResizeEdge::Left, 100, 0, 800, 600, -(MAX + 100), 0);
    EXPECT_EQ(r.width, MAX);
    EXPECT_EQ(r.x, 100 + (800 - MAX));
    // Right edge must remain at original 100 + 800 = 900
    EXPECT_EQ(r.x + r.width, 100 + 800);
}

// ========== Height clamp — bottom edge (non-moving, y stays fixed) ==========

TEST(ComputeResizeGeometryTest, BottomEdge_ClampAtMin)
{
    const auto r = computeResizeGeometry(ResizeEdge::Bottom, 0, 0, 800, 600, 0, -500);
    EXPECT_EQ(r.height, MIN);
    EXPECT_EQ(r.y, 0);
}

TEST(ComputeResizeGeometryTest, BottomEdge_ClampAtMax)
{
    const auto r = computeResizeGeometry(ResizeEdge::Bottom, 0, 0, 800, 600, 0, MAX + 100);
    EXPECT_EQ(r.height, MAX);
    EXPECT_EQ(r.y, 0);
}

// ========== Height clamp — top edge (y pinned so bottom edge stays anchored) ==========

TEST(ComputeResizeGeometryTest, TopEdge_MinClamp_PinsBottomEdge)
{
    // dy large enough to push height below MIN
    const auto r = computeResizeGeometry(ResizeEdge::Top, 0, 200, 800, 600, 0, 500);
    EXPECT_EQ(r.height, MIN);
    EXPECT_EQ(r.y, 200 + (600 - MIN));
    // Bottom edge must remain at original 200 + 600 = 800
    EXPECT_EQ(r.y + r.height, 200 + 600);
}

TEST(ComputeResizeGeometryTest, TopEdge_MaxClamp_PinsBottomEdge)
{
    // dy large negative enough to push height above MAX
    const auto r = computeResizeGeometry(ResizeEdge::Top, 0, 200, 800, 600, 0, -(MAX + 100));
    EXPECT_EQ(r.height, MAX);
    EXPECT_EQ(r.y, 200 + (600 - MAX));
    // Bottom edge must remain at original 200 + 600 = 800
    EXPECT_EQ(r.y + r.height, 200 + 600);
}

// ========== Corner clamp — both axes simultaneously ==========

TEST(ComputeResizeGeometryTest, BottomRight_ClampsBothDimensionsAtMax)
{
    const auto r = computeResizeGeometry(ResizeEdge::BottomRight, 10, 20, 800, 600, MAX, MAX);
    EXPECT_EQ(r.width, MAX);
    EXPECT_EQ(r.height, MAX);
    // BottomRight is not a left/top edge — x and y stay unchanged
    EXPECT_EQ(r.x, 10);
    EXPECT_EQ(r.y, 20);
}

TEST(ComputeResizeGeometryTest, TopLeft_ClampsBothDimensionsAtMin)
{
    // Both deltas shrink width and height below MIN
    const auto r = computeResizeGeometry(ResizeEdge::TopLeft, 100, 200, 800, 600, 700, 500);
    EXPECT_EQ(r.width, MIN);
    EXPECT_EQ(r.height, MIN);
    // x pinned to keep right edge at 100 + 800 = 900
    EXPECT_EQ(r.x, 100 + (800 - MIN));
    EXPECT_EQ(r.x + r.width, 100 + 800);
    // y pinned to keep bottom edge at 200 + 600 = 800
    EXPECT_EQ(r.y, 200 + (600 - MIN));
    EXPECT_EQ(r.y + r.height, 200 + 600);
}

TEST(ComputeResizeGeometryTest, TopRight_MinClampOnHeight_PinsBottomEdge)
{
    // TopRight: dy large, width also grows
    const auto r = computeResizeGeometry(ResizeEdge::TopRight, 100, 200, 800, 600, 100, 500);
    EXPECT_EQ(r.height, MIN);
    EXPECT_EQ(r.y, 200 + (600 - MIN));
    EXPECT_EQ(r.width, 900); // unclamped
    EXPECT_EQ(r.x, 100);     // TopRight does not move x
}

TEST(ComputeResizeGeometryTest, BottomLeft_MinClampOnWidth_PinsRightEdge)
{
    // BottomLeft: dx large positive, height also grows
    const auto r = computeResizeGeometry(ResizeEdge::BottomLeft, 100, 200, 800, 600, 700, 100);
    EXPECT_EQ(r.width, MIN);
    EXPECT_EQ(r.x, 100 + (800 - MIN));
    EXPECT_EQ(r.height, 700); // unclamped
    EXPECT_EQ(r.y, 200);      // BottomLeft does not move y
}

} // namespace
} // namespace App
