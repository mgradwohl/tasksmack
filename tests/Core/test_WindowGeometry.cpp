/// @file test_WindowGeometry.cpp
/// @brief Tests for the pure window-geometry decisions in Core/WindowGeometry.h: which rectangle is
/// persisted as the window's normal geometry (#1121), and how a restored rectangle is fitted to the
/// connected displays (#1128), when an OS maximize is replaced by the client-side one (#1208), and how
/// the normal rectangle is tracked through maximizes and restores from any source (#1250).

#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>

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

// ---- shouldAdoptSystemMaximize (#1208) ----

TEST(WindowGeometryTest, AnOsMaximizeOfTheBorderlessWindowIsAdoptedOnClientSideBackends)
{
    // Win+Up / ShowWindow(SW_MAXIMIZE) on Windows: the OS sized the window from the primary screen,
    // a quarter of a 175 % display, so it is replaced by the client-side maximize.
    EXPECT_TRUE(shouldAdoptSystemMaximize(true, true, true, true, false));
}

TEST(WindowGeometryTest, AnOsMaximizeIsLeftAloneWhereTheOsGetsItRight)
{
    EXPECT_FALSE(shouldAdoptSystemMaximize(true, false, true, true, false)); // native Wayland compositor maximize
    EXPECT_FALSE(shouldAdoptSystemMaximize(false, true, true, true, false)); // a window with an OS frame
    EXPECT_FALSE(shouldAdoptSystemMaximize(false, false, true, true, false));
}

TEST(WindowGeometryTest, TheSdlMaximizeFallbackIsNotAdoptedAgain)
{
    // Without the display's usable bounds maximize() itself falls back to SDL_MaximizeWindow();
    // adopting that MAXIMIZED event would restore and re-maximize on every event.
    EXPECT_FALSE(shouldAdoptSystemMaximize(true, true, false, true, false));
}

TEST(WindowGeometryTest, AStaleOsMaximizeNotificationIsNotAdopted)
{
    // SDL events are queued: the MAXIMIZED notification can be drained after a later OS restore or
    // minimize already changed the window. Adopting it then would undo that newer action (#1208).
    EXPECT_FALSE(shouldAdoptSystemMaximize(true, true, true, false, false)); // restored since
    EXPECT_FALSE(shouldAdoptSystemMaximize(true, true, true, true, true));   // minimized since
    EXPECT_FALSE(shouldAdoptSystemMaximize(true, true, true, false, true));
    EXPECT_TRUE(shouldAdoptSystemMaximize(true, true, true, true, false)); // still maximized
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

TEST(WindowGeometryTest, ShrinkingAWindowReachableOnlyByItsFarEdgeMovesItOnScreen)
{
    // #1253 review: 64 px of this 3000-wide window are on the 1920-wide display. Shrinking it to the
    // display's width while keeping x = -2936 would leave it entirely off-screen, so it is moved just
    // far enough to lie on the display (x clamped to 0); y was already fine and is kept.
    const std::array displays{Rect{.x = 0, .y = 0, .width = 1920, .height = 1080}};
    const Rect saved{.x = -2936, .y = 100, .width = 3000, .height = 720};
    const Rect fitted = fitRectToDisplays(saved, displays, 0, MIN_VISIBLE);
    EXPECT_EQ(fitted, (Rect{.x = 0, .y = 100, .width = 1920, .height = 720}));
    EXPECT_TRUE(isReachableOn(fitted, displays[0], MIN_VISIBLE));
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

// ---- rescaleWindowSize / targetDisplayIndex (#1168) ----

TEST(WindowGeometryTest, SizeSavedAtOneScaleKeepsItsApparentSizeAtAnother)
{
    // The default 1280 x 720 is for 100 %: at 200 % on Windows (pixel units) it is twice the pixels.
    EXPECT_EQ(rescaleWindowSize(1280, 720, 1.0F, 2.0F), (std::pair{2560, 1440}));
    // Saved on a 200 % display, reopened on a 100 % one: half the pixels, not double the size.
    EXPECT_EQ(rescaleWindowSize(2560, 1440, 2.0F, 1.0F), (std::pair{1280, 720}));
    // 175 % to 125 %, rounded to the nearest pixel.
    EXPECT_EQ(rescaleWindowSize(1001, 700, 1.75F, 1.25F), (std::pair{715, 500}));
    // Same scale: unchanged.
    EXPECT_EQ(rescaleWindowSize(1234, 567, 1.5F, 1.5F), (std::pair{1234, 567}));
}

TEST(WindowGeometryTest, SizeWithoutASavedScaleIsRestoredAsSaved)
{
    // A config written before the scale was saved: the size is restored as it always was.
    EXPECT_EQ(rescaleWindowSize(1920, 1080, std::nullopt, 2.0F), (std::pair{1920, 1080}));
}

TEST(WindowGeometryTest, UnusableScalesLeaveTheSizeAlone)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_EQ(rescaleWindowSize(1280, 720, 0.0F, 2.0F), (std::pair{1280, 720}));
    EXPECT_EQ(rescaleWindowSize(1280, 720, -1.0F, 2.0F), (std::pair{1280, 720}));
    EXPECT_EQ(rescaleWindowSize(1280, 720, nan, 2.0F), (std::pair{1280, 720}));
    EXPECT_EQ(rescaleWindowSize(1280, 720, 1.0F, 0.0F), (std::pair{1280, 720}));
    EXPECT_EQ(rescaleWindowSize(1280, 720, 1.0F, nan), (std::pair{1280, 720}));
    EXPECT_EQ(rescaleWindowSize(1280, 720, inf, 1.0F), (std::pair{1280, 720}));
    EXPECT_EQ(rescaleWindowSize(1280, 720, 1.0F, MAX_WINDOW_SCALE * 2.0F), (std::pair{1280, 720}));
}

TEST(WindowGeometryTest, RescaledSizeStaysWithinTheWindowLimits)
{
    EXPECT_EQ(rescaleWindowSize(300, 250, 4.0F, 1.0F), (std::pair{WINDOW_MIN_DIMENSION, WINDOW_MIN_DIMENSION}));
    EXPECT_EQ(rescaleWindowSize(10'000, 9'000, 1.0F, 4.0F), (std::pair{WINDOW_MAX_DIMENSION, WINDOW_MAX_DIMENSION}));
}

TEST(WindowGeometryTest, RescaleIsUsableInConstantExpressions)
{
    static_assert(rescaleWindowSize(1280, 720, 1.0F, 1.5F) == std::pair{1920, 1080});
    static_assert(isUsableWindowScale(1.0F) && !isUsableWindowScale(0.0F) && !isUsableWindowScale(MAX_WINDOW_SCALE + 1.0F));
    SUCCEED();
}

TEST(WindowGeometryTest, TargetDisplayIsTheOneTheFitUses)
{
    const std::array displays{
        Rect{.x = 0, .y = 0, .width = 1920, .height = 1040},    // primary, 100 %
        Rect{.x = 1920, .y = 0, .width = 3840, .height = 2100}, // 200 %
    };
    // On the second display: that one, whatever the primary is.
    EXPECT_EQ(targetDisplayIndex(Rect{.x = 2500, .y = 100, .width = 1280, .height = 720}, displays, 0, MIN_VISIBLE),
              std::optional<std::size_t>{1});
    // Off every display: the primary, where fitRectToDisplays() centres it.
    EXPECT_EQ(targetDisplayIndex(Rect{.x = -9000, .y = -9000, .width = 800, .height = 600}, displays, 1, MIN_VISIBLE),
              std::optional<std::size_t>{1});
    EXPECT_EQ(targetDisplayIndex(Rect{.x = -9000, .y = -9000, .width = 800, .height = 600}, displays, 7, MIN_VISIBLE),
              std::optional<std::size_t>{0});
    EXPECT_EQ(targetDisplayIndex(Rect{}, std::span<const Rect>{}, 0, MIN_VISIBLE), std::nullopt);
}

TEST(WindowGeometryTest, WindowUnitScaleIsDisplayScaleOverPixelDensity)
{
    EXPECT_FLOAT_EQ(windowUnitScale(1.75F, 1.0F), 1.75F); // Windows at 175 %: pixel units
    EXPECT_FLOAT_EQ(windowUnitScale(2.0F, 2.0F), 1.0F);   // Wayland at 200 %: logical units
    EXPECT_FLOAT_EQ(windowUnitScale(1.5F, 0.0F), 1.5F);   // unknown density: the display scale
}

// #1297 review: a maximized window saves its restore rectangle, so it must be tagged with the scale
// captured with that rectangle -- not the live scale of the maximized window, which can belong to
// another display or a later scale setting.
TEST(WindowGeometryTest, NormalGeometryScaleFollowsTheRectangleItTags)
{
    EXPECT_FLOAT_EQ(selectNormalGeometryScale(false, true, 1.0F, 1.75F), 1.75F); // Live rectangle, live scale
    EXPECT_FLOAT_EQ(selectNormalGeometryScale(true, true, 1.0F, 1.75F), 1.0F);   // Restore rectangle, its scale
    EXPECT_FLOAT_EQ(selectNormalGeometryScale(true, false, 1.0F, 1.75F), 0.0F);  // No rectangle saved: no scale
    EXPECT_FLOAT_EQ(selectNormalGeometryScale(true, true, 0.0F, 1.75F), 0.0F);   // Captured without a scale
}

// ---- NormalGeometryTracker (#1250) ----

constexpr Rect NORMAL_A{.x = 100, .y = 80, .width = 1280, .height = 720};
constexpr Rect NORMAL_B{.x = 300, .y = 200, .width = 900, .height = 600};
constexpr Rect MAXIMIZED{.x = 0, .y = 0, .width = 2560, .height = 1400};

/// What Window::getNormalGeometry() would save for a window in @p tracker's state at @p live.
std::optional<Rect> savedNormal(const NormalGeometryTracker& tracker, bool maximizedNow, const Rect& live)
{
    return selectNormalGeometry(maximizedNow, live, tracker.restoreTarget());
}

TEST(NormalGeometryTrackerTest, AnOutsideMaximizeSavesTheLastNormalGeometry)
{
    // The #1250 scenario: the window manager maximizes the window (not the title-bar button). On exit
    // the saved geometry must be the normal rectangle plus maximized = true, not the maximized one.
    NormalGeometryTracker tracker;
    tracker.observe(NORMAL_A, 1.5F, true);
    // The window manager's resize to the maximized size is drained with SDL_WINDOW_MAXIMIZED set.
    tracker.observe(MAXIMIZED, 1.5F, false);
    tracker.systemMaximized(true, false);

    EXPECT_TRUE(tracker.isMaximized());
    EXPECT_EQ(tracker.state(), MaximizeState::System);
    EXPECT_EQ(savedNormal(tracker, true, MAXIMIZED), NORMAL_A);
    EXPECT_FLOAT_EQ(selectNormalGeometryScale(true, tracker.restoreTarget().has_value(), tracker.restoreScale(), 2.0F), 1.5F);
}

TEST(NormalGeometryTrackerTest, AnOutsideMaximizeBeforeAnyNormalGeometryKeepsTheSavedOne)
{
    // Nothing seen at a normal size: no restore target, so the caller keeps its saved geometry
    // rather than saving the maximized rectangle.
    NormalGeometryTracker tracker;
    tracker.systemMaximized(true, false);
    EXPECT_TRUE(tracker.isMaximized());
    EXPECT_FALSE(tracker.restoreTarget().has_value());
    EXPECT_EQ(savedNormal(tracker, true, MAXIMIZED), std::nullopt);
}

TEST(NormalGeometryTrackerTest, AStaleMaximizedEventChangesNothing)
{
    // A queued MAXIMIZED drained after a later restore or minimize: the live flags decide.
    NormalGeometryTracker tracker;
    tracker.observe(NORMAL_A, 1.0F, true);
    tracker.systemMaximized(false, false);
    EXPECT_FALSE(tracker.isMaximized());
    tracker.systemMaximized(true, true);
    EXPECT_FALSE(tracker.isMaximized());
}

TEST(NormalGeometryTrackerTest, CompositorRestoreThenResizeThenMaximizeSavesTheNewSize)
{
    // From the #1253 review (comment on #1250): the app maximizes at A, the compositor restores, the
    // user resizes to B, the compositor maximizes, the user quits. The normal rectangle saved must be
    // B, not the stale A.
    NormalGeometryTracker tracker;
    tracker.maximizing(NORMAL_A, 1.0F, true, MaximizeState::System); // Window::maximize() on Wayland
    EXPECT_EQ(tracker.restoreTarget(), NORMAL_A);

    tracker.systemRestored(false, false); // The compositor restores
    EXPECT_FALSE(tracker.isMaximized());
    EXPECT_FALSE(tracker.restoreTarget().has_value());

    tracker.observe(NORMAL_B, 1.0F, true); // The user resizes
    tracker.systemMaximized(true, false);  // The compositor maximizes
    EXPECT_EQ(savedNormal(tracker, true, MAXIMIZED), NORMAL_B);
}

TEST(NormalGeometryTrackerTest, TheAppsOwnMaximizeRecordsTheRectangleItLeaves)
{
    NormalGeometryTracker tracker;
    tracker.observe(NORMAL_B, 1.0F, true);
    // The window is at A when maximize() runs (a move not yet observed): A is what it leaves.
    tracker.maximizing(NORMAL_A, 1.25F, true, MaximizeState::ClientSide);
    EXPECT_EQ(tracker.state(), MaximizeState::ClientSide);
    EXPECT_EQ(tracker.restoreTarget(), NORMAL_A);
    EXPECT_FLOAT_EQ(tracker.restoreScale(), 1.25F);
}

TEST(NormalGeometryTrackerTest, ASecondMaximizeKeepsTheFirstRestoreTarget)
{
    NormalGeometryTracker tracker;
    tracker.maximizing(NORMAL_A, 1.0F, true, MaximizeState::ClientSide);
    tracker.maximizing(MAXIMIZED, 1.0F, false, MaximizeState::ClientSide);
    EXPECT_EQ(tracker.restoreTarget(), NORMAL_A);

    // Nor does an outside maximize reported while already maximized.
    tracker.systemMaximized(true, false);
    EXPECT_EQ(tracker.restoreTarget(), NORMAL_A);
    EXPECT_EQ(tracker.state(), MaximizeState::ClientSide);
}

TEST(NormalGeometryTrackerTest, MaximizeOfAWindowAlreadyMaximizedFromOutsideUsesTheLastNormalGeometry)
{
    // maximize() finds the window maximized although the compositor's MAXIMIZED is still queued: the
    // live rectangle is the maximized one, so the last normal geometry is the restore target.
    NormalGeometryTracker tracker;
    tracker.observe(NORMAL_B, 1.0F, true);
    tracker.maximizing(MAXIMIZED, 1.0F, false, MaximizeState::System);
    EXPECT_EQ(tracker.restoreTarget(), NORMAL_B);
}

TEST(NormalGeometryTrackerTest, AnOsRestoreDoesNotEndAClientSideMaximize)
{
    // SDL sends RESTORED when a minimized window comes back and when adoptSystemMaximize() undoes an
    // OS maximize on its way to the client-side one; neither ends a client-side maximize, which no
    // OS flag shows.
    NormalGeometryTracker tracker;
    tracker.maximizing(NORMAL_A, 1.0F, true, MaximizeState::ClientSide);
    tracker.systemRestored(false, false);
    EXPECT_TRUE(tracker.isMaximized());
    EXPECT_EQ(tracker.restoreTarget(), NORMAL_A);
}

TEST(NormalGeometryTrackerTest, AStaleOrMinimizedRestoreDoesNotEndAnOutsideMaximize)
{
    NormalGeometryTracker tracker;
    tracker.observe(NORMAL_A, 1.0F, true);
    tracker.systemMaximized(true, false);

    tracker.systemRestored(true, false); // Un-minimized, still maximized; or re-maximized since
    EXPECT_TRUE(tracker.isMaximized());
    tracker.systemRestored(false, true); // Minimized now: cannot tell
    EXPECT_TRUE(tracker.isMaximized());
    EXPECT_EQ(tracker.restoreTarget(), NORMAL_A);
}

TEST(NormalGeometryTrackerTest, GeometryWhileNotNormalIsNotRecorded)
{
    NormalGeometryTracker tracker;
    tracker.observe(NORMAL_A, 1.0F, true);
    tracker.observe(MAXIMIZED, 1.0F, false);                                    // Maximized, minimized or fullscreen
    tracker.observe(Rect{.x = 0, .y = 0, .width = 0, .height = 0}, 1.0F, true); // Not a real size
    tracker.systemMaximized(true, false);
    EXPECT_EQ(tracker.restoreTarget(), NORMAL_A);
}

TEST(NormalGeometryTrackerTest, AClientSideMaximizeIgnoresGeometryEvenWhenSdlFlagsLookNormal)
{
    // A client-side maximize sets no SDL flag; its own resize to the usable bounds must not become the
    // last normal geometry.
    NormalGeometryTracker tracker;
    tracker.maximizing(NORMAL_A, 1.0F, true, MaximizeState::ClientSide);
    tracker.observe(MAXIMIZED, 1.0F, true);
    tracker.restored();
    tracker.systemMaximized(true, false);
    EXPECT_EQ(tracker.restoreTarget(), NORMAL_A);
}

TEST(NormalGeometryTrackerTest, RestoreSpendsTheRestoreTarget)
{
    NormalGeometryTracker tracker;
    tracker.maximizing(NORMAL_A, 1.5F, true, MaximizeState::ClientSide);
    tracker.restored();
    EXPECT_EQ(tracker.state(), MaximizeState::Normal);
    EXPECT_FALSE(tracker.restoreTarget().has_value());
    EXPECT_FLOAT_EQ(tracker.restoreScale(), 0.0F);
    // Not maximized: the live geometry is saved.
    EXPECT_EQ(savedNormal(tracker, false, NORMAL_B), NORMAL_B);
}

} // namespace
} // namespace Core::WindowGeometry
