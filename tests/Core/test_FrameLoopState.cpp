/// @file test_FrameLoopState.cpp
/// @brief Application::run()'s loop phases, each on its own (#3791): the move/resize frame and the
/// resize present hold (#882), the iteration's wait (a hidden window's, a held present's, a paced
/// frame's, the idle wait), each cut short for a failed GPU context replacement's retry (#3484), and the
/// regular frame. A fake IFrameLoopHost stands in for the clock, SDL and the layers, so no window opens.

#include "Core/FrameLoopState.h"
#include "Core/FramePacing.h"

#include <SDL3/SDL_events.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace
{

using Core::FrameLoopState;
using Core::FramePacing::IdleWaitOutcome;

constexpr double START = 100.0;
constexpr double DISPLAY_HZ = 60.0;
// Exactly representable, so the hold's remaining milliseconds are exact: 62.5 ms interval, 15.625 ms
// since the last frame, 46.875 ms left (rounded up to 47).
constexpr double MIN_INTERVAL = 0.0625;
constexpr double SINCE_LAST_FRAME = 0.015625;
constexpr int HOLD_MS = 47;

/// The clock, SDL and the layers, as the phases see them. Every call is logged in order; an event wait
/// that times out and a paced delay move the clock on by their length.
class FakeLoopHost final : public Core::IFrameLoopHost
{
  public:
    double clock = START;
    std::vector<std::string> calls;
    std::vector<std::uint64_t> delaysNs;
    std::vector<int> waitTimeoutsMs;
    /// The next event wait's outcome; any but TimedOut leaves a key-down event.
    IdleWaitOutcome waitOutcome = IdleWaitOutcome::TimedOut;
    /// What boundWaitMs() caps a wait at: the next retry of a failed context replacement (#3484).
    int recoveryBoundMs = std::numeric_limits<int>::max();
    /// False while drawing is suspended after a GPU reset (#3484): renderFrame() draws nothing.
    bool drawing = true;
    /// The frame start time each renderFrame() saw on the clock.
    std::vector<double> renderTimes;

    [[nodiscard]] double now() override
    {
        return clock;
    }

    void delayPreciseNs(const std::uint64_t nanoseconds) override
    {
        calls.emplace_back("delay");
        delaysNs.push_back(nanoseconds);
        clock += static_cast<double>(nanoseconds) / 1.0e9;
    }

    bool renderFrame(const bool resizeTriggeredFrame, Core::FrameTimes* times) override
    {
        calls.emplace_back(resizeTriggeredFrame ? "interactionFrame" : "regularFrame");
        renderTimes.push_back(clock);
        if (times != nullptr && drawing)
        {
            *times = {.updateMs = 1.0, .renderMs = 2.0, .postRenderMs = 3.0, .swapMs = 4.0};
        }
        return drawing;
    }

    IdleWaitOutcome waitForEvent(SDL_Event& event, const int timeoutMs) override
    {
        calls.emplace_back("wait");
        waitTimeoutsMs.push_back(timeoutMs);
        if (waitOutcome == IdleWaitOutcome::TimedOut)
        {
            clock += timeoutMs / 1000.0;
        }
        else
        {
            event = SDL_Event{};
            event.type = SDL_EVENT_KEY_DOWN;
        }
        return waitOutcome;
    }

    [[nodiscard]] int boundWaitMs(const int waitMs, const double /*nowSeconds*/) override
    {
        calls.emplace_back("bound");
        return std::min(waitMs, recoveryBoundMs);
    }

    void hiddenUpdate() override
    {
        calls.emplace_back("hiddenUpdate");
    }
};

/// A loop on Windows (the resize present bound applies) whose last frame started SINCE_LAST_FRAME ago.
FrameLoopState makeLoop()
{
    FrameLoopState loop;
    loop.lastFrameStart = START - SINCE_LAST_FRAME;
    loop.lastResizeTraceLogTime = START;
    loop.resizePresentMinInterval = MIN_INTERVAL;
    loop.drewFirstFrame = true;
    return loop;
}

FrameLoopState::InteractionFrameInput newSize()
{
    return {.needsResizeRedraw = true, .forceInteractionRedraw = true, .hadEvents = true, .displayRefreshHz = DISPLAY_HZ};
}

std::vector<std::string> calls(std::initializer_list<const char*> names)
{
    return {names.begin(), names.end()};
}

// ---- The move/resize frame and the resize present hold (#882) ----

TEST(FrameLoopStateTest, AMoveResizeFrameIsPacedAtTheDisplayRateThenDrawn)
{
    // A move: no new size, so no hold, but the frame still waits for the display rate (#1153).
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    const auto result = loop.interactionFrame(host, {.forceInteractionRedraw = true, .hadEvents = true, .displayRefreshHz = DISPLAY_HZ});

    EXPECT_FALSE(result.holdPresent);
    EXPECT_FALSE(result.presentOwedFrame);
    EXPECT_TRUE(result.didImmediateResizeRedraw);
    EXPECT_TRUE(result.drewFrame);
    EXPECT_EQ(host.calls, calls({"delay", "interactionFrame"}));
    ASSERT_EQ(host.delaysNs.size(), 1U);
    EXPECT_NEAR(static_cast<double>(host.delaysNs[0]) / 1.0e9, (1.0 / DISPLAY_HZ) - SINCE_LAST_FRAME, 1e-6);
    EXPECT_DOUBLE_EQ(loop.lastFrameStart, host.renderTimes.at(0)) << "the frame starts after the delay";
}

TEST(FrameLoopStateTest, AMoveResizeFrameDueAlreadyNeedsNoDelay)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.lastFrameStart = START - 1.0;
    const auto result = loop.interactionFrame(host, newSize());

    EXPECT_TRUE(result.drewFrame);
    EXPECT_EQ(host.calls, calls({"interactionFrame"}));
}

TEST(FrameLoopStateTest, ANewSizeSoonerThanTheIntervalIsHeldThenPresentedOnceTheHoldEnds)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.lastSizeChangeTime = START; // the drain just saw a new size

    // The iteration with the new size: held, owed, nothing drawn.
    const auto held = loop.interactionFrame(host, newSize());
    EXPECT_TRUE(held.holdPresent);
    EXPECT_FALSE(held.didImmediateResizeRedraw);
    EXPECT_FALSE(held.drewFrame);
    EXPECT_TRUE(loop.resizePresentOwed);
    EXPECT_TRUE(host.calls.empty());

    // Its wait lasts until the owed frame is due, and its regular frame is skipped.
    const auto waited = loop.waits(host,
                                   {
                                       .hadEvents = true,
                                       .holdPresent = held.holdPresent,
                                       .didImmediateResizeRedraw = held.didImmediateResizeRedraw,
                                       .displayRefreshHz = DISPLAY_HZ,
                                   });
    EXPECT_EQ(waited.wait, FrameLoopState::Wait::ResizeHold);
    EXPECT_FALSE(waited.idleWaitOutcome.has_value());
    EXPECT_EQ(host.waitTimeoutsMs, std::vector<int>{HOLD_MS});
    EXPECT_FALSE(loop.regularFrame(host, {.didImmediateResizeRedraw = held.didImmediateResizeRedraw, .holdPresent = held.holdPresent}));
    EXPECT_EQ(host.calls, calls({"bound", "wait"}));

    // The next iteration, with no event: the hold is over and the owed frame is drawn at once.
    host.calls.clear();
    const auto owed = loop.interactionFrame(host, {.forceInteractionRedraw = true, .hadEvents = false, .displayRefreshHz = DISPLAY_HZ});
    EXPECT_FALSE(owed.holdPresent);
    EXPECT_TRUE(owed.presentOwedFrame);
    EXPECT_TRUE(owed.drewFrame);
    EXPECT_FALSE(loop.resizePresentOwed);
    EXPECT_EQ(host.calls, calls({"interactionFrame"}));
}

TEST(FrameLoopStateTest, NoHoldWhereTheBoundDoesNotApply)
{
    // Linux: the compositor or window manager paces a resize itself (minimum interval 0).
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.resizePresentMinInterval = 0.0;
    loop.lastSizeChangeTime = START;
    const auto result = loop.interactionFrame(host, newSize());

    EXPECT_FALSE(result.holdPresent);
    EXPECT_TRUE(result.drewFrame);
    EXPECT_FALSE(loop.resizePresentOwed);
}

TEST(FrameLoopStateTest, AnOwedFrameIsPaidByTheAttemptWhileDrawingIsSuspended)
{
    // After a GPU reset (#3484) renderFrame() draws nothing; a frame kept owing would retry every iteration without waiting.
    FakeLoopHost host;
    host.drawing = false;
    FrameLoopState loop = makeLoop();
    loop.lastFrameStart = START - 1.0;
    loop.lastSizeChangeTime = START - 0.05;
    loop.resizePresentOwed = true;
    const auto result = loop.interactionFrame(host, {.forceInteractionRedraw = true, .displayRefreshHz = DISPLAY_HZ});

    EXPECT_TRUE(result.presentOwedFrame);
    EXPECT_TRUE(result.didImmediateResizeRedraw);
    EXPECT_FALSE(result.drewFrame);
    EXPECT_FALSE(loop.resizePresentOwed);
    EXPECT_EQ(host.calls, calls({"interactionFrame"}));
}

TEST(FrameLoopStateTest, AHiddenWindowThatDrawsNoFramesHoldsNothingAndOwesNothing)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.lastSizeChangeTime = START;
    loop.resizePresentOwed = true;
    auto input = newSize();
    input.drawsNoFrames = true;
    input.isHidden = true;
    const auto result = loop.interactionFrame(host, input);

    EXPECT_FALSE(result.holdPresent);
    EXPECT_FALSE(result.presentOwedFrame);
    EXPECT_FALSE(result.didImmediateResizeRedraw);
    EXPECT_FALSE(loop.resizePresentOwed);
    EXPECT_TRUE(host.calls.empty());
}

TEST(FrameLoopStateTest, AGraceIterationWithNothingNewDefersItsFrame)
{
    // #3511: no events and no client-side geometry change in the grace: no move/resize frame.
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    const auto result = loop.interactionFrame(host, {.forceInteractionRedraw = true, .traced = true, .displayRefreshHz = DISPLAY_HZ});

    EXPECT_FALSE(result.didImmediateResizeRedraw);
    EXPECT_TRUE(host.calls.empty());
    EXPECT_EQ(loop.resizeTraceStats.deferredInteractionFrames, 1U);

    // A client-side drag that moved the window last frame has something new to show.
    loop.geometryChangedLastFrame = true;
    EXPECT_TRUE(loop.interactionFrame(host, {.forceInteractionRedraw = true, .displayRefreshHz = DISPLAY_HZ}).drewFrame);
}

TEST(FrameLoopStateTest, ATracedHoldAndItsOwedFrameAreCounted)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.lastSizeChangeTime = START;
    auto input = newSize();
    input.traced = true;
    static_cast<void>(loop.interactionFrame(host, input));
    EXPECT_EQ(loop.resizeTraceStats.heldResizePresents, 1U);

    host.clock = START + MIN_INTERVAL;
    static_cast<void>(loop.interactionFrame(host, {.forceInteractionRedraw = true, .traced = true, .displayRefreshHz = DISPLAY_HZ}));
    EXPECT_EQ(loop.resizeTraceStats.owedResizePresents, 1U);
    EXPECT_EQ(loop.resizeTraceStats.frames, 1U);
    EXPECT_EQ(loop.resizeTraceStats.resizeFrames, 1U);
    EXPECT_DOUBLE_EQ(loop.loopTiming.frameMs, 10.0);
}

// ---- The iteration's wait ----

TEST(FrameLoopStateTest, AHiddenWindowWaitsThenRunsTheHiddenUpdate)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    const auto result = loop.waits(host, {.hiddenWait = {.timeoutMs = 250, .drawsNoFrames = true}, .isHidden = true});

    EXPECT_EQ(result.wait, FrameLoopState::Wait::Hidden);
    EXPECT_FALSE(result.idleWaitOutcome.has_value()) << "not the idle wait the test seam counts";
    EXPECT_EQ(host.waitTimeoutsMs, std::vector<int>{250});
    EXPECT_EQ(host.calls, calls({"bound", "wait", "hiddenUpdate"}));
}

TEST(FrameLoopStateTest, AHiddenWindowThatDrainedEventsDrainsAgainBeforeWaiting)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    const auto result = loop.waits(host, {.hiddenWait = {.timeoutMs = 250, .drawsNoFrames = true}, .hadEvents = true, .isHidden = true});

    EXPECT_EQ(result.wait, FrameLoopState::Wait::Hidden);
    EXPECT_EQ(host.calls, calls({"hiddenUpdate"}));
}

TEST(FrameLoopStateTest, EveryEventWaitEndsByTheNextContextReplacementRetry)
{
    // A failed context replacement retries in 7 ms (#3484): the hidden, the hold and the idle wait all end by then.
    {
        FakeLoopHost host;
        host.recoveryBoundMs = 7;
        FrameLoopState loop = makeLoop();
        static_cast<void>(loop.waits(host, {.hiddenWait = {.timeoutMs = 250, .drawsNoFrames = true}, .isHidden = true}));
        EXPECT_EQ(host.waitTimeoutsMs, std::vector<int>{7}) << "hidden";
    }
    {
        FakeLoopHost host;
        host.recoveryBoundMs = 7;
        FrameLoopState loop = makeLoop();
        static_cast<void>(loop.waits(host, {.holdPresent = true, .displayRefreshHz = DISPLAY_HZ}));
        EXPECT_EQ(host.waitTimeoutsMs, std::vector<int>{7}) << "hold";
    }
    {
        FakeLoopHost host;
        host.recoveryBoundMs = 7;
        FrameLoopState loop = makeLoop();
        const auto result = loop.waits(host, {.displayRefreshHz = DISPLAY_HZ});
        EXPECT_EQ(result.wait, FrameLoopState::Wait::Idle);
        EXPECT_EQ(host.waitTimeoutsMs, std::vector<int>{7}) << "idle";
    }
}

TEST(FrameLoopStateTest, InputPacesTheRegularFrameWithADelayNotAnEventWait)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    const auto result = loop.waits(host, {.hadEvents = true, .displayRefreshHz = DISPLAY_HZ});

    EXPECT_EQ(result.wait, FrameLoopState::Wait::PacedFrame);
    EXPECT_EQ(host.calls, calls({"delay"}));
    ASSERT_EQ(host.delaysNs.size(), 1U);
    EXPECT_NEAR(static_cast<double>(host.delaysNs[0]) / 1.0e9, (1.0 / FrameLoopState::MAX_FRAME_RATE) - SINCE_LAST_FRAME, 1e-6);
}

TEST(FrameLoopStateTest, AnInputGraceKeepsPacingAfterTheDrainThatStartedIt)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    const auto result = loop.waits(host, {.inputGraceActive = true, .displayRefreshHz = DISPLAY_HZ});
    EXPECT_EQ(result.wait, FrameLoopState::Wait::PacedFrame);
}

TEST(FrameLoopStateTest, NoPacedFrameOrIdleWaitAfterAMoveResizeFrame)
{
    // The move/resize frame waited for the display rate already; the drain handled events, so no idle wait.
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    const auto result = loop.waits(host, {.hadEvents = true, .didImmediateResizeRedraw = true, .displayRefreshHz = DISPLAY_HZ});

    EXPECT_EQ(result.wait, FrameLoopState::Wait::None);
    EXPECT_TRUE(host.calls.empty());
}

TEST(FrameLoopStateTest, AStaticFrameWaitsForTheFloorAndTheFirstFrameForTheIdleRate)
{
    {
        FakeLoopHost host;
        FrameLoopState loop = makeLoop();
        loop.lastFrameStart = START;
        const auto result = loop.waits(host, {.displayRefreshHz = DISPLAY_HZ});
        EXPECT_EQ(result.wait, FrameLoopState::Wait::Idle);
        EXPECT_EQ(result.idleWaitOutcome, IdleWaitOutcome::TimedOut);
        EXPECT_EQ(host.waitTimeoutsMs, std::vector<int>{FrameLoopState::STATIC_FRAME_FLOOR_MS});
    }
    {
        FakeLoopHost host;
        FrameLoopState loop = makeLoop();
        loop.lastFrameStart = START;
        loop.drewFirstFrame = false;
        static_cast<void>(loop.waits(host, {.displayRefreshHz = DISPLAY_HZ}));
        EXPECT_EQ(host.waitTimeoutsMs, std::vector<int>{FrameLoopState::IDLE_FRAME_SLEEP_MS});
    }
}

TEST(FrameLoopStateTest, AGraceWhileTheWindowStillMovesSkipsTheIdleWait)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.geometryChangedLastFrame = true;
    const auto result = loop.waits(host, {.interactionRedrawUntil = START + 1.0, .displayRefreshHz = DISPLAY_HZ});

    EXPECT_EQ(result.wait, FrameLoopState::Wait::None);
    EXPECT_TRUE(host.calls.empty());

    // Stationary within the grace: the idle wait at the idle rate, which any event wakes.
    loop.geometryChangedLastFrame = false;
    loop.lastFrameStart = START;
    EXPECT_EQ(loop.waits(host, {.interactionRedrawUntil = START + 1.0, .displayRefreshHz = DISPLAY_HZ}).wait, FrameLoopState::Wait::Idle);
    EXPECT_EQ(host.waitTimeoutsMs, std::vector<int>{FrameLoopState::IDLE_FRAME_SLEEP_MS});
}

TEST(FrameLoopStateTest, TheEventThatEndsAWaitIsKeptForTheNextDrain)
{
    // #1409, #1450: a wake, or a poll after a timeout that finds an event, leaves the event for the drain.
    for (const IdleWaitOutcome outcome : {IdleWaitOutcome::Woke, IdleWaitOutcome::PolledAfterTimeout})
    {
        FakeLoopHost host;
        host.waitOutcome = outcome;
        FrameLoopState loop = makeLoop();
        const auto result = loop.waits(host, {.displayRefreshHz = DISPLAY_HZ});
        EXPECT_EQ(result.idleWaitOutcome, outcome);
        EXPECT_EQ(loop.idleWakeEvent.value_or(SDL_Event{}).type, static_cast<std::uint32_t>(SDL_EVENT_KEY_DOWN));
        EXPECT_FALSE(loop.regularFrame(host, {})) << "the frame after a wake waits for the drain";
    }
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    EXPECT_EQ(loop.waitForEvent(host, 30, false), IdleWaitOutcome::TimedOut);
    EXPECT_FALSE(loop.idleWakeEvent.has_value());
}

TEST(FrameLoopStateTest, ATracedWaitRecordsItsTimeout)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    static_cast<void>(loop.waitForEvent(host, 30, true));
    EXPECT_EQ(loop.loopTiming.waitTimeoutMs, 30);

    loop.lastFrameStart = host.clock;

    static_cast<void>(loop.waits(host, {.hadEvents = true, .traced = true, .displayRefreshHz = DISPLAY_HZ}));
    EXPECT_EQ(loop.loopTiming.waitTimeoutMs, -1) << "a paced delay, not an event wait";
}

// ---- The regular frame ----

TEST(FrameLoopStateTest, TheRegularFrameDrawsAndPaysAHeldFrame)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.resizePresentOwed = true;

    EXPECT_TRUE(loop.regularFrame(host, {.traced = true}));
    EXPECT_FALSE(loop.resizePresentOwed) << "a frame at the current size pays any held one (#882)";
    EXPECT_DOUBLE_EQ(loop.lastFrameStart, START);
    EXPECT_EQ(host.calls, calls({"regularFrame"}));
    EXPECT_EQ(loop.resizeTraceStats.frames, 1U);
    EXPECT_EQ(loop.resizeTraceStats.resizeFrames, 0U);
}

TEST(FrameLoopStateTest, TheRegularFrameIsSkippedWhenSomethingElseDecides)
{
    const std::vector<FrameLoopState::RegularFrameInput> skips = {
        {.didImmediateResizeRedraw = true},
        {.skipRenderThisFrame = true},
        {.drawsNoFrames = true},
        {.holdPresent = true},
    };
    for (const auto& input : skips)
    {
        FakeLoopHost host;
        FrameLoopState loop = makeLoop();
        loop.resizePresentOwed = true;
        EXPECT_FALSE(loop.regularFrame(host, input));
        EXPECT_TRUE(host.calls.empty());
        EXPECT_TRUE(loop.resizePresentOwed) << "nothing drawn, so a held frame is still owed";
    }
}

TEST(FrameLoopStateTest, ARegularFrameWhileDrawingIsSuspendedDrawsNothing)
{
    FakeLoopHost host;
    host.drawing = false;
    FrameLoopState loop = makeLoop();
    EXPECT_FALSE(loop.regularFrame(host, {.traced = true}));
    EXPECT_EQ(host.calls, calls({"regularFrame"}));
    EXPECT_EQ(loop.resizeTraceStats.frames, 0U) << "nothing drawn counts as no frame (#3484)";
}

} // namespace
