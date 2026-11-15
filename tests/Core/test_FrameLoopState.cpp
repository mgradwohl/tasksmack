/// @file test_FrameLoopState.cpp
/// @brief Application::run()'s loop, each phase on its own (#3791): the drain, the bookkeeping after it (the graces,
/// the vsync transition, the trace's transitions, a close, the P3 skip, the hidden state), the move/resize frame and
/// the resize present hold (#882), the iteration's wait (a hidden window's, a held present's, a paced frame's, the idle
/// wait), each cut short for a failed GPU context replacement's retry (#3484), the regular frame and the end of the
/// iteration; then a whole iteration, and the host's own parts (the frame clock, the idle wait's timer, the idle-wait
/// record). A fake IFrameLoopHost stands in for the clock, SDL, the window and the layers, so no window opens.

#include "Core/AnimationRequest.h"
#include "Core/FrameLoopState.h"
#include "Core/FramePacing.h"
#include "Core/ResizePerfOperation.h"
#include "Core/WindowVisibility.h"

#include <SDL3/SDL_events.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{

using Core::FrameLoopState;
using Core::FramePacing::IdleWaitOutcome;
using Core::WindowVisibility::HiddenState;

constexpr double START = 100.0;
constexpr double DISPLAY_HZ = 60.0;
// Exactly representable, so the hold's remaining milliseconds are exact: 62.5 ms interval, 15.625 ms since the last
// frame, 46.875 ms left (rounded up to 47).
constexpr double MIN_INTERVAL = 0.0625;
constexpr double SINCE_LAST_FRAME = 0.015625;
constexpr int HOLD_MS = 47;

/// The clock, SDL, the window and the layers, as the loop sees them. Every call with an effect is logged in order; an
/// event wait that times out and a paced delay move the clock on by their length.
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
    /// Whether each renderFrame() was handed timings to fill: only a traced frame is.
    std::vector<bool> renderTimed;
    std::vector<IdleWaitOutcome> idleWaits;

    /// The queue pollEvent() takes from.
    std::deque<SDL_Event> queue;
    /// The drain's clock, moved on by drainStep at every reading.
    std::chrono::steady_clock::time_point drainNow;
    std::chrono::steady_clock::duration drainStep{};
    /// The publication wake's event type (0 = none).
    std::uint32_t publicationWakeType = 0;
    /// The event types the layers were handed.
    std::vector<std::uint32_t> dispatched;
    std::vector<std::pair<int, int>> raisedSizes;
    std::pair<int, int> windowSize{800, 600};
    double graceUntil = 0.0;
    bool geometryChanged = false;
    bool vsync = true;
    bool vsyncOffForInteraction = false;
    /// What a close the window asked for does: nothing pending, or pending and accepted.
    bool windowCloseAccepted = false;
    bool minimized = false;
    HiddenState hiddenState = HiddenState::Shown;
    std::vector<bool> wakesPaused;

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
        renderTimed.push_back(times != nullptr);
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

    [[nodiscard]] int boundWaitMs(const int waitMs, const double nowSeconds) override
    {
        calls.emplace_back("bound");
        // The recovery's retry is due relative to the time of the wait: the clock's reading now.
        EXPECT_DOUBLE_EQ(nowSeconds, clock) << "boundWaitMs() is given the clock's time";
        return std::min(waitMs, recoveryBoundMs);
    }

    void hiddenUpdate() override
    {
        calls.emplace_back("hiddenUpdate");
    }

    void noteIdleWait(const IdleWaitOutcome outcome) override
    {
        calls.emplace_back("noteIdleWait");
        idleWaits.push_back(outcome);
    }

    [[nodiscard]] std::chrono::steady_clock::time_point drainClock() override
    {
        const auto reading = drainNow;
        drainNow += drainStep;
        return reading;
    }

    bool pollEvent(SDL_Event& event) override
    {
        if (queue.empty())
        {
            return false;
        }
        event = queue.front();
        queue.pop_front();
        return true;
    }

    bool takePublicationWake(const SDL_Event& event) override
    {
        if (publicationWakeType == 0 || event.type != publicationWakeType)
        {
            return false;
        }
        calls.emplace_back("publicationWake");
        return true;
    }

    void dispatchToLayers(SDL_Event& event) override
    {
        dispatched.push_back(event.type);
    }

    void closeOnRequest() override
    {
        calls.emplace_back("closeOnRequest");
    }

    void quit() override
    {
        calls.emplace_back("quit");
    }

    [[nodiscard]] std::pair<int, int> windowSizeInPixels() override
    {
        calls.emplace_back("windowSize");
        return windowSize;
    }

    void raiseResized(const std::pair<int, int> pixelSize) override
    {
        calls.emplace_back("raiseResized");
        raisedSizes.push_back(pixelSize);
    }

    void windowGeometryChanged() override
    {
        calls.emplace_back("geometryChanged");
    }

    void displayChanged() override
    {
        calls.emplace_back("displayChanged");
    }

    void systemMaximized() override
    {
        calls.emplace_back("systemMaximized");
    }

    void systemRestored() override
    {
        calls.emplace_back("systemRestored");
    }

    void shellMinimized() override
    {
        calls.emplace_back("shellMinimized");
    }

    [[nodiscard]] double interactionRedrawUntil() override
    {
        return graceUntil;
    }

    void setInteractionRedrawUntil(const double seconds) override
    {
        graceUntil = seconds;
    }

    [[nodiscard]] bool takeGeometryChanged() override
    {
        return std::exchange(geometryChanged, false);
    }

    [[nodiscard]] double displayRefreshHz() override
    {
        return DISPLAY_HZ;
    }

    [[nodiscard]] bool vsyncRequested() override
    {
        return vsync;
    }

    [[nodiscard]] bool vsyncDisabledForInteraction() override
    {
        return vsyncOffForInteraction;
    }

    void setVsyncDisabledForInteraction(const bool disabled) override
    {
        calls.emplace_back(disabled ? "vsyncOff" : "vsyncOn");
        vsyncOffForInteraction = disabled;
    }

    bool closeOnWindowRequest() override
    {
        calls.emplace_back("closeOnWindowRequest");
        return windowCloseAccepted;
    }

    [[nodiscard]] bool isMinimized() override
    {
        return minimized;
    }

    void pollGraphicsRecovery() override
    {
        calls.emplace_back("pollGraphicsRecovery");
    }

    [[nodiscard]] HiddenState pollHiddenState() override
    {
        calls.emplace_back("pollHiddenState");
        return hiddenState;
    }

    void pausePublicationWakes(const bool paused) override
    {
        wakesPaused.push_back(paused);
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

TEST(FrameLoopStateTest, OnlyATracedFrameIsHandedTimings)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    EXPECT_TRUE(loop.regularFrame(host, {}));
    EXPECT_TRUE(loop.regularFrame(host, {.traced = true}));
    loop.lastFrameStart = START - 1.0;
    static_cast<void>(loop.interactionFrame(host, {.forceInteractionRedraw = true, .hadEvents = true, .displayRefreshHz = DISPLAY_HZ}));
    EXPECT_EQ(host.renderTimed, (std::vector<bool>{false, true, false}));
}

// ---- The drain ----

SDL_Event eventOf(const SDL_EventType type)
{
    SDL_Event event{};
    event.type = type;
    return event;
}

SDL_Event pixelSizeEvent(const int width, const int height)
{
    SDL_Event event = eventOf(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
    event.window.data1 = width;
    event.window.data2 = height;
    return event;
}

TEST(FrameLoopStateTest, TheDrainDispatchesTheEventThatWokeTheWaitFirst)
{
    // #1409: SDL_WaitEventTimeout() took the waking event off the queue; it is handled before the rest.
    FakeLoopHost host;
    host.queue = {eventOf(SDL_EVENT_MOUSE_MOTION)};
    FrameLoopState loop = makeLoop();
    loop.idleWakeEvent = eventOf(SDL_EVENT_KEY_DOWN);
    const auto drained = loop.drain(host, false);

    EXPECT_EQ(drained.events.eventCount, 2U);
    EXPECT_EQ(
        host.dispatched,
        (std::vector<std::uint32_t>{static_cast<std::uint32_t>(SDL_EVENT_KEY_DOWN), static_cast<std::uint32_t>(SDL_EVENT_MOUSE_MOTION)}));
    EXPECT_EQ(drained.inputEventCount, 1U) << "a key is input; mouse motion isn't (#843)";
    EXPECT_FALSE(loop.idleWakeEvent.has_value());
}

TEST(FrameLoopStateTest, APublicationWakeIsTakenAndNoLayerSeesIt)
{
    FakeLoopHost host;
    host.publicationWakeType = SDL_EVENT_USER;
    host.queue = {eventOf(SDL_EVENT_USER)};
    FrameLoopState loop = makeLoop();
    const auto drained = loop.drain(host, false);

    EXPECT_EQ(drained.events.eventCount, 1U) << "a wake still counts as an event drained";
    EXPECT_TRUE(host.dispatched.empty());
    EXPECT_EQ(host.calls, calls({"publicationWake"}));
}

TEST(FrameLoopStateTest, ANewSizeIsRaisedAndStartsTheMoveResizeGrace)
{
    FakeLoopHost host;
    host.queue = {pixelSizeEvent(1024, 768)};
    FrameLoopState loop = makeLoop();
    loop.lastResizePixelSize = {800, 600};
    const auto drained = loop.drain(host, false);

    EXPECT_TRUE(drained.needsResizeRedraw);
    EXPECT_EQ(drained.resizeEventCount, 1U);
    EXPECT_EQ(host.raisedSizes, (std::vector<std::pair<int, int>>{{1024, 768}}));
    EXPECT_EQ(loop.lastResizePixelSize, (std::pair<int, int>{1024, 768}));
    EXPECT_DOUBLE_EQ(loop.lastSizeChangeTime, START) << "the resize present bound runs from a new size (#882)";
    EXPECT_DOUBLE_EQ(host.graceUntil, START + FrameLoopState::INTERACTION_REDRAW_GRACE_SECONDS);

    // The same size again (SDL's RESIZED and PIXEL_SIZE_CHANGED carry one size twice): raised, but not a new size.
    host.clock = START + 1.0;
    host.queue = {pixelSizeEvent(1024, 768)};
    static_cast<void>(loop.drain(host, false));
    EXPECT_EQ(host.raisedSizes.size(), 2U);
    EXPECT_DOUBLE_EQ(loop.lastSizeChangeTime, START);
    EXPECT_DOUBLE_EQ(host.graceUntil, START + 1.0 + FrameLoopState::INTERACTION_REDRAW_GRACE_SECONDS);
}

TEST(FrameLoopStateTest, AZeroSizeIsNotAResize)
{
    // A minimised window's 0x0 framebuffer must not start the interaction pacing.
    FakeLoopHost host;
    host.queue = {pixelSizeEvent(0, 0)};
    FrameLoopState loop = makeLoop();
    const auto drained = loop.drain(host, false);

    EXPECT_FALSE(drained.needsResizeRedraw);
    EXPECT_EQ(drained.resizeEventCount, 0U);
    EXPECT_TRUE(host.raisedSizes.empty());
    EXPECT_DOUBLE_EQ(host.graceUntil, 0.0);
}

TEST(FrameLoopStateTest, AResizeReadsTheSizeAfterTheGeometryChange)
{
    FakeLoopHost host;
    host.windowSize = {640, 480};
    host.queue = {eventOf(SDL_EVENT_WINDOW_RESIZED)};
    FrameLoopState loop = makeLoop();
    static_cast<void>(loop.drain(host, false));

    EXPECT_EQ(host.calls, calls({"geometryChanged", "windowSize", "raiseResized"}));
    EXPECT_EQ(host.raisedSizes, (std::vector<std::pair<int, int>>{{640, 480}}));
}

TEST(FrameLoopStateTest, AnExposeIsAResizeOnlyWhenItRevealsANewSize)
{
    // #1154: a plain redraw request at the same size is no resize.
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.lastResizePixelSize = host.windowSize;
    host.queue = {eventOf(SDL_EVENT_WINDOW_EXPOSED)};
    EXPECT_FALSE(loop.drain(host, false).needsResizeRedraw);
    EXPECT_EQ(host.calls, calls({"windowSize"}));

    host.windowSize = {1280, 720};
    host.queue = {eventOf(SDL_EVENT_WINDOW_EXPOSED)};
    EXPECT_TRUE(loop.drain(host, false).needsResizeRedraw);
    EXPECT_EQ(host.raisedSizes, (std::vector<std::pair<int, int>>{{1280, 720}}));
}

TEST(FrameLoopStateTest, AMoveStartsTheGraceWithoutAResize)
{
    FakeLoopHost host;
    host.queue = {eventOf(SDL_EVENT_WINDOW_MOVED)};
    FrameLoopState loop = makeLoop();
    const auto drained = loop.drain(host, false);

    EXPECT_FALSE(drained.needsResizeRedraw);
    EXPECT_EQ(drained.resizeEventCount, 1U);
    EXPECT_EQ(host.calls, calls({"geometryChanged"}));
    EXPECT_DOUBLE_EQ(host.graceUntil, START + FrameLoopState::INTERACTION_REDRAW_GRACE_SECONDS);
}

TEST(FrameLoopStateTest, TheWindowsOwnEventsGoToTheLayersThenToTheirHandling)
{
    FakeLoopHost host;
    host.queue = {
        eventOf(SDL_EVENT_WINDOW_CLOSE_REQUESTED),
        eventOf(SDL_EVENT_QUIT),
        eventOf(SDL_EVENT_WINDOW_DISPLAY_CHANGED),
        eventOf(SDL_EVENT_WINDOW_MAXIMIZED),
        eventOf(SDL_EVENT_WINDOW_RESTORED),
        eventOf(SDL_EVENT_WINDOW_MINIMIZED),
        eventOf(SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED),
    };
    FrameLoopState loop = makeLoop();
    const auto drained = loop.drain(host, false);

    EXPECT_EQ(drained.events.eventCount, 7U);
    EXPECT_EQ(host.dispatched.size(), 7U);
    EXPECT_EQ(
        host.calls,
        calls({"closeOnRequest", "quit", "displayChanged", "systemMaximized", "systemRestored", "shellMinimized", "geometryChanged"}));
    EXPECT_EQ(drained.resizeEventCount, 0U);
}

TEST(FrameLoopStateTest, TheDrainStopsAtItsBudget)
{
    // P0 (#1410): 5 ms an event, so the second event takes the drain past its 8 ms budget.
    FakeLoopHost host;
    host.drainStep = std::chrono::milliseconds(5);
    for (int i = 0; i < 5; ++i)
    {
        host.queue.push_back(eventOf(SDL_EVENT_KEY_DOWN));
    }
    FrameLoopState loop = makeLoop();
    const auto drained = loop.drain(host, true);

    EXPECT_TRUE(drained.events.budgetExceeded);
    EXPECT_EQ(drained.events.eventCount, 2U);
    EXPECT_EQ(host.queue.size(), 3U) << "the rest waits for the next drain";
    EXPECT_DOUBLE_EQ(drained.totalDrainMs, 15.0) << "read once more at the end";
    EXPECT_DOUBLE_EQ(loop.loopTiming.drainMs, 15.0);
}

// ---- Between the drain and the frames ----

TEST(FrameLoopStateTest, AnInteractionTurnsVsyncOffAndItsEndTurnsItBackOn)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    const auto started = loop.afterDrain(host, {.resizeEventCount = 1, .needsResizeRedraw = true}, false);
    EXPECT_TRUE(started.isInteracting);
    EXPECT_FALSE(started.tracingInteraction) << "not traced";
    EXPECT_EQ(host.calls, calls({"vsyncOff", "closeOnWindowRequest", "pollGraphicsRecovery", "pollHiddenState"}));

    // The previous iteration interacted (endIteration()); the grace is over, so this one doesn't.
    loop.wasInteracting = true;
    host.calls.clear();
    const auto ended = loop.afterDrain(host, {}, false);
    EXPECT_FALSE(ended.isInteracting);
    EXPECT_EQ(host.calls, calls({"vsyncOn", "closeOnWindowRequest", "pollGraphicsRecovery", "pollHiddenState"}));
}

TEST(FrameLoopStateTest, VsyncStaysAsItIsWhenTheAppAskedForNone)
{
    FakeLoopHost host;
    host.vsync = false;
    FrameLoopState loop = makeLoop();
    static_cast<void>(loop.afterDrain(host, {.resizeEventCount = 1, .needsResizeRedraw = true}, false));
    EXPECT_EQ(host.calls, calls({"closeOnWindowRequest", "pollGraphicsRecovery", "pollHiddenState"}));
}

TEST(FrameLoopStateTest, TheGracesComeFromTheDrain)
{
    FakeLoopHost host;
    host.graceUntil = START + 0.1; // a move/resize event in an earlier drain
    FrameLoopState loop = makeLoop();
    const auto state = loop.afterDrain(host, {.inputEventCount = 1}, false);

    EXPECT_TRUE(state.inputGraceActive);
    EXPECT_DOUBLE_EQ(loop.inputRedrawUntil, START + FrameLoopState::INPUT_REDRAW_GRACE_SECONDS);
    EXPECT_TRUE(state.forceInteractionRedraw);
    EXPECT_TRUE(state.isInteracting) << "within the move/resize grace";
}

TEST(FrameLoopStateTest, AnAcceptedWindowCloseEndsTheIterationAfterItsBatchIsRecorded)
{
    FakeLoopHost host;
    host.windowCloseAccepted = true;
    FrameLoopState loop = makeLoop();
    FrameLoopState::DrainResult drained;
    drained.events.eventCount = 3;
    const auto state = loop.afterDrain(host, drained, true);

    EXPECT_TRUE(state.stopped);
    EXPECT_EQ(loop.resizeTraceStats.eventBatches, 1U) << "the final drain counts in the shutdown summary";
    EXPECT_EQ(loop.resizeTraceStats.drainedEvents, 3U);
    EXPECT_EQ(host.calls, calls({"closeOnWindowRequest"})) << "no recovery poll, no hidden state";
}

TEST(FrameLoopStateTest, TheWindowCloseIsCheckedAfterTheVsyncTransition)
{
    // The close comes after the vsync transition and the trace's batch, as it did in run(): an interaction that starts in
    // the drain that closes still turns vsync off first.
    FakeLoopHost host;
    host.windowCloseAccepted = true;
    FrameLoopState loop = makeLoop();
    const auto state = loop.afterDrain(host, {.resizeEventCount = 1, .needsResizeRedraw = true}, false);

    EXPECT_TRUE(state.stopped);
    EXPECT_EQ(host.calls, calls({"vsyncOff", "closeOnWindowRequest"}));
}

TEST(FrameLoopStateTest, ASlowDrainSkipsTheRenderButNotWhileMinimized)
{
    // P3: a drain longer than DRAIN_SKIP_RENDER_MS skips this iteration's render.
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    const FrameLoopState::DrainResult slow{.totalDrainMs = FrameLoopState::DRAIN_SKIP_RENDER_MS + 4.0};
    EXPECT_TRUE(loop.afterDrain(host, slow, true).skipRenderThisFrame);
    EXPECT_EQ(loop.resizeTraceStats.skippedRenderFrames, 1U);

    host.minimized = true;
    EXPECT_FALSE(loop.afterDrain(host, slow, false).skipRenderThisFrame);
    EXPECT_FALSE(loop.afterDrain(host, {.totalDrainMs = 1.0}, false).skipRenderThisFrame);
}

TEST(FrameLoopStateTest, AHiddenWindowDrawsNoFramesOnceTheFirstIsDrawnAndPausesTheWakes)
{
    {
        FakeLoopHost host;
        host.hiddenState = HiddenState::Hidden;
        FrameLoopState loop = makeLoop();
        const auto state = loop.afterDrain(host, {}, false);
        EXPECT_TRUE(state.isHidden);
        EXPECT_TRUE(state.hiddenWait.drawsNoFrames);
        EXPECT_EQ(state.hiddenWait.timeoutMs, FrameLoopState::HIDDEN_WAIT_TIMEOUT_MS);
        EXPECT_EQ(host.wakesPaused, std::vector<bool>{true});
    }
    {
        FakeLoopHost host;
        host.hiddenState = HiddenState::Cloaked;
        FrameLoopState loop = makeLoop();
        EXPECT_EQ(loop.afterDrain(host, {}, false).hiddenWait.timeoutMs, FrameLoopState::CLOAKED_POLL_INTERVAL_MS);
    }
    {
        // Before the first frame a hidden window still draws, so the loaded settings are published (#2731).
        FakeLoopHost host;
        host.hiddenState = HiddenState::Hidden;
        FrameLoopState loop = makeLoop();
        loop.drewFirstFrame = false;
        const auto state = loop.afterDrain(host, {}, false);
        EXPECT_TRUE(state.isHidden);
        EXPECT_FALSE(state.hiddenWait.drawsNoFrames);
        EXPECT_EQ(host.wakesPaused, std::vector<bool>{false});
    }
}

TEST(FrameLoopStateTest, TheTraceStartsAFreshSummaryWhenAnInteractionStartsAndEnds)
{
    FakeLoopHost host;
    host.clock = START + 3.0;
    FrameLoopState loop = makeLoop();
    loop.resizeTraceStats.skippedRenderFrames = 5;
    const auto started = loop.afterDrain(host, {.resizeEventCount = 1, .needsResizeRedraw = true}, true);
    EXPECT_TRUE(started.tracingInteraction);
    EXPECT_EQ(loop.resizeTraceStats.skippedRenderFrames, 0U) << "idle-end";
    EXPECT_EQ(loop.resizeTraceStats.resizeEvents, 1U) << "this drain lands in the interaction's summary";
    EXPECT_DOUBLE_EQ(loop.lastResizeTraceLogTime, START + 3.0);

    loop.wasTracingInteraction = true;
    loop.wasInteracting = true;
    loop.resizeTraceStats.skippedRenderFrames = 5;
    host.clock = START + 4.0;
    EXPECT_FALSE(loop.afterDrain(host, {}, true).tracingInteraction);
    EXPECT_EQ(loop.resizeTraceStats.skippedRenderFrames, 0U) << "interaction-end";
    EXPECT_DOUBLE_EQ(loop.lastResizeTraceLogTime, START + 4.0);
}

// ---- The end of the iteration ----

/// Clears Core::AnimationRequest's process-wide requests, before a test and after it.
void clearAnimationRequests()
{
    static_cast<void>(Core::AnimationRequest::consume());
    static_cast<void>(Core::AnimationRequest::consumeFrameWithin());
}

TEST(FrameLoopStateTest, ADrawnFrameTakesWhatItsContentAskedOfTheNextOne)
{
    clearAnimationRequests();
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.drewFirstFrame = false;
    Core::AnimationRequest::request(45.0);
    Core::AnimationRequest::requestFrameWithin(0.5);

    // Nothing drawn (a skipped render, or drawing suspended, #3484): the previous answer stands.
    loop.endIteration(host, {.drewFrame = false});
    EXPECT_DOUBLE_EQ(loop.requestedAnimationFps, 0.0);
    EXPECT_FALSE(loop.drewFirstFrame);

    loop.endIteration(host, {.drewFrame = true});
    EXPECT_DOUBLE_EQ(loop.requestedAnimationFps, 45.0);
    EXPECT_DOUBLE_EQ(loop.requestedFrameWithinSeconds, 0.5);
    EXPECT_TRUE(loop.drewFirstFrame);
    clearAnimationRequests();
}

TEST(FrameLoopStateTest, TheSkipCountRunsUntilAFrameIsDrawn)
{
    clearAnimationRequests();
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.endIteration(host, {.skipRenderThisFrame = true});
    loop.endIteration(host, {.skipRenderThisFrame = true});
    EXPECT_EQ(loop.consecutiveSkippedRenders, 2U);
    loop.endIteration(host, {});
    EXPECT_EQ(loop.consecutiveSkippedRenders, 2U) << "nothing drawn, nothing skipped";
    loop.endIteration(host, {.drewFrame = true});
    EXPECT_EQ(loop.consecutiveSkippedRenders, 0U);
    clearAnimationRequests();
}

TEST(FrameLoopStateTest, TheTraceSummaryIsLoggedAtItsCadence)
{
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.resizeTraceStats.frames = 3;

    // Interacting: every 0.5 s.
    loop.lastResizeTraceLogTime = START - (FrameLoopState::RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS / 2.0);
    loop.endIteration(host, {.isInteracting = true, .tracingInteraction = true, .traced = true});
    EXPECT_EQ(loop.resizeTraceStats.frames, 3U) << "not due";
    EXPECT_TRUE(loop.wasInteracting);
    EXPECT_TRUE(loop.wasTracingInteraction);

    loop.lastResizeTraceLogTime = START - FrameLoopState::RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS;
    loop.endIteration(host, {.isInteracting = true, .tracingInteraction = true, .traced = true});
    EXPECT_EQ(loop.resizeTraceStats.frames, 0U) << "logged: the interval's counters restart";
    EXPECT_DOUBLE_EQ(loop.lastResizeTraceLogTime, START);

    // Idle: every 5 s; and never untraced.
    loop.resizeTraceStats.frames = 3;
    loop.lastResizeTraceLogTime = START - FrameLoopState::RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS;
    loop.endIteration(host, {.traced = true});
    EXPECT_EQ(loop.resizeTraceStats.frames, 3U);
    EXPECT_FALSE(loop.wasInteracting);
    loop.lastResizeTraceLogTime = START - FrameLoopState::IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS;
    loop.endIteration(host, {});
    EXPECT_EQ(loop.resizeTraceStats.frames, 3U);
    loop.endIteration(host, {.traced = true});
    EXPECT_EQ(loop.resizeTraceStats.frames, 0U);
}

TEST(FrameLoopStateTest, ATracedLoopIsRecordedOnceItHasStarted)
{
    const auto& loops = Core::resizePerfOperations().loops;
    const auto before = loops.count;
    FrameLoopState loop = makeLoop();
    loop.finishTracedLoop(1000);
    EXPECT_EQ(loops.count, before) << "no traced loop yet";

    FakeLoopHost host;
    loop.beginIteration(host, true);
    EXPECT_NE(loop.loopStart, 0U);
    loop.loopTiming.drainMs = 3.0;
    loop.beginIteration(host, true);
    EXPECT_EQ(loops.count, before + 1) << "the previous loop's line is finished as the next begins";
    EXPECT_DOUBLE_EQ(loop.loopTiming.drainMs, 0.0) << "and the next loop's timing starts afresh";
}

TEST(FrameLoopStateTest, TheIterationTakesThePreviousFramesGeometryChange)
{
    FakeLoopHost host;
    host.geometryChanged = true;
    FrameLoopState loop = makeLoop();
    loop.beginIteration(host, false);
    EXPECT_TRUE(loop.geometryChangedLastFrame);
    EXPECT_FALSE(host.geometryChanged) << "cleared for this frame";
    loop.beginIteration(host, false);
    EXPECT_FALSE(loop.geometryChangedLastFrame);
}

// ---- A whole iteration ----

TEST(FrameLoopStateTest, AnIdleIterationWaitsForTheFloorThenDraws)
{
    clearAnimationRequests();
    FakeLoopHost host;
    FrameLoopState loop = makeLoop();
    loop.lastFrameStart = START;
    EXPECT_TRUE(loop.iterate(host, false));

    EXPECT_EQ(host.calls,
              calls({"closeOnWindowRequest", "pollGraphicsRecovery", "pollHiddenState", "bound", "wait", "noteIdleWait", "regularFrame"}));
    EXPECT_EQ(host.waitTimeoutsMs, std::vector<int>{FrameLoopState::STATIC_FRAME_FLOOR_MS});
    EXPECT_EQ(host.idleWaits, std::vector<IdleWaitOutcome>{IdleWaitOutcome::TimedOut});
    EXPECT_EQ(host.renderTimed, std::vector<bool>{false}) << "an untraced frame is handed no timings";
    clearAnimationRequests();
}

TEST(FrameLoopStateTest, AWakeDefersTheFrameToTheNextIterationWhichDispatchesTheEventFirst)
{
    // #1409: the frame after a wake shows the state after the input, not before it.
    clearAnimationRequests();
    FakeLoopHost host;
    host.waitOutcome = IdleWaitOutcome::Woke;
    FrameLoopState loop = makeLoop();
    loop.lastFrameStart = START;
    EXPECT_TRUE(loop.iterate(host, false));
    EXPECT_EQ(host.calls, calls({"closeOnWindowRequest", "pollGraphicsRecovery", "pollHiddenState", "bound", "wait", "noteIdleWait"}));
    EXPECT_EQ(host.idleWaits, std::vector<IdleWaitOutcome>{IdleWaitOutcome::Woke});

    // The key that woke it is drained, and its frame paced as an input frame.
    host.calls.clear();
    EXPECT_TRUE(loop.iterate(host, false));
    EXPECT_EQ(host.dispatched, std::vector<std::uint32_t>{static_cast<std::uint32_t>(SDL_EVENT_KEY_DOWN)});
    EXPECT_EQ(host.calls, calls({"closeOnWindowRequest", "pollGraphicsRecovery", "pollHiddenState", "delay", "regularFrame"}));
    clearAnimationRequests();
}

TEST(FrameLoopStateTest, AnAcceptedWindowCloseEndsTheLoopWithoutAFrame)
{
    FakeLoopHost host;
    host.windowCloseAccepted = true;
    FrameLoopState loop = makeLoop();
    EXPECT_FALSE(loop.iterate(host, false));
    EXPECT_EQ(host.calls, calls({"closeOnWindowRequest"}));
}

// ---- The host's own parts ----

TEST(FrameLoopStateHostTest, TheFrameClockCapsTheDeltaTimeButNotTheInterval)
{
    // Application::LoopHost: deltaTime for the layers, the real interval for lastFrameIntervalSeconds() (#1152).
    constexpr float MAX_DELTA = 1.0F;
    Core::FramePacing::FrameClock clock(10.0);
    const auto first = clock.tick(10.25, MAX_DELTA);
    EXPECT_FLOAT_EQ(first.deltaSeconds, 0.25F);
    EXPECT_DOUBLE_EQ(first.intervalSeconds, 0.25);

    const auto stalled = clock.tick(13.25, MAX_DELTA);
    EXPECT_FLOAT_EQ(stalled.deltaSeconds, MAX_DELTA) << "capped after a stall";
    EXPECT_DOUBLE_EQ(stalled.intervalSeconds, 3.0) << "a sub-1-FPS readout stays true";

    EXPECT_DOUBLE_EQ(clock.tick(13.5, MAX_DELTA).intervalSeconds, 0.25) << "from the previous frame";
}

/// IdleWaitTimer's arm() and disarm(), logged.
struct FakeIdleWaitTimer
{
    std::vector<std::string>* log = nullptr;

    void arm(const int timeoutMs) const
    {
        log->push_back("arm " + std::to_string(timeoutMs));
    }

    void disarm() const
    {
        log->emplace_back("disarm");
    }
};

TEST(FrameLoopStateHostTest, TheIdleWaitsTimerIsArmedForItsTimeoutAndDisarmedAfter)
{
    // #3712: on Windows the wait's timeout ends on time with IdleWaitTimer, armed for just that wait.
    for (const bool woke : {false, true})
    {
        std::vector<std::string> log;
        FakeIdleWaitTimer timer{.log = &log};
        SDL_Event event{};
        const auto outcome = Core::FramePacing::waitForIdleEventOnTime(
            timer,
            30,
            event,
            [&](SDL_Event& /*waited*/)
            {
                log.emplace_back("wait");
                return woke;
            },
            [&](SDL_Event& /*polled*/)
            {
                log.emplace_back("poll");
                return false;
            });
        if (woke)
        {
            EXPECT_EQ(outcome, IdleWaitOutcome::Woke);
            EXPECT_EQ(log, calls({"arm 30", "wait", "disarm"}));
        }
        else
        {
            EXPECT_EQ(outcome, IdleWaitOutcome::TimedOut);
            EXPECT_EQ(log, calls({"arm 30", "wait", "poll", "disarm"})) << "the poll after a timeout is inside the armed wait (#1450)";
        }
    }
}

TEST(FrameLoopStateHostTest, TheIdleWaitRecordCountsEachWaitAndKeepsHowTheLastEnded)
{
    // Application's idle-wait test seam (#1446, #1450).
    Core::FramePacing::IdleWaitRecord record;
    record.record(IdleWaitOutcome::Woke);
    EXPECT_EQ(record.count, 1U);
    EXPECT_TRUE(record.lastWoke);
    EXPECT_FALSE(record.lastPolledEvent);

    record.record(IdleWaitOutcome::PolledAfterTimeout);
    EXPECT_EQ(record.count, 2U);
    EXPECT_FALSE(record.lastWoke);
    EXPECT_TRUE(record.lastPolledEvent);

    record.record(IdleWaitOutcome::TimedOut);
    EXPECT_EQ(record.count, 3U);
    EXPECT_FALSE(record.lastWoke);
    EXPECT_FALSE(record.lastPolledEvent);
}

} // namespace
