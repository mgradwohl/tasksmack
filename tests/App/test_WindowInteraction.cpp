/// @file test_WindowInteraction.cpp
/// @brief The custom title bar's drag and resize sessions (#1547), without SDL: a drag follows the
/// pointer and moves only on a change; a maximized window is restored only past the drag threshold,
/// then rebased so the pointer keeps its place across it; a resize moves on every sample but commits
/// the size at most every SIZE_COMMIT_INTERVAL_SECONDS, holds the latest back and drops it when the
/// pointer returns, and raises a resize event only for a new pixel size, at most every
/// RESIZE_EVENT_INTERVAL_SECONDS.

#include "App/TitleBarGeometry.h"
#include "App/WindowInteraction.h"

#include <gtest/gtest.h>

#include <optional>
#include <utility>

namespace App
{
namespace
{

using DragKind = DragTracker::Step::Kind;

// ---- Drag -------------------------------------------------------------------------------------

TEST(DragTrackerTest, FollowsThePointerFromWhereTheWindowStarted)
{
    DragTracker drag;
    drag.begin(500, 20, 100, 50, false, 0);
    EXPECT_FALSE(drag.pendingRestore());

    const DragTracker::Step step = drag.step(530, 45);
    EXPECT_EQ(step.kind, DragKind::Move);
    EXPECT_EQ(step.x, 130);
    EXPECT_EQ(step.y, 75);
}

TEST(DragTrackerTest, MovesOnlyWhenThePositionChanges)
{
    DragTracker drag;
    drag.begin(500, 20, 100, 50, false, 0);
    EXPECT_EQ(drag.step(500, 20).kind, DragKind::None); // not moved from the press
    EXPECT_EQ(drag.step(510, 20).kind, DragKind::Move);
    EXPECT_EQ(drag.step(510, 20).kind, DragKind::None); // the same sample again
    // Back to the start is a move like any other: it is not where the window was last put.
    const DragTracker::Step back = drag.step(500, 20);
    EXPECT_EQ(back.kind, DragKind::Move);
    EXPECT_EQ(back.x, 100);
}

TEST(DragTrackerTest, AMaximizedWindowIsRestoredOnlyPastTheThreshold)
{
    constexpr int T = DragTracker::DRAG_THRESHOLD_PX;
    DragTracker drag;
    drag.begin(960, 10, 0, 0, true, 1920);
    EXPECT_TRUE(drag.pendingRestore());

    // Anywhere strictly inside the threshold square is still a click.
    EXPECT_EQ(drag.step(960 + T - 1, 10 - (T - 1)).kind, DragKind::None);
    EXPECT_EQ(drag.step(960 - (T - 1), 10 + T - 1).kind, DragKind::None);
    EXPECT_TRUE(drag.pendingRestore());

    // Reaching it on either axis restores.
    EXPECT_EQ(drag.step(960, 10 + T).kind, DragKind::Restore);
    EXPECT_TRUE(drag.pendingRestore()); // until the layer has rebased it
}

TEST(DragTrackerTest, TheRestoredWindowKeepsThePointerInProportionAndTheDragGoesOn)
{
    DragTracker drag;
    // Pressed three quarters of the way across a 1920-wide maximized window at x = 0.
    drag.begin(1440, 10, 0, 0, true, 1920);
    ASSERT_EQ(drag.step(1450, 10).kind, DragKind::Restore);

    // Restored to 800 wide: three quarters across is 600 px in, so for the press its left edge is at
    // 840, and with the pointer now 10 px right of the press, at 850. The press was 10 px down the
    // title bar, so the top is 10 px above the pointer, whatever the window's saved restore Y (#2715).
    const auto [x, y] = drag.rebaseAfterRestore(800, 1450, 10);
    EXPECT_EQ(x, computeRestoreFromMaximizedDragX(1440, 0, 1920, 800) + 10);
    EXPECT_EQ(x, 850);
    EXPECT_EQ(y, 0);
    EXPECT_FALSE(drag.pendingRestore());

    // The same pointer position again is not a move.
    EXPECT_EQ(drag.step(1450, 10).kind, DragKind::None);

    // From then on the window follows the pointer, still measured from the original press.
    const DragTracker::Step step = drag.step(1460, 30);
    EXPECT_EQ(step.kind, DragKind::Move);
    EXPECT_EQ(step.x, 860);
    EXPECT_EQ(step.y, 20);
}

TEST(DragTrackerTest, TheRestoredWindowKeepsThePointerAsFarDownTheTitleBarAsThePress)
{
    // A maximized window at (0, 40), say below a top taskbar, pressed 28 px down its title bar and
    // dragged 200 px straight down: the restored top is 28 px above the pointer.
    DragTracker drag;
    drag.begin(960, 68, 0, 40, true, 1920);
    ASSERT_EQ(drag.step(960, 268).kind, DragKind::Restore);
    const auto [x, y] = drag.rebaseAfterRestore(1000, 960, 268);
    EXPECT_EQ(x, 460);
    EXPECT_EQ(y, 240);

    // The drag goes on with the pointer at that spot.
    const DragTracker::Step step = drag.step(970, 300);
    EXPECT_EQ(step.kind, DragKind::Move);
    EXPECT_EQ(step.x, 470);
    EXPECT_EQ(step.y, 272);
}

TEST(DragTrackerTest, TheRestoredWindowsTopStaysInTheWorkArea)
{
    // Pressed 20 px down the title bar of a window maximized at y = 40, then moved up 10 px and
    // sideways past the threshold: the top would be at 30, above the work area, so it is held at 40.
    DragTracker drag;
    drag.begin(960, 60, 0, 40, true, 1920);
    ASSERT_EQ(drag.step(1000, 50).kind, DragKind::Restore);
    const auto [x, y] = drag.rebaseAfterRestore(1000, 1000, 50);
    EXPECT_EQ(x, 500);
    EXPECT_EQ(y, 40);

    // Moving down from there moves the window down with the pointer, at the clamped offset.
    const DragTracker::Step step = drag.step(1000, 70);
    EXPECT_EQ(step.kind, DragKind::Move);
    EXPECT_EQ(step.y, 60);
}

TEST(DragTrackerTest, BeginStartsAFreshDrag)
{
    DragTracker drag;
    drag.begin(960, 10, 0, 0, true, 1920);
    drag.begin(100, 10, 50, 50, false, 0);
    EXPECT_FALSE(drag.pendingRestore());
    EXPECT_EQ(drag.step(110, 10).kind, DragKind::Move);
}

// ---- Resize -----------------------------------------------------------------------------------

constexpr WindowRect START{.x = 100, .y = 100, .width = 800, .height = 600};
constexpr int MIN_W = 400;
constexpr int MIN_H = 300;
constexpr double LATER = ResizeTracker::SIZE_COMMIT_INTERVAL_SECONDS;

TEST(ResizeTrackerTest, TheRightEdgeCommitsTheNewWidthOnceTheIntervalHasPassed)
{
    ResizeTracker resize;
    resize.begin(ResizeEdge::Right, 900, 400, START, 10.0);
    EXPECT_EQ(resize.edge(), ResizeEdge::Right);

    const ResizeTracker::Step step = resize.step(950, 400, MIN_W, MIN_H, 10.0 + LATER);
    EXPECT_FALSE(step.move); // the right edge leaves the origin where it is
    EXPECT_TRUE(step.commitSize);
    EXPECT_EQ(step.width, 850);
    EXPECT_EQ(step.height, 600);
    EXPECT_FALSE(resize.pendingSize().has_value());
}

TEST(ResizeTrackerTest, TheLeftEdgeMovesEverySampleButHoldsTheSizeBackWithinTheInterval)
{
    ResizeTracker resize;
    resize.begin(ResizeEdge::Left, 100, 400, START, 10.0);

    // Too soon after the press for a size commit: the origin moves now, the size waits.
    const ResizeTracker::Step first = resize.step(80, 400, MIN_W, MIN_H, 10.01);
    EXPECT_TRUE(first.move);
    EXPECT_EQ(first.x, 80);
    EXPECT_EQ(first.y, 100);
    EXPECT_FALSE(first.commitSize);
    ASSERT_TRUE(resize.pendingSize().has_value());
    EXPECT_EQ(resize.pendingSize(), std::optional(std::pair(820, 600)));

    // The latest size replaces the held one.
    const ResizeTracker::Step second = resize.step(70, 400, MIN_W, MIN_H, 10.02);
    EXPECT_TRUE(second.move);
    EXPECT_FALSE(second.commitSize);
    EXPECT_EQ(resize.pendingSize(), std::optional(std::pair(830, 600)));

    // Once the interval has passed it is committed, and nothing is held any more.
    const ResizeTracker::Step third = resize.step(70, 400, MIN_W, MIN_H, 10.0 + LATER);
    EXPECT_FALSE(third.move); // same origin as the last sample
    EXPECT_TRUE(third.commitSize);
    EXPECT_EQ(third.width, 830);
    EXPECT_FALSE(resize.pendingSize().has_value());

    // And the next commit waits a whole interval from that one.
    EXPECT_FALSE(resize.step(60, 400, MIN_W, MIN_H, 10.0 + LATER + 0.01).commitSize);
    EXPECT_TRUE(resize.step(60, 400, MIN_W, MIN_H, 10.0 + (2.5 * LATER)).commitSize);
}

TEST(ResizeTrackerTest, AHeldSizeIsDroppedWhenThePointerReturnsToTheCommittedSize)
{
    ResizeTracker resize;
    resize.begin(ResizeEdge::Bottom, 400, 700, START, 10.0);
    EXPECT_FALSE(resize.step(400, 750, MIN_W, MIN_H, 10.01).commitSize);
    ASSERT_TRUE(resize.pendingSize().has_value());

    // Dragged back to where it started: committing the held size would jump the window to a size the
    // pointer has left.
    const ResizeTracker::Step back = resize.step(400, 700, MIN_W, MIN_H, 10.02);
    EXPECT_FALSE(back.move);
    EXPECT_FALSE(back.commitSize);
    EXPECT_FALSE(resize.pendingSize().has_value());
}

TEST(ResizeTrackerTest, TheSizeStopsAtTheMinimum)
{
    ResizeTracker resize;
    resize.begin(ResizeEdge::TopLeft, 100, 100, START, 10.0);
    // Far past the opposite corner: the window stops at its minimum, its far corner fixed.
    const ResizeTracker::Step step = resize.step(2000, 2000, MIN_W, MIN_H, 10.0 + LATER);
    EXPECT_TRUE(step.commitSize);
    EXPECT_EQ(step.width, MIN_W);
    EXPECT_EQ(step.height, MIN_H);
    EXPECT_TRUE(step.move);
    EXPECT_EQ(step.x, START.x + START.width - MIN_W);
    EXPECT_EQ(step.y, START.y + START.height - MIN_H);
}

TEST(ResizeTrackerTest, AResizeEventOnlyForANewPixelSizeAndNotTooOften)
{
    constexpr double EVENT = ResizeTracker::RESIZE_EVENT_INTERVAL_SECONDS;
    ResizeTracker resize;
    resize.begin(ResizeEdge::Right, 900, 400, START, 10.0);

    EXPECT_TRUE(resize.takeResizeEvent(1700, 1200, 10.0 + EVENT));
    // The same pixel size again: nothing new to tell.
    EXPECT_FALSE(resize.takeResizeEvent(1700, 1200, 10.0 + (3.0 * EVENT)));
    // A new size, but too soon after the last event.
    EXPECT_FALSE(resize.takeResizeEvent(1720, 1200, 10.0 + (1.5 * EVENT)));
    EXPECT_TRUE(resize.takeResizeEvent(1720, 1200, 10.0 + (2.0 * EVENT)));
}

TEST(ResizeTrackerTest, BeginStartsAFreshResize)
{
    ResizeTracker resize;
    resize.begin(ResizeEdge::Left, 100, 400, START, 10.0);
    static_cast<void>(resize.step(80, 400, MIN_W, MIN_H, 10.01));
    ASSERT_TRUE(resize.pendingSize().has_value());
    ASSERT_TRUE(resize.takeResizeEvent(1600, 1200, 11.0));

    resize.begin(ResizeEdge::Bottom, 400, 700, START, 20.0);
    EXPECT_EQ(resize.edge(), ResizeEdge::Bottom);
    EXPECT_FALSE(resize.pendingSize().has_value());
    EXPECT_TRUE(resize.takeResizeEvent(1600, 1200, 21.0)); // the last session's size is forgotten

    // A reset tracker (as after the drag ends) has no edge.
    resize = {};
    EXPECT_EQ(resize.edge(), ResizeEdge::None);
}

} // namespace
} // namespace App
