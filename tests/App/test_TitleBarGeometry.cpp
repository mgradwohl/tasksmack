#include "App/TitleBarButtons.h"
#include "App/TitleBarGeometry.h"
#include "App/TitleBarView.h" // the bar's layout ratios, for the hit test over its real geometry
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

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

// ========== computeRestoreFromMaximizedDragY ==========
// Restoring a maximized window mid-drag should keep the pointer as far below the restored
// window's top as the press was below the maximized window's top, not leave the window at its
// saved restore Y (#2715).

TEST(ComputeRestoreFromMaximizedDragYTest, PressNearTheTopOfTheTitleBar_TopJustAboveThePointer)
{
    const int result = computeRestoreFromMaximizedDragY(/*startMouseGlobalY=*/2,
                                                        /*maximizedWindowY=*/0,
                                                        /*mouseGlobalY=*/300,
                                                        /*workAreaTop=*/0);
    EXPECT_EQ(result, 298);
}

TEST(ComputeRestoreFromMaximizedDragYTest, PressLowerInTheTitleBar_KeepsThatOffset)
{
    // Maximized below a 48 px top taskbar, pressed 30 px down the title bar.
    const int result = computeRestoreFromMaximizedDragY(/*startMouseGlobalY=*/78,
                                                        /*maximizedWindowY=*/48,
                                                        /*mouseGlobalY=*/500,
                                                        /*workAreaTop=*/48);
    EXPECT_EQ(result, 470);
}

TEST(ComputeRestoreFromMaximizedDragYTest, NearTheScreensTop_ClampedToTheWorkArea)
{
    // Pressed 30 px down, then the pointer moved up to 10 px below the work area's top: the
    // unclamped top would be 20 px above the work area.
    const int result = computeRestoreFromMaximizedDragY(/*startMouseGlobalY=*/30,
                                                        /*maximizedWindowY=*/0,
                                                        /*mouseGlobalY=*/10,
                                                        /*workAreaTop=*/0);
    EXPECT_EQ(result, 0);
}

TEST(ComputeRestoreFromMaximizedDragYTest, OnASecondMonitor_ClampedToItsWorkArea)
{
    // A monitor above the primary one, its work area starting at y = -1080.
    const int result = computeRestoreFromMaximizedDragY(/*startMouseGlobalY=*/-1060,
                                                        /*maximizedWindowY=*/-1080,
                                                        /*mouseGlobalY=*/-1075,
                                                        /*workAreaTop=*/-1080);
    EXPECT_EQ(result, -1080);
}

TEST(ComputeRestoreFromMaximizedDragYTest, PressAboveTheMaximizedWindow_CountsAsNoOffset)
{
    const int result = computeRestoreFromMaximizedDragY(/*startMouseGlobalY=*/-5,
                                                        /*maximizedWindowY=*/0,
                                                        /*mouseGlobalY=*/200,
                                                        /*workAreaTop=*/0);
    EXPECT_EQ(result, 200);
}

// ========== computeTitleBarAreaHitTest ==========
// See #744 and #2716: where the OS or compositor runs the drag (native Wayland, Windows), the empty
// title-bar drag area must return DRAGGABLE, but title-bar buttons must stay NORMAL on every
// backend, since a DRAGGABLE result consumes the click before the app ever sees it (button clicks
// would stop working).

TEST(ComputeTitleBarAreaHitTestTest, ControlArea_AlwaysNormal_RegardlessOfPolicy)
{
    EXPECT_EQ(computeTitleBarAreaHitTest(/*isInControlArea=*/true, /*systemDrag=*/true), SDL_HITTEST_NORMAL);
    EXPECT_EQ(computeTitleBarAreaHitTest(/*isInControlArea=*/true, /*systemDrag=*/false), SDL_HITTEST_NORMAL);
}

TEST(ComputeTitleBarAreaHitTestTest, EmptyDragArea_SystemDrag_ReturnsDraggable)
{
    EXPECT_EQ(computeTitleBarAreaHitTest(/*isInControlArea=*/false, /*systemDrag=*/true), SDL_HITTEST_DRAGGABLE);
}

TEST(ComputeTitleBarAreaHitTestTest, EmptyDragArea_ClientSideDrag_ReturnsNormal)
{
    // X11 and XWayland keep the client-side drag path, as does a client-side maximized window on
    // Windows (computeSystemDragApplies()).
    EXPECT_EQ(computeTitleBarAreaHitTest(/*isInControlArea=*/false, /*systemDrag=*/false), SDL_HITTEST_NORMAL);
}

// ========== computeSystemDragApplies (#2716) ==========

TEST(ComputeSystemDragAppliesTest, ANormalWindowFollowsTheBackend)
{
    EXPECT_TRUE(computeSystemDragApplies(/*usesSystemDrag=*/true, /*clientSideMaximize=*/true, /*isMaximized=*/false));   // Windows
    EXPECT_TRUE(computeSystemDragApplies(/*usesSystemDrag=*/true, /*clientSideMaximize=*/false, /*isMaximized=*/false));  // Wayland
    EXPECT_FALSE(computeSystemDragApplies(/*usesSystemDrag=*/false, /*clientSideMaximize=*/true, /*isMaximized=*/false)); // X11
}

TEST(ComputeSystemDragAppliesTest, AClientSideMaximizedWindowKeepsThePressForTheApp)
{
    // Windows: the OS sees a normal window the size of the work area, so its move loop would drag it
    // around at that size; the client-side drag restores it under the pointer instead (#2715).
    EXPECT_FALSE(computeSystemDragApplies(/*usesSystemDrag=*/true, /*clientSideMaximize=*/true, /*isMaximized=*/true));
    // Native Wayland: the compositor knows the window is maximized and handles the drag.
    EXPECT_TRUE(computeSystemDragApplies(/*usesSystemDrag=*/true, /*clientSideMaximize=*/false, /*isMaximized=*/true));
    EXPECT_FALSE(computeSystemDragApplies(/*usesSystemDrag=*/false, /*clientSideMaximize=*/true, /*isMaximized=*/true));
}

// ========== hitTestResultForEdge ==========

TEST(HitTestResultForEdgeTest, MapsEveryEdgeAndCorner)
{
    EXPECT_EQ(hitTestResultForEdge(ResizeEdge::None), SDL_HITTEST_NORMAL);
    EXPECT_EQ(hitTestResultForEdge(ResizeEdge::Left), SDL_HITTEST_RESIZE_LEFT);
    EXPECT_EQ(hitTestResultForEdge(ResizeEdge::Right), SDL_HITTEST_RESIZE_RIGHT);
    EXPECT_EQ(hitTestResultForEdge(ResizeEdge::Top), SDL_HITTEST_RESIZE_TOP);
    EXPECT_EQ(hitTestResultForEdge(ResizeEdge::Bottom), SDL_HITTEST_RESIZE_BOTTOM);
    EXPECT_EQ(hitTestResultForEdge(ResizeEdge::TopLeft), SDL_HITTEST_RESIZE_TOPLEFT);
    EXPECT_EQ(hitTestResultForEdge(ResizeEdge::TopRight), SDL_HITTEST_RESIZE_TOPRIGHT);
    EXPECT_EQ(hitTestResultForEdge(ResizeEdge::BottomLeft), SDL_HITTEST_RESIZE_BOTTOMLEFT);
    EXPECT_EQ(hitTestResultForEdge(ResizeEdge::BottomRight), SDL_HITTEST_RESIZE_BOTTOMRIGHT);
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, EmptyTitleRow_ClientSideDrag_ReturnsNormal)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   TITLE_BAR_H - 5.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, EmptyTitleRow_SystemDrag_ReturnsDraggable)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   TITLE_BAR_H - 5.0F,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*systemDrag=*/true,
                                   /*systemResize=*/true),
              SDL_HITTEST_DRAGGABLE);
}

TEST(ComputeWindowHitTestTest, TitleBarBoundary_JustAboveHeight_StillDraggable_SystemDrag)
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
                                   /*systemDrag=*/true,
                                   /*systemResize=*/true),
              SDL_HITTEST_DRAGGABLE);
}

TEST(ComputeWindowHitTestTest, TitleBarBoundary_ExactlyAtHeight_ReturnsNormal_SystemDrag)
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
                                   /*systemDrag=*/true,
                                   /*systemResize=*/true),
              SDL_HITTEST_NORMAL);
}

TEST(ComputeWindowHitTestTest, TitleBarBoundary_ExactlyAtHeight_ReturnsNormal_ClientSideDrag)
{
    EXPECT_EQ(computeWindowHitTest(static_cast<float>(WIN_W) / 2.0F,
                                   TITLE_BAR_H,
                                   WIN_W,
                                   WIN_H,
                                   TITLE_BAR_H,
                                   BORDER,
                                   /*isMaximized=*/false,
                                   /*isInControlArea=*/false,
                                   /*systemDrag=*/false,
                                   /*systemResize=*/true),
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
                                   /*systemDrag=*/true,
                                   /*systemResize=*/true),
              SDL_HITTEST_NORMAL);
}

// Windows (#2716): the title bar is the OS's drag, the borders stay the app's resize.
TEST(ComputeWindowHitTestTest, ClientSideResize_BordersAnswerNormal_TitleBarStillDrags)
{
    const auto hitTest = [](float x, float y)
    {
        return computeWindowHitTest(x,
                                    y,
                                    WIN_W,
                                    WIN_H,
                                    TITLE_BAR_H,
                                    BORDER,
                                    /*isMaximized=*/false,
                                    /*isInControlArea=*/false,
                                    /*systemDrag=*/true,
                                    /*systemResize=*/false);
    };
    EXPECT_EQ(hitTest(WIN_W - 1.0F, static_cast<float>(WIN_H) / 2.0F), SDL_HITTEST_NORMAL); // right edge
    EXPECT_EQ(hitTest(0.0F, static_cast<float>(WIN_H) - 1.0F), SDL_HITTEST_NORMAL);         // bottom-left corner
    EXPECT_EQ(hitTest(0.0F, 0.0F), SDL_HITTEST_NORMAL);                                     // top-left corner
    // The top strip crosses the title bar: NORMAL, not DRAGGABLE, so the press reaches the
    // client-side resize rather than the OS move loop.
    EXPECT_EQ(hitTest(static_cast<float>(WIN_W) / 2.0F, BORDER - 1.0F), SDL_HITTEST_NORMAL);
    EXPECT_EQ(hitTest(static_cast<float>(WIN_W) / 2.0F, BORDER), SDL_HITTEST_DRAGGABLE);
    EXPECT_EQ(hitTest(static_cast<float>(WIN_W) / 2.0F, TITLE_BAR_H - 5.0F), SDL_HITTEST_DRAGGABLE);
}

TEST(ComputeWindowHitTestTest, Maximized_NoResizeAnswers_WhoeverWouldServeThem)
{
    for (const bool systemResize : {true, false})
    {
        SCOPED_TRACE(systemResize ? "system resize" : "client-side resize");
        const auto hitTest = [systemResize](float x, float y)
        {
            return computeWindowHitTest(x,
                                        y,
                                        WIN_W,
                                        WIN_H,
                                        TITLE_BAR_H,
                                        BORDER,
                                        /*isMaximized=*/true,
                                        /*isInControlArea=*/false,
                                        /*systemDrag=*/true,
                                        systemResize);
        };
        EXPECT_EQ(hitTest(0.0F, static_cast<float>(WIN_H) - 1.0F), SDL_HITTEST_NORMAL);
        EXPECT_EQ(hitTest(WIN_W - 1.0F, static_cast<float>(WIN_H) - 1.0F), SDL_HITTEST_NORMAL);
        EXPECT_EQ(hitTest(0.0F, static_cast<float>(WIN_H) / 2.0F), SDL_HITTEST_NORMAL);
        // The top strip is the title bar now: a drag where the policy says so.
        EXPECT_EQ(hitTest(static_cast<float>(WIN_W) / 2.0F, 0.0F), SDL_HITTEST_DRAGGABLE);
    }
}

// ========== computeWindowHitTestAt (#2716) ==========
// The geometry-level decision the real callback makes (TitleBarLayer::hitTest()): the buttons and
// the icon laid out as the bar draws them, at the display scale, so every control, edge and corner
// can be asked by position rather than by a precomputed "is in control area" flag.

constexpr int AT_W = 1200;
constexpr int AT_H = 800;
/// The title bar at a 1.0 display scale: 24pt at 96 DPI (UILayer); it scales with the display.
constexpr float BAR_REFERENCE_PX = 32.0F;

/// The bar's geometry at @p scale, laid out as TitleBarView::render() lays it out: the six buttons from
/// the right edge, the icon inset at the left.
[[nodiscard]] WindowHitTestInput hitTestInputAt(float scale, bool systemDrag, bool systemResize, bool isMaximized = false)
{
    const float barHeight = BAR_REFERENCE_PX * scale;
    const float iconSize = computeTitleBarIconSize(barHeight, barHeight * TITLE_BAR_ICON_INSET_RATIO);
    const float iconX = barHeight * TITLE_BAR_EDGE_MARGIN_RATIO;
    const float iconY = (barHeight - iconSize) * 0.5F;
    return {.windowWidth = AT_W,
            .windowHeight = AT_H,
            .titleBarHeightPx = barHeight,
            .resizeBorderPx = computeResizeBorderThickness(scale),
            .buttons = computeTitleBarButtonLayout(static_cast<float>(AT_W),
                                                   computeTitleBarButtonWidth(barHeight, TITLE_BAR_BUTTON_ASPECT),
                                                   barHeight,
                                                   barHeight * TITLE_BAR_SEPARATOR_GAP_RATIO),
            .icon = {.minX = iconX, .maxX = iconX + iconSize, .minY = iconY, .maxY = iconY + iconSize},
            .isMaximized = isMaximized,
            .systemDrag = systemDrag,
            .systemResize = systemResize};
}

constexpr std::array<float, 2> HIT_TEST_SCALES{1.0F, 2.0F};

TEST(ComputeWindowHitTestAtTest, TheEmptyTitleBarDragsWhereTheBackendRunsTheDrag)
{
    for (const float scale : HIT_TEST_SCALES)
    {
        SCOPED_TRACE(scale);
        const WindowHitTestInput system = hitTestInputAt(scale, /*systemDrag=*/true, /*systemResize=*/true);
        const WindowHitTestInput client = hitTestInputAt(scale, /*systemDrag=*/false, /*systemResize=*/true);
        const float middleX = static_cast<float>(AT_W) * 0.5F;
        const float middleY = system.titleBarHeightPx * 0.5F;
        EXPECT_EQ(computeWindowHitTestAt(middleX, middleY, system), SDL_HITTEST_DRAGGABLE);
        EXPECT_EQ(computeWindowHitTestAt(middleX, middleY, client), SDL_HITTEST_NORMAL);
        // The gap between the app buttons and the window controls is still the bar.
        const float gapX = system.buttons.minimize.minX - 1.0F;
        EXPECT_EQ(computeWindowHitTestAt(gapX, middleY, system), SDL_HITTEST_DRAGGABLE);
        // The bar's last row is a drag; the row below it is content.
        EXPECT_EQ(computeWindowHitTestAt(middleX, system.titleBarHeightPx - 1.0F, system), SDL_HITTEST_DRAGGABLE);
        EXPECT_EQ(computeWindowHitTestAt(middleX, system.titleBarHeightPx, system), SDL_HITTEST_NORMAL);
        EXPECT_EQ(computeWindowHitTestAt(middleX, system.titleBarHeightPx + 100.0F, system), SDL_HITTEST_NORMAL);
    }
}

TEST(ComputeWindowHitTestAtTest, EveryButtonAndTheIconAnswerNormal)
{
    for (const float scale : HIT_TEST_SCALES)
    {
        SCOPED_TRACE(scale);
        const WindowHitTestInput input = hitTestInputAt(scale, /*systemDrag=*/true, /*systemResize=*/true);
        const auto& b = input.buttons;
        for (const ButtonBounds& bounds : {b.about, b.help, b.settings, b.minimize, b.maximize, b.close})
        {
            EXPECT_EQ(computeWindowHitTestAt((bounds.minX + bounds.maxX) * 0.5F, (bounds.minY + bounds.maxY) * 0.5F, input),
                      SDL_HITTEST_NORMAL);
            EXPECT_EQ(computeWindowHitTestAt(bounds.minX, bounds.minY, input), SDL_HITTEST_NORMAL);
            EXPECT_EQ(computeWindowHitTestAt(bounds.maxX, bounds.maxY, input), SDL_HITTEST_NORMAL);
        }
        // The close button reaches the window's right edge and the top strip: still the button (#750),
        // except for the top-right resize corner itself (#2882, TheTopRightCornerIsTheBordersNotTheCloseButtons).
        EXPECT_EQ(computeWindowHitTestAt(static_cast<float>(AT_W) - 1.0F, input.resizeBorderPx, input), SDL_HITTEST_NORMAL);
        EXPECT_EQ(computeWindowHitTestAt(static_cast<float>(AT_W) - input.resizeBorderPx - 1.0F, 0.0F, input), SDL_HITTEST_NORMAL);
        // The icon, its top pixels inside the top strip included.
        EXPECT_EQ(computeWindowHitTestAt((input.icon.minX + input.icon.maxX) * 0.5F, (input.icon.minY + input.icon.maxY) * 0.5F, input),
                  SDL_HITTEST_NORMAL);
        EXPECT_EQ(computeWindowHitTestAt(input.icon.minX, input.icon.minY, input), SDL_HITTEST_NORMAL);
    }
}

TEST(ComputeWindowHitTestAtTest, EveryEdgeAndCornerAnswersItsResize)
{
    for (const float scale : HIT_TEST_SCALES)
    {
        SCOPED_TRACE(scale);
        const WindowHitTestInput input = hitTestInputAt(scale, /*systemDrag=*/true, /*systemResize=*/true);
        constexpr float RIGHT = static_cast<float>(AT_W) - 1.0F;
        constexpr float BOTTOM = static_cast<float>(AT_H) - 1.0F;
        constexpr float MID_X = static_cast<float>(AT_W) * 0.5F;
        constexpr float MID_Y = static_cast<float>(AT_H) * 0.5F;
        EXPECT_EQ(computeWindowHitTestAt(0.0F, MID_Y, input), SDL_HITTEST_RESIZE_LEFT);
        EXPECT_EQ(computeWindowHitTestAt(RIGHT, MID_Y, input), SDL_HITTEST_RESIZE_RIGHT);
        EXPECT_EQ(computeWindowHitTestAt(MID_X, 0.0F, input), SDL_HITTEST_RESIZE_TOP);
        EXPECT_EQ(computeWindowHitTestAt(MID_X, BOTTOM, input), SDL_HITTEST_RESIZE_BOTTOM);
        EXPECT_EQ(computeWindowHitTestAt(0.0F, 0.0F, input), SDL_HITTEST_RESIZE_TOPLEFT);
        EXPECT_EQ(computeWindowHitTestAt(0.0F, BOTTOM, input), SDL_HITTEST_RESIZE_BOTTOMLEFT);
        EXPECT_EQ(computeWindowHitTestAt(RIGHT, BOTTOM, input), SDL_HITTEST_RESIZE_BOTTOMRIGHT);
        // The top-right corner too, although the close button's slot reaches it (#2882): the corner
        // square is the border's, like the other three, whether the bar has drawn its buttons yet or
        // not (before the first frame none is laid out).
        EXPECT_EQ(computeWindowHitTestAt(RIGHT, 0.0F, input), SDL_HITTEST_RESIZE_TOPRIGHT);
        WindowHitTestInput firstFrame = input;
        firstFrame.buttons = {};
        firstFrame.icon = {};
        EXPECT_EQ(computeWindowHitTestAt(RIGHT, 0.0F, firstFrame), SDL_HITTEST_RESIZE_TOPRIGHT);

        // The strip is one physical size at every scale: its inner edge moves with the scale.
        const float border = input.resizeBorderPx;
        EXPECT_EQ(computeWindowHitTestAt(border - 1.0F, MID_Y, input), SDL_HITTEST_RESIZE_LEFT);
        EXPECT_EQ(computeWindowHitTestAt(border, MID_Y, input), SDL_HITTEST_NORMAL);
        EXPECT_EQ(computeWindowHitTestAt(MID_X, border - 1.0F, input), SDL_HITTEST_RESIZE_TOP);
        EXPECT_EQ(computeWindowHitTestAt(MID_X, border, input), SDL_HITTEST_DRAGGABLE);
    }
    // The same point, 8px down the middle of the bar, is inside the 8px strip at 1.0 only at 2.0.
    const float middleX = static_cast<float>(AT_W) * 0.5F;
    EXPECT_EQ(computeWindowHitTestAt(middleX, RESIZE_BORDER_REFERENCE_PX, hitTestInputAt(1.0F, true, true)), SDL_HITTEST_DRAGGABLE);
    EXPECT_EQ(computeWindowHitTestAt(middleX, RESIZE_BORDER_REFERENCE_PX, hitTestInputAt(2.0F, true, true)), SDL_HITTEST_RESIZE_TOP);
}

// #2882: the Close button's slot reaches the window's top and right edges, and the controls are
// tested before the borders (#750), so the top-right corner was the one corner that could not start
// a resize. The corner square -- the border's thickness on a side, where the diagonal cursor shows at
// the other three corners -- is the border's; one pixel past it in either direction is the button.
TEST(ComputeWindowHitTestAtTest, TheTopRightCornerIsTheBordersNotTheCloseButtons)
{
    for (const float scale : HIT_TEST_SCALES)
    {
        SCOPED_TRACE(scale);
        const WindowHitTestInput input = hitTestInputAt(scale, /*systemDrag=*/true, /*systemResize=*/true);
        const float border = input.resizeBorderPx;
        constexpr auto WIDTH = static_cast<float>(AT_W);
        ASSERT_LE(input.buttons.close.minX, WIDTH - border)
            << "the close button's slot must still cover the corner for this to test anything";

        // The corner square, its outermost and its innermost pixel, is not the control area but the border.
        EXPECT_FALSE(computeIsPointInControlArea(WIDTH - 1.0F, 0.0F, input));
        EXPECT_FALSE(computeIsPointInControlArea(WIDTH - border, border - 1.0F, input));
        EXPECT_EQ(computeWindowHitTestAt(WIDTH - 1.0F, 0.0F, input), SDL_HITTEST_RESIZE_TOPRIGHT);
        EXPECT_EQ(computeWindowHitTestAt(WIDTH - border, border - 1.0F, input), SDL_HITTEST_RESIZE_TOPRIGHT);
        EXPECT_EQ(computeWindowHitTestAt(WIDTH - (border * 0.5F), border * 0.5F, input), SDL_HITTEST_RESIZE_TOPRIGHT);

        // One pixel inside the square, left of it or below it, is the Close button: a press there closes.
        EXPECT_TRUE(computeIsPointInControlArea(WIDTH - border - 1.0F, border - 1.0F, input));
        EXPECT_TRUE(computeIsPointInControlArea(WIDTH - 1.0F, border, input));
        EXPECT_EQ(computeWindowHitTestAt(WIDTH - border - 1.0F, border - 1.0F, input), SDL_HITTEST_NORMAL);
        EXPECT_EQ(computeWindowHitTestAt(WIDTH - 1.0F, border, input), SDL_HITTEST_NORMAL);
        // The rest of the top strip over the buttons and the right strip down the Close button are
        // unchanged: the button's.
        EXPECT_EQ(computeWindowHitTestAt((input.buttons.maximize.minX + input.buttons.maximize.maxX) * 0.5F, 0.0F, input),
                  SDL_HITTEST_NORMAL);
        EXPECT_EQ(computeWindowHitTestAt(WIDTH - 1.0F, input.titleBarHeightPx - 1.0F, input), SDL_HITTEST_NORMAL);

        // Where the app resizes (Windows), the corner answers NORMAL like every border, so the press is
        // delivered to the app: the control-area decision is what tells it from the button there.
        const WindowHitTestInput windows = hitTestInputAt(scale, /*systemDrag=*/true, /*systemResize=*/false);
        EXPECT_EQ(computeWindowHitTestAt(WIDTH - 1.0F, 0.0F, windows), SDL_HITTEST_NORMAL);
        EXPECT_FALSE(computeIsPointInControlArea(WIDTH - 1.0F, 0.0F, windows));
        EXPECT_TRUE(computeIsPointInControlArea(WIDTH - 1.0F, border, windows));

        // A maximized window has no borders: its Close button reaches the corner, as on Windows.
        const WindowHitTestInput maximized = hitTestInputAt(scale, /*systemDrag=*/true, /*systemResize=*/true, /*isMaximized=*/true);
        EXPECT_TRUE(computeIsPointInControlArea(WIDTH - 1.0F, 0.0F, maximized));
        EXPECT_EQ(computeWindowHitTestAt(WIDTH - 1.0F, 0.0F, maximized), SDL_HITTEST_NORMAL);
    }
}

TEST(ComputeWindowHitTestAtTest, WindowsPolicy_TheTitleBarDragsAndTheBordersStayWithTheApp)
{
    for (const float scale : HIT_TEST_SCALES)
    {
        SCOPED_TRACE(scale);
        const WindowHitTestInput input = hitTestInputAt(scale, /*systemDrag=*/true, /*systemResize=*/false);
        constexpr float RIGHT = static_cast<float>(AT_W) - 1.0F;
        constexpr float BOTTOM = static_cast<float>(AT_H) - 1.0F;
        constexpr float MID_X = static_cast<float>(AT_W) * 0.5F;
        constexpr float MID_Y = static_cast<float>(AT_H) * 0.5F;
        for (const auto [x, y] : std::array<std::pair<float, float>, 7>{
                 {{0.0F, MID_Y}, {RIGHT, MID_Y}, {MID_X, 0.0F}, {MID_X, BOTTOM}, {0.0F, 0.0F}, {0.0F, BOTTOM}, {RIGHT, BOTTOM}}})
        {
            EXPECT_EQ(computeWindowHitTestAt(x, y, input), SDL_HITTEST_NORMAL) << "at (" << x << ", " << y << ")";
        }
        EXPECT_EQ(computeWindowHitTestAt(MID_X, input.titleBarHeightPx * 0.5F, input), SDL_HITTEST_DRAGGABLE);
    }
}

TEST(ComputeWindowHitTestAtTest, Maximized_NoResizeAnswers_TheTitleBarFollowsThePolicy)
{
    for (const float scale : HIT_TEST_SCALES)
    {
        SCOPED_TRACE(scale);
        // Native Wayland: the compositor drags a maximized window. Windows and X11: TitleBarLayer passes
        // systemDrag=false for a client-side maximized window (computeSystemDragApplies()).
        const WindowHitTestInput compositor = hitTestInputAt(scale, /*systemDrag=*/true, /*systemResize=*/true, /*isMaximized=*/true);
        const WindowHitTestInput clientSide = hitTestInputAt(scale, /*systemDrag=*/false, /*systemResize=*/true, /*isMaximized=*/true);
        constexpr float RIGHT = static_cast<float>(AT_W) - 1.0F;
        constexpr float BOTTOM = static_cast<float>(AT_H) - 1.0F;
        constexpr float MID_X = static_cast<float>(AT_W) * 0.5F;
        constexpr float MID_Y = static_cast<float>(AT_H) * 0.5F;
        for (const WindowHitTestInput& input : {compositor, clientSide})
        {
            EXPECT_EQ(computeWindowHitTestAt(0.0F, MID_Y, input), SDL_HITTEST_NORMAL);
            EXPECT_EQ(computeWindowHitTestAt(RIGHT, MID_Y, input), SDL_HITTEST_NORMAL);
            EXPECT_EQ(computeWindowHitTestAt(MID_X, BOTTOM, input), SDL_HITTEST_NORMAL);
            EXPECT_EQ(computeWindowHitTestAt(0.0F, BOTTOM, input), SDL_HITTEST_NORMAL);
            EXPECT_EQ(computeWindowHitTestAt(RIGHT, BOTTOM, input), SDL_HITTEST_NORMAL);
            EXPECT_EQ(computeWindowHitTestAt(RIGHT, 0.0F, input), SDL_HITTEST_NORMAL); // the close button
        }
        // The top strip is the title bar now.
        EXPECT_EQ(computeWindowHitTestAt(MID_X, 0.0F, compositor), SDL_HITTEST_DRAGGABLE);
        EXPECT_EQ(computeWindowHitTestAt(MID_X, compositor.titleBarHeightPx * 0.5F, compositor), SDL_HITTEST_DRAGGABLE);
        EXPECT_EQ(computeWindowHitTestAt(MID_X, 0.0F, clientSide), SDL_HITTEST_NORMAL);
        EXPECT_EQ(computeWindowHitTestAt(MID_X, clientSide.titleBarHeightPx * 0.5F, clientSide), SDL_HITTEST_NORMAL);
    }
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

// ========== computeTitleBarButtonLayout (#1600) ==========
// The title bar's buttons, from the one computation TitleBarLayer both draws them at and keeps out
// of the drag area with. #1600 added About (the circle-i) just left of Help (the circle-?).

constexpr float LAYOUT_RIGHT = 1100.0F;
constexpr float LAYOUT_BUTTON_W = 46.0F;
constexpr float LAYOUT_BAR_H = 32.0F;
constexpr float LAYOUT_GAP = 12.0F;

[[nodiscard]] TitleBarButtonLayout sampleLayout()
{
    return computeTitleBarButtonLayout(LAYOUT_RIGHT, LAYOUT_BUTTON_W, LAYOUT_BAR_H, LAYOUT_GAP);
}

[[nodiscard]] float centreX(const ButtonBounds& bounds)
{
    return (bounds.minX + bounds.maxX) * 0.5F;
}

TEST(ComputeTitleBarButtonLayoutTest, LaysOutAboutHelpSettingsThenTheWindowControls)
{
    const TitleBarButtonLayout layout = sampleLayout();
    // Close touches the right edge; each button abuts the next, except for the gap before Settings.
    EXPECT_FLOAT_EQ(layout.close.maxX, LAYOUT_RIGHT);
    EXPECT_FLOAT_EQ(layout.maximize.maxX, layout.close.minX);
    EXPECT_FLOAT_EQ(layout.minimize.maxX, layout.maximize.minX);
    EXPECT_FLOAT_EQ(layout.settings.maxX, layout.minimize.minX - LAYOUT_GAP);
    EXPECT_FLOAT_EQ(layout.help.maxX, layout.settings.minX);
    EXPECT_FLOAT_EQ(layout.about.maxX, layout.help.minX); // "i" right beside "?"
    EXPECT_FLOAT_EQ(layout.about.minX, LAYOUT_RIGHT - (LAYOUT_BUTTON_W * static_cast<float>(TITLE_BAR_BUTTON_COUNT)) - LAYOUT_GAP);

    for (const ButtonBounds& bounds : {layout.about, layout.help, layout.settings, layout.minimize, layout.maximize, layout.close})
    {
        EXPECT_FLOAT_EQ(bounds.maxX - bounds.minX, LAYOUT_BUTTON_W);
        EXPECT_FLOAT_EQ(bounds.minY, 0.0F);
        EXPECT_FLOAT_EQ(bounds.maxY, LAYOUT_BAR_H);
    }
}

TEST(ComputeTitleBarButtonLayoutTest, TheContentWidthCountsEveryButtonItLaysOut)
{
    // The minimum window width must leave room for every button the layout places, About included.
    const TitleBarButtonLayout layout = sampleLayout();
    const float buttonsWidth = LAYOUT_RIGHT - layout.about.minX;
    EXPECT_FLOAT_EQ(computeTitleBarContentWidth(0.0F, 0.0F, 0.0F, 0.0F, LAYOUT_BUTTON_W, LAYOUT_GAP), buttonsWidth);
}

TEST(ComputeTitleBarButtonLayoutTest, EveryButtonIsExcludedFromTheDragArea)
{
    const TitleBarButtonLayout layout = sampleLayout();
    for (const ButtonBounds& bounds : {layout.about, layout.help, layout.settings, layout.minimize, layout.maximize, layout.close})
    {
        EXPECT_TRUE(computeIsPointOnTitleBarButton(centreX(bounds), LAYOUT_BAR_H * 0.5F, layout));
    }
    // About's edges are included, as every button's are.
    EXPECT_TRUE(computeIsPointOnTitleBarButton(layout.about.minX, 0.0F, layout));
    EXPECT_TRUE(computeIsPointOnTitleBarButton(layout.about.maxX, LAYOUT_BAR_H, layout));
}

TEST(ComputeTitleBarButtonLayoutTest, TheBarLeftOfAboutAndTheGapStayDraggable)
{
    const TitleBarButtonLayout layout = sampleLayout();
    EXPECT_FALSE(computeIsPointOnTitleBarButton(layout.about.minX - 1.0F, LAYOUT_BAR_H * 0.5F, layout));
    EXPECT_FALSE(computeIsPointOnTitleBarButton(layout.minimize.minX - (LAYOUT_GAP * 0.5F), LAYOUT_BAR_H * 0.5F, layout));
    EXPECT_FALSE(computeIsPointOnTitleBarButton(centreX(layout.about), LAYOUT_BAR_H + 1.0F, layout)); // Below the bar
    EXPECT_FALSE(computeIsPointOnTitleBarButton(10.0F, 10.0F, TitleBarButtonLayout{}));               // Not laid out yet
}

TEST(ComputeTitleBarButtonLayoutTest, WhereTheSystemDragsAboutIsAClickNotADrag)
{
    // What the SDL hit test sees: the About button must stay NORMAL (clickable) where the empty bar
    // beside it hands the press to the compositor (#744) or the OS (#2716) as a drag.
    const TitleBarButtonLayout layout = sampleLayout();
    const auto hitTest = [&layout](float x, float y)
    {
        return computeWindowHitTest(x,
                                    y,
                                    1100,
                                    800,
                                    LAYOUT_BAR_H,
                                    8.0F,
                                    /*isMaximized=*/false,
                                    computeIsPointOnTitleBarButton(x, y, layout),
                                    /*systemDrag=*/true,
                                    /*systemResize=*/true);
    };
    EXPECT_EQ(hitTest(centreX(layout.about), LAYOUT_BAR_H * 0.5F), SDL_HITTEST_NORMAL);
    EXPECT_EQ(hitTest(layout.about.minX - 20.0F, LAYOUT_BAR_H * 0.5F), SDL_HITTEST_DRAGGABLE);
}

TEST(TitleBarButtonsTest, AboutAndHelpHaveTheirOwnIconsAndTooltips)
{
    // #1600: the "?" is Help and a new "i" is About, each with its own ImGui ID and tooltip.
    const std::string_view about{TitleBarButtons::ABOUT_LABEL};
    const std::string_view help{TitleBarButtons::HELP_LABEL};
    const std::string_view settings{TitleBarButtons::SETTINGS_LABEL};
    EXPECT_TRUE(about.starts_with(ICON_FA_CIRCLE_INFO));
    EXPECT_TRUE(help.starts_with(ICON_FA_CIRCLE_QUESTION));
    EXPECT_TRUE(settings.starts_with(ICON_FA_GEAR));
    EXPECT_NE(about, help);
    EXPECT_NE(about, settings);
    EXPECT_EQ(std::string_view{TitleBarButtons::ABOUT_TOOLTIP}, "About TaskSmack");
    EXPECT_EQ(std::string_view{TitleBarButtons::HELP_TOOLTIP}, "Help (F1)");
    EXPECT_EQ(std::string_view{TitleBarButtons::SETTINGS_TOOLTIP}, "Settings (F2)");
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

// The bar's content: margin, icon, gap, wordmark, gap again, six buttons and the separator.
TEST(TitleBarGeometryTest, TitleBarContentWidthSumsWhatTheBarDraws)
{
    // The 175% bar measured in the report: 56px tall, so margin 11.2, icon 52.64, gap 16.24,
    // buttons 64.4 each, separator 21.84 -- with a 370px wordmark.
    const float width = computeTitleBarContentWidth(11.2F, 52.64F, 16.24F, 370.0F, 64.4F, 21.84F);
    // Six buttons since #1600: About, Help, Settings, Minimize, Maximize, Close.
    EXPECT_NEAR(width, 11.2F + 52.64F + 16.24F + 370.0F + 16.24F + (64.4F * 6.0F) + 21.84F, 0.01F);
}

TEST(TitleBarGeometryTest, TitleBarContentWidthIgnoresUnusableInputs)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(computeTitleBarContentWidth(nan, -5.0F, nan, 0.0F, 10.0F, nan), 60.0F);
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

// ========== Main tab bar width (#1939) ==========

// Each tab is its label, frame padding either side and ImGui's one pixel; then the gaps between the
// tabs and the gutter either side of the bar.
TEST(TitleBarGeometryTest, MainTabBarWidthIsItsTabsGapsAndInsets)
{
    const std::array<float, 3> labels{100.0F, 50.0F, 80.0F};
    const MainTabBarStyle style{
        .framePaddingXPx = 16.0F, .tabSpacingPx = 4.0F, .minTabWidthPx = 1.0F, .maxTabWidthPx = 260.0F, .insetPx = 12.0F};
    // Tabs: 133 + 83 + 113 = 329; two gaps: 8; insets: 24.
    EXPECT_FLOAT_EQ(computeMainTabBarWidth(labels, style), 361.0F);
}

// A tab is never narrower than TabMinWidthBase nor wider than ImGui's widest tab.
TEST(TitleBarGeometryTest, MainTabBarWidthHoldsEachTabToImGuisLimits)
{
    const std::array<float, 2> labels{2.0F, 1000.0F};
    const MainTabBarStyle style{
        .framePaddingXPx = 4.0F, .tabSpacingPx = 0.0F, .minTabWidthPx = 30.0F, .maxTabWidthPx = 200.0F, .insetPx = 0.0F};
    EXPECT_FLOAT_EQ(computeMainTabBarWidth(labels, style), 230.0F);
    // No cap given: the long tab keeps its whole label.
    MainTabBarStyle uncapped = style;
    uncapped.maxTabWidthPx = 0.0F;
    EXPECT_FLOAT_EQ(computeMainTabBarWidth(labels, uncapped), 30.0F + 1009.0F);
}

TEST(TitleBarGeometryTest, MainTabBarWidthIgnoresUnusableInputs)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const MainTabBarStyle style{
        .framePaddingXPx = nan, .tabSpacingPx = -3.0F, .minTabWidthPx = nan, .maxTabWidthPx = -1.0F, .insetPx = nan};
    const std::array<float, 2> labels{nan, 10.0F};
    EXPECT_FLOAT_EQ(computeMainTabBarWidth(labels, style), 1.0F + 11.0F);
    // No tabs, no bar.
    EXPECT_FLOAT_EQ(computeMainTabBarWidth(std::array<float, 0>{}, style), 0.0F);
}

// The window's minimum width is the wider of the panels' and the tab bar's (ShellLayer): with tabs
// wider than the panels' content, the tab bar's whole width sets the minimum.
TEST(TitleBarGeometryTest, MinimumWidthCoversTheMainTabBar)
{
    const std::array<float, 6> labels{180.0F, 90.0F, 140.0F, 85.0F, 75.0F, 70.0F};
    const MainTabBarStyle style{
        .framePaddingXPx = 28.0F, .tabSpacingPx = 7.0F, .minTabWidthPx = 1.0F, .maxTabWidthPx = 560.0F, .insetPx = 21.0F};
    const float tabBar = computeMainTabBarWidth(labels, style);
    const float panels = computeContentMinimumWidth(500.0F, 100.0F, 16.0F, 60.0F);
    ASSERT_GT(tabBar, panels);
    EXPECT_EQ(computeMinimumWindowSize(1.75F, 0.0F, std::max(panels, tabBar)).width, static_cast<int>(std::ceil(tabBar)));
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

// ========== Content-derived minimum height (#1278) ==========

// The height floor is the sum of what a tab has to show at its shortest.
TEST(TitleBarGeometryTest, ContentMinimumHeightSumsTheParts)
{
    const ContentMinimumHeightParts parts{
        .titleBarPx = 45.0F,
        .mainTabsPx = 58.0F,
        .subTabsPx = 46.0F,
        .chartPx = 210.0F,
        .statusBarPx = 34.0F,
        .chromePx = 12.0F,
    };
    EXPECT_FLOAT_EQ(computeContentMinimumHeight(parts), 405.0F);
}

TEST(TitleBarGeometryTest, ContentMinimumHeightIgnoresUnusableParts)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const ContentMinimumHeightParts parts{
        .titleBarPx = 0.0F, // native decorations: the title bar is outside the client area
        .mainTabsPx = nan,
        .subTabsPx = -5.0F,
        .chartPx = 200.0F,
        .statusBarPx = inf,
        .chromePx = 10.0F,
    };
    EXPECT_FLOAT_EQ(computeContentMinimumHeight(parts), 210.0F);
}

// Style at 150% Medium: a 16px em, 4.5px item spacing, 3px cell padding, 4.5px frame padding.
constexpr ChartBlockStyle STYLE_150{
    .textLineWithSpacingPx = 20.5F,
    .frameHeightWithSpacingPx = 29.5F,
    .itemSpacingYPx = 4.5F,
    .cellPaddingYPx = 3.0F,
    .plotMinHeightPx = 180.0F,
};

// Copilot on #1370: renderHistoryWithNowBars() wraps the plot in a table that keeps its vertical cell
// padding even with compact spacing, so the chart block is the plot plus that padding top and bottom.
TEST(TitleBarGeometryTest, ChartBlockIncludesTheTablesCellPadding)
{
    const ChartBlockLead headingOnly{.textLines = 1, .frameRows = 0, .spacings = 0};
    // heading 20.5 + cell padding 2 * 3 + plot 180 + spacing after 4.5
    EXPECT_FLOAT_EQ(computeChartBlockHeight(STYLE_150, headingOnly), 211.0F);
}

// Copilot on #1370: the GPU tab draws "GPU Monitoring", a Spacing() and the expanded adapter's
// header before its first chart's heading. The minimum must cover that, the tallest tab.
TEST(TitleBarGeometryTest, ChartBlockCoversTheExpandedGpuTab)
{
    // 2 lines (41) + header row (29.5) + Spacing (4.5) + table (6 + 180) + spacing after (4.5)
    EXPECT_FLOAT_EQ(computeChartBlockHeight(STYLE_150, GPU_FIRST_CHART_LEAD), 265.5F);
    EXPECT_FLOAT_EQ(computeTallestFirstChartBlock(STYLE_150), 265.5F);
    EXPECT_GE(computeTallestFirstChartBlock(STYLE_150), computeChartBlockHeight(STYLE_150, OVERVIEW_FIRST_CHART_LEAD));
    EXPECT_GE(computeTallestFirstChartBlock(STYLE_150), computeChartBlockHeight(STYLE_150, NETWORK_FIRST_CHART_LEAD));

    // And the window minimum built from it: title 45 + main tabs 58 + sub-tabs 44.5 + status 34 + chrome 12.
    const float height = computeContentMinimumHeight({
        .titleBarPx = 45.0F,
        .mainTabsPx = 58.0F,
        .subTabsPx = 44.5F,
        .chartPx = computeTallestFirstChartBlock(STYLE_150),
        .statusBarPx = 34.0F,
        .chromePx = 12.0F,
    });
    EXPECT_FLOAT_EQ(height, 459.0F);
    EXPECT_EQ(computeMinimumWindowSize(1.5F, 0.0F, 0.0F, height).height, 459);
}

// The largest lead-in wins whatever the style: a frame padding of zero still leaves the GPU tab
// at least as tall as the Overview, and a combo row never makes Network taller than GPU.
TEST(TitleBarGeometryTest, TallestFirstChartBlockIsTheMaximumOverTheTabs)
{
    const ChartBlockStyle flat{
        .textLineWithSpacingPx = 20.0F,
        .frameHeightWithSpacingPx = 20.0F,
        .itemSpacingYPx = 4.0F,
        .cellPaddingYPx = 2.0F,
        .plotMinHeightPx = 100.0F,
    };
    const float tallest = computeTallestFirstChartBlock(flat);
    EXPECT_FLOAT_EQ(tallest, computeChartBlockHeight(flat, OVERVIEW_FIRST_CHART_LEAD));
    EXPECT_FLOAT_EQ(tallest, computeChartBlockHeight(flat, GPU_FIRST_CHART_LEAD));
}

TEST(TitleBarGeometryTest, ChartBlockIgnoresUnusableInputs)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ChartBlockStyle broken{
        .textLineWithSpacingPx = nan,
        .frameHeightWithSpacingPx = -3.0F,
        .itemSpacingYPx = std::numeric_limits<float>::infinity(),
        .cellPaddingYPx = 2.0F,
        .plotMinHeightPx = 100.0F,
    };
    EXPECT_FLOAT_EQ(computeChartBlockHeight(broken, ChartBlockLead{.textLines = -2, .frameRows = 1, .spacings = 1}), 104.0F);
}

// The minimum height's first-chart part: the estimate, or the tabs' measurement plus the plot at its
// minimum, whichever is taller (#1370 review: a wrapped value strip the estimate cannot count).
TEST(TitleBarGeometryTest, FirstChartBudgetTakesTheTallerOfEstimateAndMeasurement)
{
    // Nothing measured yet: the estimate.
    EXPECT_FLOAT_EQ(computeFirstChartBudget(265.5F, 0.0F, 180.0F), 265.5F);
    // Measured less than the estimate (a wide window): the estimate still holds.
    EXPECT_FLOAT_EQ(computeFirstChartBudget(265.5F, 70.0F, 180.0F), 265.5F);
    // Measured with the GPU strip wrapped onto two extra rows: the measurement wins.
    EXPECT_FLOAT_EQ(computeFirstChartBudget(265.5F, 126.5F, 180.0F), 306.5F);
    // Unusable inputs count as nothing.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(computeFirstChartBudget(265.5F, nan, 180.0F), 265.5F);
    EXPECT_FLOAT_EQ(computeFirstChartBudget(nan, 100.0F, -5.0F), 100.0F);
}

// The reported case: at 150% the minimum height was the scaled base, 300px, which does not hold
// the tab strips, one chart at its minimum and the status bar. The content now sets it.
TEST(TitleBarGeometryTest, MinimumHeightCoversThePanelContent)
{
    const auto minimum = computeMinimumWindowSize(1.5F, 0.0F, 0.0F, 404.3F);
    EXPECT_EQ(minimum.height, 405); // rounded up, as the width is
    EXPECT_EQ(minimum.width, 300);
}

// Content shorter than the scaled base, or not known yet, leaves the base minimum.
TEST(TitleBarGeometryTest, MinimumHeightIsNeverBelowTheScaledBase)
{
    EXPECT_EQ(computeMinimumWindowSize(2.0F, 0.0F, 0.0F, 250.0F).height, 400);
    EXPECT_EQ(computeMinimumWindowSize(1.0F, 0.0F, 0.0F, 0.0F).height, MIN);
    EXPECT_EQ(computeMinimumWindowSize(1.0F, 0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN()).height, MIN);
    EXPECT_EQ(computeMinimumWindowSize(1.0F, 0.0F, 0.0F, -50.0F).height, MIN);
    // Absurdly tall content is held to the maximum dimension.
    EXPECT_EQ(computeMinimumWindowSize(1.0F, 0.0F, 0.0F, 1.0e9F).height, MAX);
}

// A content-derived height taller than the display's work area is capped to it like the width.
TEST(TitleBarGeometryTest, ContentMinimumHeightIsCappedToTheDisplay)
{
    const auto minimum = computeMinimumWindowSize(1.0F, 0.0F, 0.0F, 1500.0F);
    EXPECT_EQ(minimum.height, 1500);
    EXPECT_EQ(capMinimumToUsable(minimum, 1920, 1040).height, 1040);
}

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

// #1945: the window grows to its minimum at startup and when the font, scale or display moved the
// tab-independent part of it; a tab's measurement alone must not resize it.
TEST(TitleBarGeometryTest, TheFirstMinimumMayGrowTheWindow)
{
    EXPECT_EQ(minimumSizeChangeFor(std::nullopt, WindowMinimumSize{.width = 1809, .height = 900}),
              Core::WindowGeometry::MinimumSizeChange::AllowGrow);
}

TEST(TitleBarGeometryTest, AFontOrScaleChangeMayGrowTheWindow)
{
    // Huge -> Even Huger moves the tab-independent minimum: the tab strip and title bar must fit.
    const std::optional<WindowMinimumSize> huge = WindowMinimumSize{.width = 1809, .height = 900};
    EXPECT_EQ(minimumSizeChangeFor(huge, WindowMinimumSize{.width = 2031, .height = 982}),
              Core::WindowGeometry::MinimumSizeChange::AllowGrow);
    // A smaller font moves it too; growing to a lower minimum resizes nothing.
    EXPECT_EQ(minimumSizeChangeFor(huge, WindowMinimumSize{.width = 1033, .height = 700}),
              Core::WindowGeometry::MinimumSizeChange::AllowGrow);
    // Either side alone counts.
    EXPECT_EQ(minimumSizeChangeFor(huge, WindowMinimumSize{.width = 1809, .height = 901}),
              Core::WindowGeometry::MinimumSizeChange::AllowGrow);
}

TEST(TitleBarGeometryTest, ATabMeasurementAloneKeepsTheWindowsSize)
{
    // Showing the machine tab raised the full minimum to 1296 tall, but the tab-independent part is
    // where it was: the window keeps the size it has.
    const std::optional<WindowMinimumSize> huge = WindowMinimumSize{.width = 1809, .height = 900};
    EXPECT_EQ(minimumSizeChangeFor(huge, WindowMinimumSize{.width = 1809, .height = 900}),
              Core::WindowGeometry::MinimumSizeChange::KeepSize);
}

static_assert(minimumSizeChangeFor(std::nullopt, WindowMinimumSize{}) == Core::WindowGeometry::MinimumSizeChange::AllowGrow);
static_assert(minimumSizeChangeFor(WindowMinimumSize{}, WindowMinimumSize{}) == Core::WindowGeometry::MinimumSizeChange::KeepSize);

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
