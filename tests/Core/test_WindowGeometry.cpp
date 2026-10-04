/// @file test_WindowGeometry.cpp
/// @brief Tests for the pure window-geometry decisions in Core/WindowGeometry.h: which rectangle is
/// persisted as the window's normal geometry (#1121), and how a restored rectangle is fitted to the
/// connected displays (#1128).

#include "Core/WindowGeometry.h"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <span>

namespace Core::WindowGeometry
{
namespace
{

constexpr int MIN_VISIBLE = MIN_VISIBLE_EXTENT;

// ---- selectNormalGeometry (#1121) ----

TEST(WindowGeometryTest, NotMaximizedPersistsTheLiveGeometry)
{
    const Rect current{.x = 100, .y = 50, .width = 1024, .height = 768};
    const Rect staleRestore{.x = 1, .y = 2, .width = 300, .height = 200};
    EXPECT_EQ(selectNormalGeometry(false, current, std::nullopt), current);
    EXPECT_EQ(selectNormalGeometry(false, current, staleRestore), current);
}

TEST(WindowGeometryTest, MaximizedPersistsTheRestoreRectangleNotTheMaximizedOne)
{
    // The regression: the maximized (screen-sized) rectangle was saved, became the restore target on
    // the next launch, and Restore then did nothing.
    const Rect maximized{.x = 0, .y = 0, .width = 1920, .height = 1040};
    const Rect normal{.x = 200, .y = 120, .width = 1280, .height = 720};
    EXPECT_EQ(selectNormalGeometry(true, maximized, normal), normal);
}

TEST(WindowGeometryTest, MaximizedWithNoKnownRestoreRectangleKeepsTheSavedGeometry)
{
    // Maximized by the OS or compositor rather than by Window::maximize(): the normal rectangle is
    // unknown, so nothing is returned and the caller keeps what it saved last time.
    const Rect maximized{.x = 0, .y = 0, .width = 1920, .height = 1040};
    EXPECT_EQ(selectNormalGeometry(true, maximized, std::nullopt), std::nullopt);
    EXPECT_EQ(selectNormalGeometry(true, maximized, Rect{.x = 5, .y = 5, .width = 0, .height = 600}), std::nullopt);
    EXPECT_EQ(selectNormalGeometry(true, maximized, Rect{.x = 5, .y = 5, .width = 800, .height = -1}), std::nullopt);
}

// ---- spanOverlap / isReachableOn ----

TEST(WindowGeometryTest, SpanOverlap)
{
    EXPECT_EQ(spanOverlap(0, 100, 50, 100), 50);
    EXPECT_EQ(spanOverlap(0, 100, 100, 100), 0); // Touching edges do not overlap.
    EXPECT_EQ(spanOverlap(-500, 100, 0, 1920), 0);
    EXPECT_EQ(spanOverlap(10, 20, 0, 1920), 20);
}

TEST(WindowGeometryTest, SpanOverlapDoesNotOverflowAtExtremeCoordinates)
{
    EXPECT_EQ(spanOverlap(2'147'483'000, 16'384, 0, 1920), 0);
    EXPECT_EQ(spanOverlap(-2'147'483'000, 16'384, 0, 1920), 0);
}

TEST(WindowGeometryTest, TitleBarAboveTheDisplayIsUnreachable)
{
    const Rect display{.x = 0, .y = 0, .width = 1920, .height = 1080};
    // Most of the window is on screen, but its top edge -- the only place a borderless window can
    // be grabbed -- is above the display.
    EXPECT_FALSE(isReachableOn(Rect{.x = 100, .y = -10, .width = 800, .height = 600}, display, MIN_VISIBLE));
    EXPECT_TRUE(isReachableOn(Rect{.x = 100, .y = 0, .width = 800, .height = 600}, display, MIN_VISIBLE));
}

TEST(WindowGeometryTest, TitleBarAtTheVeryBottomIsUnreachable)
{
    const Rect display{.x = 0, .y = 0, .width = 1920, .height = 1080};
    EXPECT_FALSE(isReachableOn(Rect{.x = 100, .y = 1080 - (MIN_VISIBLE - 1), .width = 800, .height = 600}, display, MIN_VISIBLE));
    EXPECT_TRUE(isReachableOn(Rect{.x = 100, .y = 1080 - MIN_VISIBLE, .width = 800, .height = 600}, display, MIN_VISIBLE));
}

TEST(WindowGeometryTest, ASliverAtTheSideIsUnreachable)
{
    const Rect display{.x = 0, .y = 0, .width = 1920, .height = 1080};
    EXPECT_FALSE(isReachableOn(Rect{.x = 1920 - (MIN_VISIBLE - 1), .y = 100, .width = 800, .height = 600}, display, MIN_VISIBLE));
    EXPECT_TRUE(isReachableOn(Rect{.x = 1920 - MIN_VISIBLE, .y = 100, .width = 800, .height = 600}, display, MIN_VISIBLE));
    EXPECT_FALSE(isReachableOn(Rect{.x = -800 + (MIN_VISIBLE - 1), .y = 100, .width = 800, .height = 600}, display, MIN_VISIBLE));
}

TEST(WindowGeometryTest, AnEmptyDisplayIsNeverReachable)
{
    EXPECT_FALSE(isReachableOn(Rect{.x = 0, .y = 0, .width = 800, .height = 600}, Rect{}, MIN_VISIBLE));
}

// ---- fitRectToDisplays (#1128) ----

TEST(WindowGeometryTest, OffToTheLeftOfAnUnpluggedMonitorIsCentredOnThePrimary)
{
    // The issue's scenario: saved on an external monitor at x = -1920, which is now disconnected.
    const std::array displays{Rect{.x = 0, .y = 0, .width = 1920, .height = 1040}};
    const Rect saved{.x = -1700, .y = 100, .width = 1280, .height = 720};
    const Rect fitted = fitRectToDisplays(saved, displays, 0, MIN_VISIBLE);
    EXPECT_EQ(fitted, (Rect{.x = 320, .y = 160, .width = 1280, .height = 720}));
}

TEST(WindowGeometryTest, APartlyVisibleWindowKeepsItsPosition)
{
    // Hanging off the right edge but with its title bar on screen: the user put it there, and it
    // can be dragged back, so it is left alone.
    const std::array displays{Rect{.x = 0, .y = 0, .width = 1920, .height = 1040}};
    const Rect saved{.x = 1500, .y = 300, .width = 1280, .height = 720};
    EXPECT_EQ(fitRectToDisplays(saved, displays, 0, MIN_VISIBLE), saved);
}

TEST(WindowGeometryTest, AWindowLargerThanTheDisplayIsShrunkToFit)
{
    const std::array displays{Rect{.x = 0, .y = 0, .width = 1366, .height = 728}};
    const Rect saved{.x = 0, .y = 0, .width = 2560, .height = 1400};
    EXPECT_EQ(fitRectToDisplays(saved, displays, 0, MIN_VISIBLE), (Rect{.x = 0, .y = 0, .width = 1366, .height = 728}));
}

TEST(WindowGeometryTest, AnOffScreenWindowLargerThanThePrimaryIsShrunkAndCentred)
{
    const std::array displays{Rect{.x = 0, .y = 0, .width = 1366, .height = 728}};
    const Rect saved{.x = 5000, .y = 5000, .width = 2560, .height = 1400};
    EXPECT_EQ(fitRectToDisplays(saved, displays, 0, MIN_VISIBLE), (Rect{.x = 0, .y = 0, .width = 1366, .height = 728}));
}

TEST(WindowGeometryTest, AWindowOnASecondaryMonitorStaysThere)
{
    // A second monitor to the left of the primary, as in the issue, still connected.
    const std::array displays{Rect{.x = 0, .y = 0, .width = 1920, .height = 1040}, Rect{.x = -1920, .y = 0, .width = 1920, .height = 1040}};
    const Rect saved{.x = -1700, .y = 100, .width = 1280, .height = 720};
    EXPECT_EQ(fitRectToDisplays(saved, displays, 0, MIN_VISIBLE), saved);
}

TEST(WindowGeometryTest, SizeIsClampedToTheDisplayHoldingMostOfTheWindow)
{
    // Reachable on both monitors; most of it is on the small one, so that one bounds its size.
    const std::array displays{Rect{.x = 0, .y = 0, .width = 2560, .height = 1400}, Rect{.x = 2560, .y = 0, .width = 1280, .height = 1000}};
    const Rect saved{.x = 2400, .y = 50, .width = 1600, .height = 1200};
    const Rect fitted = fitRectToDisplays(saved, displays, 0, MIN_VISIBLE);
    EXPECT_EQ(fitted, (Rect{.x = 2400, .y = 50, .width = 1280, .height = 1000}));
}

TEST(WindowGeometryTest, FallsBackToThePrimaryDisplayNotTheFirst)
{
    const std::array displays{Rect{.x = -1280, .y = 0, .width = 1280, .height = 1024}, Rect{.x = 0, .y = 0, .width = 1920, .height = 1080}};
    const Rect saved{.x = 10'000, .y = 10'000, .width = 800, .height = 600};
    EXPECT_EQ(fitRectToDisplays(saved, displays, 1, MIN_VISIBLE), (Rect{.x = 560, .y = 240, .width = 800, .height = 600}));
}

TEST(WindowGeometryTest, AnOutOfRangePrimaryIndexFallsBackToTheFirstDisplay)
{
    const std::array displays{Rect{.x = 0, .y = 0, .width = 1920, .height = 1080}};
    const Rect saved{.x = 10'000, .y = 10'000, .width = 800, .height = 600};
    EXPECT_EQ(fitRectToDisplays(saved, displays, 7, MIN_VISIBLE), (Rect{.x = 560, .y = 240, .width = 800, .height = 600}));
}

TEST(WindowGeometryTest, CentresWithinTheUsableBoundsNotTheFullDisplay)
{
    // Usable bounds exclude a 40px top panel: the window is centred below it.
    const std::array displays{Rect{.x = 0, .y = 40, .width = 1920, .height = 1040}};
    const Rect saved{.x = 0, .y = -500, .width = 1000, .height = 600};
    EXPECT_EQ(fitRectToDisplays(saved, displays, 0, MIN_VISIBLE), (Rect{.x = 460, .y = 260, .width = 1000, .height = 600}));
}

TEST(WindowGeometryTest, NoKnownDisplayLeavesTheRectangleUnchanged)
{
    const Rect saved{.x = -5000, .y = -5000, .width = 800, .height = 600};
    EXPECT_EQ(fitRectToDisplays(saved, std::span<const Rect>{}, 0, MIN_VISIBLE), saved);
}

TEST(WindowGeometryTest, FitIsUsableInConstantExpressions)
{
    constexpr std::array displays{Rect{.x = 0, .y = 0, .width = 1920, .height = 1080}};
    constexpr Rect fitted = fitRectToDisplays(Rect{.x = -3000, .y = 0, .width = 1920, .height = 1080}, displays, 0, MIN_VISIBLE);
    static_assert(fitted == Rect{.x = 0, .y = 0, .width = 1920, .height = 1080});
    EXPECT_EQ(fitted.x, 0);
}

} // namespace
} // namespace Core::WindowGeometry
