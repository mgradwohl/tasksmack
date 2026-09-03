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

// ========== computeTitleBarAreaHitTest ==========
// See #744: on native Wayland, the empty title-bar drag area must return DRAGGABLE (compositor-
// managed drag), but title-bar buttons must stay NORMAL on every backend, since a DRAGGABLE
// result consumes the click before the app ever sees it (button clicks would stop working).

TEST(ComputeTitleBarAreaHitTestTest, ControlArea_AlwaysNormal_RegardlessOfBackend)
{
    EXPECT_EQ(computeTitleBarAreaHitTest(/*isInControlArea=*/true, /*isNativeWayland=*/true), SDL_HITTEST_NORMAL);
    EXPECT_EQ(computeTitleBarAreaHitTest(/*isInControlArea=*/true, /*isNativeWayland=*/false), SDL_HITTEST_NORMAL);
}

TEST(ComputeTitleBarAreaHitTestTest, EmptyDragArea_NativeWayland_ReturnsDraggable)
{
    EXPECT_EQ(computeTitleBarAreaHitTest(/*isInControlArea=*/false, /*isNativeWayland=*/true), SDL_HITTEST_DRAGGABLE);
}

TEST(ComputeTitleBarAreaHitTestTest, EmptyDragArea_NonWayland_ReturnsNormal)
{
    // X11, XWayland, and Windows all keep the client-side drag path.
    EXPECT_EQ(computeTitleBarAreaHitTest(/*isInControlArea=*/false, /*isNativeWayland=*/false), SDL_HITTEST_NORMAL);
}

// ========== computeWindowHitTest ==========
// See #750 review: a title-bar button can sit within resizeBorderThickness of a window edge
// (the close button's rightmost pixels reach the window's right edge; the icon's top pixels sit
// inside the top resize strip), so the control-area check must win over every resize-border
// branch below it -- this exercises the *ordering* itself, not just the terminal helper.

constexpr int WIN_W = 1200;
constexpr int WIN_H = 800;
constexpr float TITLE_BAR_H = 40.0F;
constexpr float BORDER = 8.0F;

TEST(ComputeWindowHitTestTest, ControlArea_WinsOverRightEdgeOverlap)
{
    // Close button's rightmost pixel: geometrically inside the right resize-border strip.
    EXPECT_EQ(computeWindowHitTest(WIN_W - 1.0F,
                                   20.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/true,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, SamePoint_NotControlArea_IsRightEdgeResize)
{
    // Sanity check: absent the control-area guard, that same point is genuinely a resize edge.
    EXPECT_EQ(computeWindowHitTest(WIN_W - 1.0F,
                                   20.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_RESIZE_RIGHT);
}

TEST(ComputeWindowHitTestTest, ControlArea_WinsOverTopEdgeOverlap)
{
    // Icon's top pixels: geometrically inside the top resize-border strip.
    EXPECT_EQ(computeWindowHitTest(20.0F,
                                   5.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/true,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, SamePoint_NotControlArea_IsTopEdgeResize)
{
    EXPECT_EQ(computeWindowHitTest(20.0F,
                                   5.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_RESIZE_TOP);
}

TEST(ComputeWindowHitTestTest, ControlArea_WinsEvenWhenMaximized)
{
    EXPECT_EQ(computeWindowHitTest(WIN_W - 1.0F,
                                   20.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/true,
                                   /*isInControlArea=*/true,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, NotControlArea_BottomLeftCorner_ReturnsResizeBottomLeft)
{
    EXPECT_EQ(computeWindowHitTest(0.0F,
                                   static_cast<float>(WIN_H) - 1.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_RESIZE_BOTTOMLEFT);
}

TEST(ComputeWindowHitTestTest, NotControlArea_BottomRightCorner_ReturnsResizeBottomRight)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) - 1.0F,
                                   static_cast<float>(WIN_H) - 1.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_RESIZE_BOTTOMRIGHT);
}

TEST(ComputeWindowHitTestTest, NotControlArea_BottomEdgeMiddle_ReturnsResizeBottom)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   static_cast<float>(WIN_H) - 1.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_RESIZE_BOTTOM);
}

TEST(ComputeWindowHitTestTest, NotControlArea_TopLeftCorner_ReturnsResizeTopLeft)
{
    EXPECT_EQ(computeWindowHitTest(0.0F,
                                   0.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_RESIZE_TOPLEFT);
}

TEST(ComputeWindowHitTestTest, NotControlArea_TopRightCorner_ReturnsResizeTopRight)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) - 1.0F,
                                   0.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_RESIZE_TOPRIGHT);
}

TEST(ComputeWindowHitTestTest, NotControlArea_LeftEdgeMiddle_ReturnsResizeLeft)
{
    EXPECT_EQ(computeWindowHitTest(0.0F,
                                   TITLE_BAR_H + 50.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_RESIZE_LEFT);
}

TEST(ComputeWindowHitTestTest, Maximized_SuppressesResizeBorders_EvenNearEdge)
{
    // Same point that returned RESIZE_RIGHT when not maximized (see above) falls through to the
    // title-row decision once maximized, since border resize is disabled while maximized.
    EXPECT_EQ(computeWindowHitTest(WIN_W - 1.0F,
                                   20.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/true,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, EmptyTitleRow_NonWayland_ReturnsNormal)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   TITLE_BAR_H - 5.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, EmptyTitleRow_NativeWayland_ReturnsDraggable)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   TITLE_BAR_H - 5.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/true),
              SDL_HITTEST_DRAGGABLE);
}

TEST(ComputeWindowHitTestTest, TitleBarBoundary_JustAboveHeight_StillDraggable_NativeWayland)
{
    // One pixel inside the title-bar row: still the title bar's row, so still draggable.
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   TITLE_BAR_H - 1.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/true),
              SDL_HITTEST_DRAGGABLE);
}

TEST(ComputeWindowHitTestTest, TitleBarBoundary_ExactlyAtHeight_ReturnsNormal_NativeWayland)
{
    // titleBarHeight is an exclusive upper bound: ShellLayer's content window starts at
    // exactly y == titleBarHeight, so this row must not be draggable, or native Wayland
    // would steal its clicks from the first content/tab row (see #750 review).
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   TITLE_BAR_H,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/true),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, TitleBarBoundary_ExactlyAtHeight_ReturnsNormal_NonWayland)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   TITLE_BAR_H,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/false),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, BelowTitleBar_NotBorderNotControl_ReturnsNormal)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   TITLE_BAR_H + 100.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*isNativeWayland=*/true),
              SDL_HITTEST_NORMAL);
}

// ========== computeResizeCursorUpdate: active resize takes priority ==========

TEST(ComputeResizeCursorUpdateTest, ActiveResize_UsesResizeEdge_NoCacheUpdate)
{
    const auto r = computeResizeCursorUpdate(/*isInteracting=*/true,
                                             /*resizeEdge=*/ResizeEdge::Right,
                                             /*focusMismatch=*/false,
                                             /*hoverSampleAvailable=*/false,
                                             /*hoverEdge=*/ResizeEdge::None,
                                             /*cachedEdge=*/ResizeEdge::Left);
    EXPECT_EQ(r.resolvedEdge, ResizeEdge::Right);
    EXPECT_FALSE(r.updateCachedEdge);
    EXPECT_TRUE(r.applyCursor);
}

TEST(ComputeResizeCursorUpdateTest, ActiveDrag_IgnoresHoverSample_ResolvesToNone)
{
    // During a drag (isInteracting=true, resizeEdge=None -- TitleBarLayer clears m_Resize.edge
    // when a drag starts), a hover sample computed this frame must be ignored even though one
    // is available: real hover detection must not run at all while dragging, since the window
    // moves under the pointer and the window-local coordinate can transiently cross into the
    // resize-border zone, flipping in a resize cursor mid-drag (see the #750-follow-up review).
    const auto r = computeResizeCursorUpdate(/*isInteracting=*/true,
                                             /*resizeEdge=*/ResizeEdge::None,
                                             /*focusMismatch=*/false,
                                             /*hoverSampleAvailable=*/true,
                                             /*hoverEdge=*/ResizeEdge::Right,
                                             /*cachedEdge=*/ResizeEdge::None);
    EXPECT_EQ(r.resolvedEdge, ResizeEdge::None);
    EXPECT_FALSE(r.updateCachedEdge);
    EXPECT_FALSE(r.applyCursor);
}

TEST(ComputeResizeCursorUpdateTest, ActiveDrag_ClearsStaleCachedResizeCursor)
{
    // A resize cursor cached from just before the drag started (e.g. the pointer was over a
    // border a frame earlier) must be cleared once dragging begins, not left applied.
    const auto r = computeResizeCursorUpdate(/*isInteracting=*/true,
                                             /*resizeEdge=*/ResizeEdge::None,
                                             /*focusMismatch=*/false,
                                             /*hoverSampleAvailable=*/false,
                                             /*hoverEdge=*/ResizeEdge::None,
                                             /*cachedEdge=*/ResizeEdge::Right);
    EXPECT_EQ(r.resolvedEdge, ResizeEdge::None);
    EXPECT_FALSE(r.updateCachedEdge);
    EXPECT_TRUE(r.applyCursor);
}

// ========== computeResizeCursorUpdate: WM/compositor focus mismatch ==========

TEST(ComputeResizeCursorUpdateTest, FocusMismatch_HoldsCachedEdge_NoCursorTouch)
{
    // Even with a hover sample available, focus mismatch must win: hold the cached edge and
    // never touch the cursor (see #699, #749, #750 review).
    const auto r = computeResizeCursorUpdate(/*isInteracting=*/false,
                                             /*resizeEdge=*/ResizeEdge::None,
                                             /*focusMismatch=*/true,
                                             /*hoverSampleAvailable=*/true,
                                             /*hoverEdge=*/ResizeEdge::Right,
                                             /*cachedEdge=*/ResizeEdge::Left);
    EXPECT_EQ(r.resolvedEdge, ResizeEdge::Left);
    EXPECT_FALSE(r.updateCachedEdge);
    EXPECT_FALSE(r.applyCursor);
}

TEST(ComputeResizeCursorUpdateTest, FocusMismatch_NoCachedEdge_StillNoCursorTouch)
{
    const auto r = computeResizeCursorUpdate(/*isInteracting=*/false,
                                             /*resizeEdge=*/ResizeEdge::None,
                                             /*focusMismatch=*/true,
                                             /*hoverSampleAvailable=*/false,
                                             /*hoverEdge=*/ResizeEdge::None,
                                             /*cachedEdge=*/ResizeEdge::None);
    EXPECT_EQ(r.resolvedEdge, ResizeEdge::None);
    EXPECT_FALSE(r.updateCachedEdge);
    EXPECT_FALSE(r.applyCursor);
}

// ========== computeResizeCursorUpdate: real hover sample (focus restored) ==========

TEST(ComputeResizeCursorUpdateTest, HoverSample_UpdatesCacheAndAppliesNewEdge)
{
    const auto r = computeResizeCursorUpdate(/*isInteracting=*/false,
                                             /*resizeEdge=*/ResizeEdge::None,
                                             /*focusMismatch=*/false,
                                             /*hoverSampleAvailable=*/true,
                                             /*hoverEdge=*/ResizeEdge::TopLeft,
                                             /*cachedEdge=*/ResizeEdge::None);
    EXPECT_EQ(r.resolvedEdge, ResizeEdge::TopLeft);
    EXPECT_TRUE(r.updateCachedEdge);
    EXPECT_TRUE(r.applyCursor);
}

TEST(ComputeResizeCursorUpdateTest, HoverSample_LeavesEdge_RestoresDefaultCursor)
{
    // The pointer left the border this frame: apply None once to restore the default cursor,
    // and cache the new (None) edge so it isn't reapplied every subsequent frame.
    const auto r = computeResizeCursorUpdate(/*isInteracting=*/false,
                                             /*resizeEdge=*/ResizeEdge::None,
                                             /*focusMismatch=*/false,
                                             /*hoverSampleAvailable=*/true,
                                             /*hoverEdge=*/ResizeEdge::None,
                                             /*cachedEdge=*/ResizeEdge::Right);
    EXPECT_EQ(r.resolvedEdge, ResizeEdge::None);
    EXPECT_TRUE(r.updateCachedEdge);
    EXPECT_TRUE(r.applyCursor);
}

// ========== computeResizeCursorUpdate: state-unchanged fast path ==========

TEST(ComputeResizeCursorUpdateTest, StateUnchanged_NonNoneCachedEdge_ReappliesWithoutCacheWrite)
{
    // No new hover sample this frame (mouse/window state unchanged) -- ImGui may have reset
    // the cursor, so still reapply the cached edge, but there's nothing new to cache.
    const auto r = computeResizeCursorUpdate(/*isInteracting=*/false,
                                             /*resizeEdge=*/ResizeEdge::None,
                                             /*focusMismatch=*/false,
                                             /*hoverSampleAvailable=*/false,
                                             /*hoverEdge=*/ResizeEdge::None,
                                             /*cachedEdge=*/ResizeEdge::Bottom);
    EXPECT_EQ(r.resolvedEdge, ResizeEdge::Bottom);
    EXPECT_FALSE(r.updateCachedEdge);
    EXPECT_TRUE(r.applyCursor);
}

TEST(ComputeResizeCursorUpdateTest, StateUnchanged_NoneCachedEdge_NoCursorTouch)
{
    // Nothing to do at all: avoid a needless SDL_SetCursor call when neither the cache nor
    // the resolved edge indicate a border.
    const auto r = computeResizeCursorUpdate(/*isInteracting=*/false,
                                             /*resizeEdge=*/ResizeEdge::None,
                                             /*focusMismatch=*/false,
                                             /*hoverSampleAvailable=*/false,
                                             /*hoverEdge=*/ResizeEdge::None,
                                             /*cachedEdge=*/ResizeEdge::None);
    EXPECT_FALSE(r.updateCachedEdge);
    EXPECT_FALSE(r.applyCursor);
}

TEST(ComputeResizeCursorUpdateTest, PureFunction_DeterministicAcrossRepeatedCalls)
{
    const auto first = computeResizeCursorUpdate(false, ResizeEdge::None, false, true, ResizeEdge::TopRight, ResizeEdge::None);
    const auto second = computeResizeCursorUpdate(false, ResizeEdge::None, false, true, ResizeEdge::TopRight, ResizeEdge::None);
    EXPECT_EQ(first.resolvedEdge, second.resolvedEdge);
    EXPECT_EQ(first.updateCachedEdge, second.updateCachedEdge);
    EXPECT_EQ(first.applyCursor, second.applyCursor);
}

} // namespace
} // namespace App
