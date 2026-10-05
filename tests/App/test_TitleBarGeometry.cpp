#include "App/TitleBarGeometry.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

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

// ========== computeRestoreFromMaximizedDragX ==========
// Restoring a maximized window mid-drag should keep the mouse at the same proportional X
// offset it had while maximized (see updateDrag's pendingRestore handling).

TEST(ComputeRestoreFromMaximizedDragXTest, MouseAtLeftEdge_RestoredAtSameGlobalX)
{
    // Mouse was at the maximized window's left edge (proportion 0) -- restored window's left
    // edge should land exactly under the mouse.
    const int result = computeRestoreFromMaximizedDragX(/*startMouseGlobalX=*/0,
                                                        /*maximizedWindowX=*/0,
                                                        /*maximizedWindowWidth=*/1920,
                                                        /*restoredWidth=*/800);
    EXPECT_EQ(result, 0);
}

TEST(ComputeRestoreFromMaximizedDragXTest, MouseAtHorizontalCenter_RestoredCenteredUnderMouse)
{
    // Mouse at proportion 0.5 of a 1920-wide maximized window (global X 960) -- restored
    // window (800 wide) should be centered under the mouse: 960 - 400 = 560.
    const int result = computeRestoreFromMaximizedDragX(/*startMouseGlobalX=*/960,
                                                        /*maximizedWindowX=*/0,
                                                        /*maximizedWindowWidth=*/1920,
                                                        /*restoredWidth=*/800);
    EXPECT_EQ(result, 560);
}

TEST(ComputeRestoreFromMaximizedDragXTest, MouseAtRightEdge_RestoredRightEdgeUnderMouse)
{
    // Proportion 1.0: restored window's right edge lands under the mouse.
    const int result = computeRestoreFromMaximizedDragX(/*startMouseGlobalX=*/1920,
                                                        /*maximizedWindowX=*/0,
                                                        /*maximizedWindowWidth=*/1920,
                                                        /*restoredWidth=*/800);
    EXPECT_EQ(result, 1120);
}

TEST(ComputeRestoreFromMaximizedDragXTest, NonZeroMaximizedWindowX_OffsetsProportionCorrectly)
{
    // Maximized window starts at global X 100 (e.g. a secondary monitor); mouse at global X
    // 1060 is proportion 0.5 of a 1920-wide window relative to that origin.
    const int result = computeRestoreFromMaximizedDragX(/*startMouseGlobalX=*/1060,
                                                        /*maximizedWindowX=*/100,
                                                        /*maximizedWindowWidth=*/1920,
                                                        /*restoredWidth=*/800);
    EXPECT_EQ(result, 660);
}

TEST(ComputeRestoreFromMaximizedDragXTest, ZeroMaximizedWidth_FallsBackToCenteredProportion_NoUBCast)
{
    // Regression test: maximizedWindowWidth <= 0 must not divide by zero (NaN/Inf -> UB on
    // the int cast). Falls back to proportion 0.5: startMouseGlobalX - restoredWidth/2.
    const int result = computeRestoreFromMaximizedDragX(/*startMouseGlobalX=*/500,
                                                        /*maximizedWindowX=*/0,
                                                        /*maximizedWindowWidth=*/0,
                                                        /*restoredWidth=*/800);
    EXPECT_EQ(result, 100); // 500 - 400
}

TEST(ComputeRestoreFromMaximizedDragXTest, NegativeMaximizedWidth_FallsBackToCenteredProportion)
{
    const int result = computeRestoreFromMaximizedDragX(/*startMouseGlobalX=*/500,
                                                        /*maximizedWindowX=*/0,
                                                        /*maximizedWindowWidth=*/-1,
                                                        /*restoredWidth=*/800);
    EXPECT_EQ(result, 100);
}

TEST(ComputeRestoreFromMaximizedDragXTest, MouseOutsideMaximizedWindow_ProportionClampedNotExtrapolated)
{
    // startMouseGlobalX before maximizedWindowX would give a negative raw proportion --
    // clamped to 0 rather than extrapolating past the window's left edge.
    const int belowLeft = computeRestoreFromMaximizedDragX(/*startMouseGlobalX=*/-500,
                                                           /*maximizedWindowX=*/0,
                                                           /*maximizedWindowWidth=*/1920,
                                                           /*restoredWidth=*/800);
    EXPECT_EQ(belowLeft, -500); // proportion clamped to 0.0 -> result == startMouseGlobalX

    // Past the right edge clamps to proportion 1.0 rather than extrapolating further right.
    const int pastRight = computeRestoreFromMaximizedDragX(/*startMouseGlobalX=*/3000,
                                                           /*maximizedWindowX=*/0,
                                                           /*maximizedWindowWidth=*/1920,
                                                           /*restoredWidth=*/800);
    EXPECT_EQ(pastRight, 2200); // 3000 - 800
}

TEST(ComputeRestoreFromMaximizedDragXTest, PureFunction_DeterministicAcrossRepeatedCalls)
{
    const int first = computeRestoreFromMaximizedDragX(777, 50, 1920, 1024);
    const int second = computeRestoreFromMaximizedDragX(777, 50, 1920, 1024);
    EXPECT_EQ(first, second);
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

// ========== computeIsPointInBounds (#769) ==========
// Extracted from TitleBarLayer::isPointInControlArea()'s free-function isInsideBounds()
// helper, so it's testable without linking TitleBarLayer.cpp (real ImGui/SDL calls
// throughout - same reason test_ProcessesPanel.cpp doesn't link ProcessesPanel.cpp).

TEST(ComputeIsPointInBoundsTest, PointInsideReturnsTrue)
{
    const ButtonBounds bounds{.minX = 10.0F, .maxX = 50.0F, .minY = 5.0F, .maxY = 25.0F};
    EXPECT_TRUE(computeIsPointInBounds(30.0F, 15.0F, bounds));
}

TEST(ComputeIsPointInBoundsTest, PointOnEdgeIsInclusive)
{
    const ButtonBounds bounds{.minX = 10.0F, .maxX = 50.0F, .minY = 5.0F, .maxY = 25.0F};
    EXPECT_TRUE(computeIsPointInBounds(10.0F, 5.0F, bounds));
    EXPECT_TRUE(computeIsPointInBounds(50.0F, 25.0F, bounds));
}

TEST(ComputeIsPointInBoundsTest, PointOutsideReturnsFalse)
{
    const ButtonBounds bounds{.minX = 10.0F, .maxX = 50.0F, .minY = 5.0F, .maxY = 25.0F};
    EXPECT_FALSE(computeIsPointInBounds(9.9F, 15.0F, bounds));
    EXPECT_FALSE(computeIsPointInBounds(50.1F, 15.0F, bounds));
    EXPECT_FALSE(computeIsPointInBounds(30.0F, 4.9F, bounds));
    EXPECT_FALSE(computeIsPointInBounds(30.0F, 25.1F, bounds));
}

TEST(ComputeIsPointInBoundsTest, DefaultConstructedBoundsNeverMatch)
{
    // maxX == minX (both 0.0F) is the "unset" sentinel - a button whose bounds haven't been
    // computed yet (e.g. before the first render) must never claim a hit.
    const ButtonBounds unset{};
    EXPECT_FALSE(computeIsPointInBounds(0.0F, 0.0F, unset));
}

// ========== computeDetectResizeEdge (#769) ==========
// Extracted from TitleBarLayer::detectResizeEdge().

TEST(ComputeDetectResizeEdgeTest, MaximizedAlwaysReturnsNone)
{
    // Even a point deep in a corner returns None when maximized - there are no resize
    // borders on a maximized window.
    EXPECT_EQ(computeDetectResizeEdge(0.0F, 0.0F, 800, 600, /*isMaximized=*/true, 8.0F), ResizeEdge::None);
}

TEST(ComputeDetectResizeEdgeTest, InteriorPointReturnsNone)
{
    EXPECT_EQ(computeDetectResizeEdge(400.0F, 300.0F, 800, 600, false, 8.0F), ResizeEdge::None);
}

TEST(ComputeDetectResizeEdgeTest, EdgesAndCorners)
{
    constexpr float THICKNESS = 8.0F;
    constexpr int W = 800;
    constexpr int H = 600;

    EXPECT_EQ(computeDetectResizeEdge(0.0F, 300.0F, W, H, false, THICKNESS), ResizeEdge::Left);
    EXPECT_EQ(computeDetectResizeEdge(static_cast<float>(W) - 1.0F, 300.0F, W, H, false, THICKNESS), ResizeEdge::Right);
    EXPECT_EQ(computeDetectResizeEdge(400.0F, 0.0F, W, H, false, THICKNESS), ResizeEdge::Top);
    EXPECT_EQ(computeDetectResizeEdge(400.0F, static_cast<float>(H) - 1.0F, W, H, false, THICKNESS), ResizeEdge::Bottom);

    EXPECT_EQ(computeDetectResizeEdge(0.0F, 0.0F, W, H, false, THICKNESS), ResizeEdge::TopLeft);
    EXPECT_EQ(computeDetectResizeEdge(static_cast<float>(W) - 1.0F, 0.0F, W, H, false, THICKNESS), ResizeEdge::TopRight);
    EXPECT_EQ(computeDetectResizeEdge(0.0F, static_cast<float>(H) - 1.0F, W, H, false, THICKNESS), ResizeEdge::BottomLeft);
    EXPECT_EQ(computeDetectResizeEdge(static_cast<float>(W) - 1.0F, static_cast<float>(H) - 1.0F, W, H, false, THICKNESS),
              ResizeEdge::BottomRight);
}

TEST(ComputeDetectResizeEdgeTest, ThicknessBoundaryIsExclusiveNearFarEdge)
{
    // x == resizeBorderThickness is just outside the "near left" zone (nearLeft uses <, not
    // <=), so it must not be classified as an edge.
    EXPECT_EQ(computeDetectResizeEdge(8.0F, 300.0F, 800, 600, false, 8.0F), ResizeEdge::None);
}

// Note: TitleBarLayer.cpp's resize-perf-tracing env-var check used to have its own private
// "0"/"false"/"off"/"no" case-insensitive parser here. It was a byte-for-byte duplicate of
// the already-shared, already-tested Core::isEnvFlagEnabled() (Core/EnvUtils.h, covered by
// tests/Core/test_EnvUtils.cpp) - the same helper main.cpp already uses for this exact env
// var. Deleted the duplicate and pointed TitleBarLayer.cpp at Core::isEnvFlagEnabled()
// instead of re-testing the same logic a third time here.

// ========== Title bar chrome sizing (#936: chrome scales with density, not the body font) ==========

TEST(TitleBarGeometryTest, IconIsInsetFromTheBarHeight)
{
    EXPECT_FLOAT_EQ(computeTitleBarIconSize(40.0F, 4.0F), 36.0F);
}

TEST(TitleBarGeometryTest, IconSizeIsNeverNegative)
{
    // ImGui::Image() with a negative extent is undefined; clamp instead.
    EXPECT_FLOAT_EQ(computeTitleBarIconSize(10.0F, 50.0F), 0.0F);
    EXPECT_GE(computeTitleBarIconSize(0.0F, 4.0F), 0.0F);
}

TEST(TitleBarGeometryTest, ButtonWidthKeepsItsAspectAsTheBarGrows)
{
    EXPECT_FLOAT_EQ(computeTitleBarButtonWidth(40.0F, 1.15F), 46.0F);
    EXPECT_FLOAT_EQ(computeTitleBarButtonWidth(80.0F, 1.15F), 92.0F);
}

TEST(TitleBarGeometryTest, ButtonWidthIsNeverNegative)
{
    EXPECT_GE(computeTitleBarButtonWidth(-40.0F, 1.15F), 0.0F);
    EXPECT_GE(computeTitleBarButtonWidth(40.0F, -1.0F), 0.0F);
}

TEST(TitleBarGeometryTest, MatchedGlyphSizeScalesUpAGlyphWithSmallerInk)
{
    // The real case: fa-xmark's ink spans 0.625 em of width against window-minimize's 1.000, so at
    // a shared 18px it comes out 11.25px wide against 18px. Drawing it at 28.8px levels the two.
    EXPECT_FLOAT_EQ(computeMatchedGlyphSize(18.0F, 18.0F, 11.25F), 28.8F);
}

TEST(TitleBarGeometryTest, MatchedGlyphSizeLeavesAnAlreadyMatchingGlyphAlone)
{
    EXPECT_FLOAT_EQ(computeMatchedGlyphSize(18.0F, 18.0F, 18.0F), 18.0F);
}

TEST(TitleBarGeometryTest, MatchedGlyphSizeScalesDownAGlyphWithLargerInk)
{
    EXPECT_FLOAT_EQ(computeMatchedGlyphSize(20.0F, 10.0F, 20.0F), 10.0F);
}

TEST(TitleBarGeometryTest, MatchedGlyphSizeFallsBackToTheReferenceSizeOnUnusableMeasurements)
{
    // A glyph that failed to rasterize measures zero; drawing at the shared size is wrong but
    // readable, where dividing by it would not be.
    EXPECT_FLOAT_EQ(computeMatchedGlyphSize(18.0F, 18.0F, 0.0F), 18.0F);
    EXPECT_FLOAT_EQ(computeMatchedGlyphSize(18.0F, 0.0F, 11.25F), 18.0F);
    EXPECT_FLOAT_EQ(computeMatchedGlyphSize(18.0F, -1.0F, -1.0F), 18.0F);
}

TEST(TitleBarGeometryTest, MatchedGlyphSizeIsNeverNegative)
{
    EXPECT_GE(computeMatchedGlyphSize(-18.0F, 18.0F, 11.25F), 0.0F);
}

// ========== Resize border (#970) ==========

// At a 1.0 display scale the strip is the 8px it always was.
TEST(TitleBarGeometryTest, ResizeBorderIsUnchangedAtTheReferenceScale)
{
    EXPECT_FLOAT_EQ(computeResizeBorderThickness(1.0F), RESIZE_BORDER_REFERENCE_PX);
}

// The strip keeps its physical size on a scaled display instead of halving at 200%.
TEST(TitleBarGeometryTest, ResizeBorderScalesWithTheDisplay)
{
    EXPECT_FLOAT_EQ(computeResizeBorderThickness(1.75F), 14.0F);
    EXPECT_FLOAT_EQ(computeResizeBorderThickness(2.0F), 16.0F);
}

// Whole pixels: the strip is compared against integer window coordinates.
TEST(TitleBarGeometryTest, ResizeBorderIsWholePixels)
{
    EXPECT_FLOAT_EQ(computeResizeBorderThickness(1.3F), 10.0F);
    EXPECT_FLOAT_EQ(computeResizeBorderThickness(1.2F), 10.0F);
}

// A scale below 1.0, or a garbage one from a window that is not mapped yet, must not shrink the
// only resize affordance the borderless window has.
TEST(TitleBarGeometryTest, ResizeBorderIsNeverThinnerThanTheReference)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const float scale : {0.5F, 0.0F, -1.0F, nan, std::numeric_limits<float>::infinity()})
    {
        EXPECT_FLOAT_EQ(computeResizeBorderThickness(scale), RESIZE_BORDER_REFERENCE_PX);
    }
}

// ========== Minimum window size (#970) ==========

// The bar's content: margin, icon, gap, wordmark, gap again, five buttons and the separator.
TEST(TitleBarGeometryTest, TitleBarContentWidthSumsWhatTheBarDraws)
{
    // The 175% bar measured in the report: 56px tall, so margin 11.2, icon 52.64, gap 16.24,
    // buttons 64.4 each, separator 21.84 -- with a 370px wordmark.
    const float width = computeTitleBarContentWidth(11.2F, 52.64F, 16.24F, 370.0F, 64.4F, 21.84F);
    EXPECT_NEAR(width, 11.2F + 52.64F + 16.24F + 370.0F + 16.24F + (64.4F * 5.0F) + 21.84F, 0.01F);
}

TEST(TitleBarGeometryTest, TitleBarContentWidthIgnoresUnusableInputs)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(computeTitleBarContentWidth(nan, -5.0F, nan, 0.0F, 10.0F, nan), 50.0F);
}

// At a 1.0 display scale with nothing measured yet, the minimum is the base minimum it always was.
TEST(TitleBarGeometryTest, MinimumSizeIsTheBaseMinimumAtTheReferenceScale)
{
    const auto minimum = computeMinimumWindowSize(1.0F, 0.0F);
    EXPECT_EQ(minimum.width, MIN);
    EXPECT_EQ(minimum.height, MIN);
}

// The reported case: at 175% the window could be dragged to 200 units wide while its title bar
// needed about 800. The minimum width must cover the bar's content.
TEST(TitleBarGeometryTest, MinimumWidthCoversTheTitleBarContent)
{
    const auto minimum = computeMinimumWindowSize(1.75F, 810.4F);
    EXPECT_EQ(minimum.width, 811); // rounded up: a fraction of a pixel short still overlaps
    EXPECT_EQ(minimum.height, 350);
}

// Content narrower than the scaled base minimum does not lower it.
TEST(TitleBarGeometryTest, MinimumWidthIsNeverBelowTheScaledBase)
{
    const auto minimum = computeMinimumWindowSize(2.0F, 120.0F);
    EXPECT_EQ(minimum.width, 400);
    EXPECT_EQ(minimum.height, 400);
}

TEST(TitleBarGeometryTest, MinimumSizeSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    for (const auto& minimum : {
             computeMinimumWindowSize(nan, nan),
             computeMinimumWindowSize(0.0F, -100.0F),
             computeMinimumWindowSize(inf, inf),
             computeMinimumWindowSize(0.25F, 0.0F),
         })
    {
        EXPECT_EQ(minimum.width, MIN);
        EXPECT_EQ(minimum.height, MIN);
    }

    // Absurdly wide content is held to the maximum dimension rather than overflowing the int.
    EXPECT_EQ(computeMinimumWindowSize(1.0F, 1.0e9F).width, MAX);
    EXPECT_EQ(computeMinimumWindowSize(1.0e6F, 0.0F).height, MAX);
}

// ========== Content floors (#1207) ==========

// The Processes toolbar is the wider floor: the minimum is that row plus the chrome around it.
TEST(TitleBarGeometryTest, ContentMinimumCoversTheProcessesToolbar)
{
    // 16px em: the chart row needs 96 + 15 * 16 = 336px; the toolbar 520px.
    EXPECT_FLOAT_EQ(computeContentMinimumWidth(520.0F, 96.0F, 16.0F, 24.0F), 544.0F);
}

// The NowBar column plus 15 em of plot is the wider floor: at 200 units the Overview's plots were
// about 50px wide.
TEST(TitleBarGeometryTest, ContentMinimumLeavesFifteenEmsOfPlotBesideTheNowBars)
{
    // 32px em: 200 + 15 * 32 = 680px, wider than a 500px toolbar.
    EXPECT_FLOAT_EQ(computeContentMinimumWidth(500.0F, 200.0F, 32.0F, 20.0F), 700.0F);
    EXPECT_FLOAT_EQ(MIN_PLOT_WIDTH_EM, 15.0F);
}

TEST(TitleBarGeometryTest, ContentMinimumIgnoresUnusableInputs)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(computeContentMinimumWidth(nan, -5.0F, nan, -1.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeContentMinimumWidth(0.0F, 0.0F, 10.0F, 0.0F), 150.0F);
}

// The old minimum ignored the panels: at the reference scale the window went down to 200 units
// whatever they needed.
TEST(TitleBarGeometryTest, MinimumWidthCoversThePanelContent)
{
    const auto minimum = computeMinimumWindowSize(1.0F, 300.0F, 640.4F);
    EXPECT_EQ(minimum.width, 641);
    EXPECT_EQ(minimum.height, MIN);
    // The wider of the title bar and the panels wins.
    EXPECT_EQ(computeMinimumWindowSize(1.0F, 900.0F, 640.0F).width, 900);
    // Not known yet: the title bar alone, as before.
    EXPECT_EQ(computeMinimumWindowSize(1.0F, 300.0F).width, 300);
    EXPECT_EQ(computeMinimumWindowSize(1.0F, 300.0F, std::numeric_limits<float>::quiet_NaN()).width, 300);
}

// The status bar's FPS readout was drawn over "Ready" in a narrow window; it is left out instead.
TEST(TitleBarGeometryTest, MinimumIsCappedToTheDisplaysUsableBounds)
{
    // A large font on a small display: four NowBars and 15 em of plot wanted more than the work
    // area, so the window could not fit on-screen and maximize could not satisfy it (#1207).
    const WindowMinimumSize wanted{.width = 1600, .height = 400};
    const auto capped = capMinimumToUsable(wanted, 1280, 984);
    EXPECT_EQ(capped.width, 1280);
    EXPECT_EQ(capped.height, 400);

    EXPECT_EQ(capMinimumToUsable(WindowMinimumSize{.width = 900, .height = 1200}, 1920, 1040).height, 1040);
}

TEST(TitleBarGeometryTest, MinimumThatFitsTheDisplayIsUnchanged)
{
    const WindowMinimumSize wanted{.width = 700, .height = 400};
    EXPECT_EQ(capMinimumToUsable(wanted, 1920, 1040), wanted);
    EXPECT_EQ(capMinimumToUsable(wanted, 700, 400), wanted); // exactly the usable size
}

TEST(TitleBarGeometryTest, MinimumIsUncappedWhenTheUsableBoundsAreUnknown)
{
    const WindowMinimumSize wanted{.width = 1600, .height = 1200};
    EXPECT_EQ(capMinimumToUsable(wanted, 0, 0), wanted);
    EXPECT_EQ(capMinimumToUsable(wanted, -1, 1040).width, 1600);
    EXPECT_EQ(capMinimumToUsable(wanted, -1, 1040).height, 1040);
}

TEST(TitleBarGeometryTest, CappedMinimumNeverFallsBelowTheBaseMinimum)
{
    const WindowMinimumSize wanted{.width = 1600, .height = 1200};
    const auto capped = capMinimumToUsable(wanted, 50, 50);
    EXPECT_EQ(capped.width, MIN);
    EXPECT_EQ(capped.height, MIN);
}

TEST(TitleBarGeometryTest, StatusBarReadoutOnlyWhenItFits)
{
    EXPECT_TRUE(computeStatusBarReadoutFits(60.0F, 300.0F, 8.0F));
    EXPECT_TRUE(computeStatusBarReadoutFits(60.0F, 68.0F, 8.0F)); // exactly the gap
    EXPECT_FALSE(computeStatusBarReadoutFits(60.0F, 67.0F, 8.0F));
    EXPECT_FALSE(computeStatusBarReadoutFits(60.0F, 20.0F, 8.0F)); // would start left of "Ready"'s end
    EXPECT_FALSE(computeStatusBarReadoutFits(std::numeric_limits<float>::quiet_NaN(), 300.0F, 8.0F));
}

// ========== Resize geometry with a caller-supplied minimum (#970) ==========

// Dragging the right edge inward stops at the supplied minimum, not the base one.
TEST(ComputeResizeGeometryTest, SuppliedMinimum_StopsAShrinkFromTheRight)
{
    const auto r = computeResizeGeometry(ResizeEdge::Right, 100, 100, 1200, 800, -1100, 0, 811, 350);
    EXPECT_EQ(r.width, 811);
    EXPECT_EQ(r.x, 100);
}

// From the left, the right edge stays anchored when the minimum bites.
TEST(ComputeResizeGeometryTest, SuppliedMinimum_KeepsTheRightEdgeAnchoredFromTheLeft)
{
    const auto r = computeResizeGeometry(ResizeEdge::Left, 100, 100, 1200, 800, 1100, 0, 811, 350);
    EXPECT_EQ(r.width, 811);
    EXPECT_EQ(r.x + r.width, 100 + 1200);
}

TEST(ComputeResizeGeometryTest, SuppliedMinimum_AppliesToHeightFromTheTop)
{
    const auto r = computeResizeGeometry(ResizeEdge::Top, 100, 100, 1200, 800, 0, 700, 811, 350);
    EXPECT_EQ(r.height, 350);
    EXPECT_EQ(r.y + r.height, 100 + 800);
}

// A supplied minimum can neither undercut the base minimum nor exceed the maximum.
TEST(ComputeResizeGeometryTest, SuppliedMinimum_IsHeldInsideTheAbsoluteBounds)
{
    const auto tooSmall = computeResizeGeometry(ResizeEdge::BottomRight, 0, 0, 800, 600, -5000, -5000, 10, -3);
    EXPECT_EQ(tooSmall.width, MIN);
    EXPECT_EQ(tooSmall.height, MIN);

    const auto tooLarge = computeResizeGeometry(ResizeEdge::BottomRight, 0, 0, 800, 600, 0, 0, MAX * 2, MAX * 2);
    EXPECT_EQ(tooLarge.width, MAX);
    EXPECT_EQ(tooLarge.height, MAX);
}

// #1282 review: a resolution change on the same display keeps its id and the wanted width, so only the
// display event can trigger the refresh -- it must be recognised, and unrelated events must not.
TEST(TitleBarGeometryTest, DisplayModeChangesInvalidateTheUsableBoundsCap)
{
    EXPECT_TRUE(invalidatesUsableBounds(SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED));
    EXPECT_TRUE(invalidatesUsableBounds(SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED));
    EXPECT_TRUE(invalidatesUsableBounds(SDL_EVENT_DISPLAY_ADDED));
    EXPECT_TRUE(invalidatesUsableBounds(SDL_EVENT_DISPLAY_REMOVED));
    EXPECT_FALSE(invalidatesUsableBounds(SDL_EVENT_WINDOW_RESIZED));
    EXPECT_FALSE(invalidatesUsableBounds(SDL_EVENT_MOUSE_MOTION));
}

// ---- selectIconPixelSize (#1169) ----

TEST(TitleBarGeometryTest, IconIsTheSmallestBundledSizeAtLeastAsLargeAsDrawn)
{
    // 100 %: a 30 px icon in a 32 px bar uses the 32 px file, as it always did.
    EXPECT_EQ(selectIconPixelSize(30.0F, APP_ICON_PIXEL_SIZES), 32);
    EXPECT_EQ(selectIconPixelSize(32.0F, APP_ICON_PIXEL_SIZES), 32);
    // 150 % and 200 %: about 45 and 60 px were the 32 px file upscaled; now they are scaled down.
    EXPECT_EQ(selectIconPixelSize(45.0F, APP_ICON_PIXEL_SIZES), 48);
    EXPECT_EQ(selectIconPixelSize(60.0F, APP_ICON_PIXEL_SIZES), 64);
    EXPECT_EQ(selectIconPixelSize(64.5F, APP_ICON_PIXEL_SIZES), 128);
    EXPECT_EQ(selectIconPixelSize(10.0F, APP_ICON_PIXEL_SIZES), 16);
}

TEST(TitleBarGeometryTest, IconLargerThanEveryBundledSizeUsesTheLargest)
{
    EXPECT_EQ(selectIconPixelSize(400.0F, APP_ICON_PIXEL_SIZES), 256);
    EXPECT_EQ(selectIconPixelSize(std::numeric_limits<float>::infinity(), APP_ICON_PIXEL_SIZES), 256);
}

TEST(TitleBarGeometryTest, DegenerateIconSizeUsesTheSmallest)
{
    EXPECT_EQ(selectIconPixelSize(0.0F, APP_ICON_PIXEL_SIZES), 16);
    EXPECT_EQ(selectIconPixelSize(-5.0F, APP_ICON_PIXEL_SIZES), 16);
    EXPECT_EQ(selectIconPixelSize(std::numeric_limits<float>::quiet_NaN(), APP_ICON_PIXEL_SIZES), 16);
}

TEST(TitleBarGeometryTest, IconAtEveryCommonScaleIsNeverUpscaled)
{
    for (const float scale : {1.0F, 1.25F, 1.5F, 1.75F, 2.0F, 2.5F, 3.0F})
    {
        const float barPx = std::round(24.0F * (96.0F * scale) / 72.0F);
        const float iconPx = computeTitleBarIconSize(barPx, barPx * 0.06F); // TitleBarLayer's TITLE_BAR_ICON_INSET_RATIO
        EXPECT_GE(static_cast<float>(selectIconPixelSize(iconPx, APP_ICON_PIXEL_SIZES)), iconPx) << scale;
    }
}
} // namespace
} // namespace App
