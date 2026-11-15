#include "Core/FrameLoopState.h"

#include "Core/AnimationRequest.h"
#include "Core/FramePacing.h"
#include "Core/ResizePerfOperation.h"
#include "Core/ResizePerfTrace.h"
#include "Core/WindowEventRouting.h"
#include "Core/WindowVisibility.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <ratio>
#include <utility>

namespace Core
{

bool FrameLoopState::iterate(IFrameLoopHost& host, const bool traced)
{
    beginIteration(host, traced);
    const DrainResult drained = drain(host, traced);
    const IterationState state = afterDrain(host, drained, traced);
    if (state.stopped)
    {
        return false;
    }

    // The move/resize frame and the resize present hold (#882), then the iteration's wait, then the
    // regular frame, in this order.
    const InteractionFrameResult interaction = interactionFrame(host,
                                                                {
                                                                    .drawsNoFrames = state.hiddenWait.drawsNoFrames,
                                                                    .needsResizeRedraw = drained.needsResizeRedraw,
                                                                    .forceInteractionRedraw = state.forceInteractionRedraw,
                                                                    .hadEvents = drained.events.eventCount > 0,
                                                                    .isHidden = state.isHidden,
                                                                    .skipRenderThisFrame = state.skipRenderThisFrame,
                                                                    .traced = traced,
                                                                    .displayRefreshHz = host.displayRefreshHz(),
                                                                });

    const WaitResult waited = waits(host,
                                    {
                                        .hiddenWait = state.hiddenWait,
                                        .hadEvents = drained.events.eventCount > 0,
                                        .inputGraceActive = state.inputGraceActive,
                                        .isHidden = state.isHidden,
                                        .holdPresent = interaction.holdPresent,
                                        .didImmediateResizeRedraw = interaction.didImmediateResizeRedraw,
                                        .interactionRedrawUntil = host.interactionRedrawUntil(),
                                        .traced = traced,
                                        .displayRefreshHz = host.displayRefreshHz(),
                                    });
    if (waited.idleWaitOutcome.has_value())
    {
        host.noteIdleWait(*waited.idleWaitOutcome);
    }

    const bool drewRegularFrame = regularFrame(host,
                                               {
                                                   .didImmediateResizeRedraw = interaction.didImmediateResizeRedraw,
                                                   .skipRenderThisFrame = state.skipRenderThisFrame,
                                                   .drawsNoFrames = state.hiddenWait.drawsNoFrames,
                                                   .holdPresent = interaction.holdPresent,
                                                   .traced = traced,
                                               });

    endIteration(host,
                 {
                     .isInteracting = state.isInteracting,
                     .tracingInteraction = state.tracingInteraction,
                     .skipRenderThisFrame = state.skipRenderThisFrame,
                     .drewFrame = interaction.drewFrame || drewRegularFrame,
                     .traced = traced,
                 });
    return true;
}

void FrameLoopState::beginIteration(IFrameLoopHost& host, const bool traced)
{
    if (traced)
    {
        const auto now = SDL_GetPerformanceCounter();
        finishTracedLoop(now);
        loopStart = now;
        loopTiming = {};
    }
    // Snapshot geometry-changed flag from previous frame, then reset for this frame.
    geometryChangedLastFrame = host.takeGeometryChanged();
}

FrameLoopState::DrainResult FrameLoopState::drain(IFrameLoopHost& host, const bool traced)
{
    DrainResult drained;
    // Always capture drain start: used by P0 budget check and P3 skip-render decision
    // regardless of whether tracing is active.
    const auto eventDrainStart = host.drainClock();
    // P0: Drain time budget, checked after every event (#1410). On Wayland, individual
    // SDL_PollEvent calls can stall on compositor protocol; breaking here caps the combined
    // per-frame drain stall.
    drained.events = FramePacing::drainEventsWithinBudget(
        [&] { return pollAndDispatch(host, drained); },
        [&] { return std::chrono::duration<double, std::milli>(host.drainClock() - eventDrainStart).count(); },
        DRAIN_BUDGET_MS);
    // Always capture end time; used for both trace recording and P3 skip-render decision.
    const auto eventDrainEnd = host.drainClock();
    drained.totalDrainMs = std::chrono::duration<double, std::milli>(eventDrainEnd - eventDrainStart).count();
    if (traced)
    {
        loopTiming.drainMs = drained.totalDrainMs;
    }
    return drained;
}

bool FrameLoopState::pollAndDispatch(IFrameLoopHost& host, DrainResult& drained)
{
    // The event that woke the previous iteration's idle wait comes first: SDL_WaitEventTimeout() took
    // it off the queue (#1409).
    SDL_Event sdlEvent;
    if (idleWakeEvent.has_value())
    {
        sdlEvent = *idleWakeEvent;
        idleWakeEvent.reset();
    }
    else if (!host.pollEvent(sdlEvent))
    {
        return false;
    }
    // A publication (#3156): no layer handles it. Taken before this iteration draws, so a
    // publication after this point pushes a new wake, and the frame below shows every one before.
    if (host.takePublicationWake(sdlEvent))
    {
        return true;
    }
    if (WindowEventRouting::isInputEvent(sdlEvent.type))
    {
        ++drained.inputEventCount;
    }
    // Let layers handle raw SDL events (for ImGui integration and input handling)
    host.dispatchToLayers(sdlEvent);

    switch (WindowEventRouting::classify(sdlEvent.type))
    {
    case WindowEventRouting::Action::VetoableClose:
        // A close request becomes a WindowCloseEvent. A layer that handles it vetoes the
        // close; unhandled, the app stops (contract in WindowEvents.h).
        host.closeOnRequest();
        break;
    case WindowEventRouting::Action::Quit:
        // SIGINT/SIGTERM and OS logout/shutdown: always stops, and is not raised as a
        // WindowCloseEvent, so no layer can veto it (#1150).
        host.quit();
        break;
    // Drive viewport updates from resize-related events. On Windows, interactive border
    // drag can surface WINDOW_RESIZED/EXPOSED before (or instead of)
    // WINDOW_PIXEL_SIZE_CHANGED in some paths, so the physical pixel size is queried when
    // the event does not carry it.
    case WindowEventRouting::Action::PixelSizeChanged:
        handleResize(host, {sdlEvent.window.data1, sdlEvent.window.data2}, drained);
        break;
    case WindowEventRouting::Action::Resized:
        // The normal size a later maximize from outside the app restores to (#1250).
        host.windowGeometryChanged();
        handleResize(host, host.windowSizeInPixels(), drained);
        break;
    case WindowEventRouting::Action::Exposed:
        // A plain redraw request (#1154): having drained an event already rules out the
        // idle sleep, so the regular frame below repaints. Only an expose that reveals a
        // new size counts as a resize.
        if (const auto pixelSize = host.windowSizeInPixels(); WindowEventRouting::exposeChangesSize(lastResizePixelSize, pixelSize))
        {
            handleResize(host, pixelSize, drained);
        }
        break;
    case WindowEventRouting::Action::Moved:
        host.windowGeometryChanged(); // As for Resized (#1250)
        ++drained.resizeEventCount;
        host.setInteractionRedrawUntil(host.now() + INTERACTION_REDRAW_GRACE_SECONDS);
        break;
    case WindowEventRouting::Action::DisplayChanged:
        host.displayChanged();
        break;
    case WindowEventRouting::Action::SystemMaximized:
        // On Windows, Win+Up, snap to the top edge or ShowWindow(SW_MAXIMIZE) on the
        // borderless window is replaced by the title-bar button's maximize, whose resize
        // events follow (#1208): the quarter-screen maximize is SDL's Win32
        // WM_GETMINMAXINFO sizing. X11/XWayland window managers and Wayland compositors size
        // a maximized window themselves, and the asynchronous restore/maximize round trip is
        // untested there, so on Linux the maximize is kept as it is -- but tracked, so the
        // rectangle saved as the normal size is the one from before it, not the maximized
        // one (#1250).
        host.systemMaximized();
        break;
    case WindowEventRouting::Action::SystemRestored:
        // A window-manager or compositor restore of an OS maximize (#1250).
        host.systemRestored();
        break;
    case WindowEventRouting::Action::Minimized:
        // Win+Down on the client-side maximized window restores it, as it does a native
        // maximized one (#1279). The window's subclass procedure normally catches it before
        // the minimize happens; this is for a minimize that did not pass through it.
        host.shellMinimized();
        break;
    case WindowEventRouting::Action::DisplayScaleChanged:
        // Refresh the normal geometry's scale, as a move or resize would (#1250).
        host.windowGeometryChanged();
        break;
    case WindowEventRouting::Action::None:
        break;
    }

    return true;
}

void FrameLoopState::handleResize(IFrameLoopHost& host, const std::pair<int, int> pixelSize, DrainResult& drained)
{
    // A 0x0 framebuffer (minimised) is not a resize: neither raised nor counted as an
    // interaction, so it can't start the interaction pacing.
    if (!WindowEventRouting::isRealPixelSize(pixelSize))
    {
        return;
    }
    ++drained.resizeEventCount;
    // A new size, not the same size again (SDL's RESIZED and PIXEL_SIZE_CHANGED carry one
    // size twice): the resize present bound runs while the size keeps changing (#882).
    if (pixelSize != lastResizePixelSize)
    {
        lastSizeChangeTime = host.now();
    }
    lastResizePixelSize = pixelSize;
    host.raiseResized(pixelSize);
    drained.needsResizeRedraw = true;
    host.setInteractionRedrawUntil(host.now() + INTERACTION_REDRAW_GRACE_SECONDS);
}

FrameLoopState::IterationState FrameLoopState::afterDrain(IFrameLoopHost& host, const DrainResult& drained, const bool traced)
{
    IterationState state;
    // Input this drain restarts the input redraw grace (#843); while it runs, the regular frame below
    // is paced as an input frame (FramePacing::computeFrameRateCap) rather than taking the idle wait.
    inputRedrawUntil =
        FramePacing::computeInputRedrawUntil(host.now(), drained.inputEventCount, inputRedrawUntil, INPUT_REDRAW_GRACE_SECONDS);
    state.inputGraceActive = FramePacing::isWithinInteractionGrace(host.now(), inputRedrawUntil);
    // recordEventBatch() for THIS drain is deferred until after the interaction-transition
    // reset below (isInteracting depends on resizeEventCount/needsResizeRedraw from this
    // same drain), so the batch that triggers a transition lands in the correctly-reset
    // accumulator for its own state instead of being recorded into the old state's
    // accumulator and then immediately wiped (or misattributed into the other state's
    // boundary log) by that reset. The shouldClose() check that used to sit here is moved
    // below, after recordEventBatch() actually runs: otherwise a shutdown request arriving
    // in this drain would break out before recordEventBatch() executes at all, silently
    // dropping the final drain from the "shutdown" summary.

    state.forceInteractionRedraw = FramePacing::isWithinInteractionGrace(host.now(), host.interactionRedrawUntil());
    state.isInteracting =
        FramePacing::computeIsInteracting(drained.needsResizeRedraw, state.forceInteractionRedraw, drained.resizeEventCount);
    state.tracingInteraction = traced && state.isInteracting;

    // P1: Adaptive vsync — disable vsync at interaction start to break the
    // vsync/compositor-stall coupling that causes drain and swap spikes on
    // Wayland. Restore adaptive vsync when the interaction ends (after the
    // grace period expires) so idle frames remain tear-free.
    const auto vsyncStart = traced ? SDL_GetPerformanceCounter() : 0;
    switch (
        FramePacing::computeVsyncTransition(wasInteracting, state.isInteracting, host.vsyncRequested(), host.vsyncDisabledForInteraction()))
    {
    case FramePacing::VsyncTransition::Disable:
        host.setVsyncDisabledForInteraction(true);
        break;
    case FramePacing::VsyncTransition::Restore:
        host.setVsyncDisabledForInteraction(false);
        break;
    case FramePacing::VsyncTransition::NoChange:
        break;
    }
    if (traced)
    {
        loopTiming.vsyncMs = resizePerfElapsedMs(vsyncStart, SDL_GetPerformanceCounter());
    }

    if (traced && wasTracingInteraction && !state.tracingInteraction)
    {
        logResizePerfTraceSummary(resizeTraceStats, "interaction-end");
        resizeTraceStats = {};
        // Restart the cadence here too (mirroring the idle->interaction reset below):
        // lastResizeTraceLogTime is at most one 0.5s interaction-progress tick stale, so
        // without this the first post-interaction idle-progress window comes up short of a
        // full IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS, rather than starting fresh at the
        // moment idle begins.
        lastResizeTraceLogTime = host.now();
    }
    // Flush the partial idle interval before resetting, so rare stalls and tail counts
    // are not discarded when the user begins the next interaction.
    if (traced && !wasTracingInteraction && state.tracingInteraction)
    {
        logResizePerfTraceSummary(resizeTraceStats, "idle-end");
        resizeTraceStats = {};
        lastResizeTraceLogTime = host.now();
    }

    // Record THIS drain now that any transition reset above has run, so it's classified
    // into (and counted by) the accumulator for its own state, not the state that just
    // ended.
    if (traced)
    {
        resizeTraceStats.recordEventBatch(drained.events.eventCount,
                                          drained.resizeEventCount,
                                          drained.totalDrainMs,
                                          drained.events.maxSingleEventMs,
                                          drained.events.budgetExceeded);
        resizeTraceStats.inputEvents += drained.inputEventCount;
    }

    // Deferred from just after the drain (see comment above) so this frame's batch is
    // always recorded -- including the final one before shutdown -- before we might break.
    // Window::requestClose() (the custom title bar's Close button and system menu) is a close
    // request like Alt+F4, so it goes through the same WindowCloseEvent veto (#1077).
    if (host.closeOnWindowRequest())
    {
        state.stopped = true;
        return state;
    }

    // P3: If drain severely exceeded a full-frame budget, skip rendering this
    // frame to avoid compounding the stall with render+swap time. Events are
    // fully processed; the display catches up on the next frame.
    // Bounded at MAX_CONSECUTIVE_SKIPPED_RENDERS in a row, so a sustained slow drain can't
    // freeze the display (#1410).
    state.skipRenderThisFrame = FramePacing::computeSkipRenderThisFrame(drained.totalDrainMs,
                                                                        DRAIN_SKIP_RENDER_MS,
                                                                        host.isMinimized(),
                                                                        consecutiveSkippedRenders,
                                                                        FramePacing::MAX_CONSECUTIVE_SKIPPED_RENDERS);
    // Counted regardless of interaction state (not just tracingInteraction): an idle drain
    // that overruns the budget also skips rendering, and the idle-progress/shutdown
    // summaries should reflect that instead of always reporting skippedFrames=0 for idle.
    if (state.skipRenderThisFrame && traced)
    {
        ++resizeTraceStats.skippedRenderFrames;
    }

    // Minimized, covered (#1125), or on Windows cloaked, behind a locked or disconnected session or
    // on a display that is off (#3152): nothing on screen, so no frames at all once the first is
    // drawn (#3156); the loop waits for the event that shows the window again. Only drawing stops:
    // the samplers keep the Settings interval on their own threads (#2731).
    // A failed context replacement's retry (#3484), here and not in renderFrame(): a minimized or
    // hidden window draws no frames, and must still retry, or give up and stop.
    host.pollGraphicsRecovery();

    const WindowVisibility::HiddenState hiddenState = host.pollHiddenState();
    state.isHidden = hiddenState != WindowVisibility::HiddenState::Shown; // Window::isHidden()
    state.hiddenWait = FramePacing::computeHiddenWait(hiddenState, drewFirstFrame, HIDDEN_WAIT_TIMEOUT_MS, CLOAKED_POLL_INTERVAL_MS);
    // Nothing to redraw while no frames are drawn; the event that shows the window draws the newest data.
    host.pausePublicationWakes(state.hiddenWait.drawsNoFrames);
    return state;
}

FrameLoopState::InteractionFrameResult FrameLoopState::interactionFrame(IFrameLoopHost& host, const InteractionFrameInput& input)
{
    InteractionFrameResult result;
    // isHidden, not only minimized: an occluded window that gets a move/resize event must not render
    // at the display rate through the interaction grace period either (#1125). Within the grace, only
    // when there is something new to show (#3511): an iteration with no events and no client-side
    // geometry change falls to the regular frame below (paced, or the idle wait) instead of presenting
    // the same frame again.
    if (input.traced && FramePacing::isDeferredInteractionFrame(input.needsResizeRedraw,
                                                                input.forceInteractionRedraw,
                                                                input.hadEvents,
                                                                geometryChangedLastFrame,
                                                                input.isHidden,
                                                                input.skipRenderThisFrame))
    {
        ++resizeTraceStats.deferredInteractionFrames;
    }
    // While the size keeps changing, at most one frame per RESIZE_PRESENT_MIN_INTERVAL_SECONDS (#882): a
    // held iteration draws and presents nothing and waits for the next event or the end of the hold
    // below; the frame it owes is drawn once the hold ends, at the size of the moment. Not while
    // the hidden window draws no frames: the event that shows it draws the newest state.
    const FramePacing::ResizePresent resizePresent = input.drawsNoFrames
                                                       ? FramePacing::ResizePresent::Unbounded
                                                       : FramePacing::computeResizePresent({
                                                             .nowSeconds = host.now(),
                                                             .lastSizeChangeSeconds = lastSizeChangeTime,
                                                             .lastFrameStartSeconds = lastFrameStart,
                                                             .frameOwed = resizePresentOwed,
                                                             .minIntervalSeconds = resizePresentMinInterval,
                                                             .burstWindowSeconds = FramePacing::RESIZE_BURST_WINDOW_SECONDS,
                                                         });
    result.holdPresent = resizePresent == FramePacing::ResizePresent::Hold;
    result.presentOwedFrame = resizePresent == FramePacing::ResizePresent::PresentOwed;
    if (input.drawsNoFrames)
    {
        resizePresentOwed = false;
    }
    const bool wantsInteractionFrame = FramePacing::computeShouldRenderInteractionFrame(input.needsResizeRedraw || result.presentOwedFrame,
                                                                                        input.forceInteractionRedraw,
                                                                                        input.hadEvents,
                                                                                        geometryChangedLastFrame,
                                                                                        input.isHidden,
                                                                                        input.skipRenderThisFrame);
    // Only a held move/resize frame is owed: a regular frame the hold delays (an animation, an idle
    // frame) comes through the regular path anyway once the hold ends.
    if (result.holdPresent && wantsInteractionFrame)
    {
        resizePresentOwed = true;
        if (input.traced)
        {
            ++resizeTraceStats.heldResizePresents;
        }
    }
    // Keep interactive move/resize visually responsive across platforms without reintroducing
    // per-event rendering stalls: render at most once per drained event batch.
    if (wantsInteractionFrame && !result.holdPresent)
    {
        // Vsync is off during an interaction, so a stream of mouse events would otherwise render
        // unbounded (#1153): cap it at the display rate, which vsync would have given.
        const double interactionWaitSeconds = FramePacing::computeFrameWaitSeconds(
            host.now() - lastFrameStart, FramePacing::framePeriodSeconds(input.displayRefreshHz, AnimationRequest::FULL_RATE));
        if (interactionWaitSeconds > 0.0)
        {
            pacedDelay(host, interactionWaitSeconds, input.traced);
        }
        const bool drewResizeFrame = drawFrame(host, true, input.traced);
        result.drewFrame = drewResizeFrame;
        if (input.traced && drewResizeFrame && result.presentOwedFrame)
        {
            ++resizeTraceStats.owedResizePresents;
        }
        // Paid by any attempt, drawn or not: while drawing is suspended after a GPU reset (#3484) an
        // owed frame kept owing would make every iteration try again without waiting.
        resizePresentOwed = false;
        result.didImmediateResizeRedraw = true;
    }
    return result;
}

FrameLoopState::WaitResult FrameLoopState::waits(IFrameLoopHost& host, const WaitInput& input)
{
    // Pace the regular frame:
    // - Something moving visibly asked for more frames than idling gives, or input arrived --
    //   this drain, or recently enough that its grace still runs (#843): cap the frame rate
    //   (FramePacing::computeFrameRateCap) at a whole number of display refreshes. Charts keep a
    //   steady rate whatever the input (#1037), only as fast as their motion needs (#1125); input
    //   frames are capped too (#1153). Events that arrive during the wait are handled by the next
    //   drain, at most one period later.
    // - Otherwise, with the event queue empty, decide whether to sleep or render immediately:
    //   - Inside the grace period: skip the sleep and fall through to renderFrame so the
    //     display stays current during burst gaps between resize/move events.
    //   - Outside the grace period: sleep (FramePacing::computeIdlePeriodMs) to reduce CPU/GPU usage
    //     when the display hasn't changed: ~20 fps while something moves slowly or a grace runs, 5 fps
    //     for a hidden window that still draws, and for a static frame until the slow floor
    //     (STATIC_FRAME_FLOOR_INTERVAL) or a deadline the frame asked for (#3156). Any SDL event
    //     wakes the sleep immediately, keeping interactive frame rate unaffected, and so does a
    //     publication (the wake event above). The waking event is dispatched before the next
    //     frame renders (#1409): see idleWakeEvent.
    // A hidden window that draws nothing (#3156) only waits, for at most hiddenWait.timeoutMs, and
    // runs the layers' onHiddenUpdate(). After a drain that handled events it doesn't wait: the next
    // iteration drains again, and waits once the queue is empty. The event that shows the window
    // again wakes the wait; the next iteration dispatches it and draws.
    WaitResult result;
    const double animationFps = FramePacing::computeAnimationRate(requestedAnimationFps, IDLE_FRAME_RATE, MAX_FRAME_RATE);
    const double frameRateCap =
        FramePacing::computeFrameRateCap(animationFps, input.hadEvents || input.inputGraceActive, input.isHidden, MAX_FRAME_RATE);
    if (input.hiddenWait.drawsNoFrames)
    {
        result.wait = Wait::Hidden;
        if (!input.hadEvents)
        {
            // No longer than the next retry of a failed context replacement (#3484).
            static_cast<void>(waitForEvent(host, host.boundWaitMs(input.hiddenWait.timeoutMs, host.now()), input.traced));
        }
        host.hiddenUpdate();
    }
    // A held present (#882): no frame this iteration. Wait for the next event, which the next drain
    // handles at once, or for the end of the hold, when the owed frame is drawn. The event pump never
    // stops: any event ends the wait.
    else if (input.holdPresent)
    {
        result.wait = Wait::ResizeHold;
        // No longer than the next retry of a failed context replacement (#3484).
        static_cast<void>(waitForEvent(
            host,
            host.boundWaitMs(FramePacing::resizePresentHoldMs(host.now(), lastFrameStart, resizePresentMinInterval), host.now()),
            input.traced));
    }
    // Not after a move/resize frame, which waited for the display rate already; but a grace iteration
    // that deferred its frame (#3511) still paces an animation or input frame here.
    else if (FramePacing::computeUsesPacedRegularFrame(frameRateCap, input.didImmediateResizeRedraw))
    {
        result.wait = Wait::PacedFrame;
        const double waitSeconds = FramePacing::computeFrameWaitSeconds(
            host.now() - lastFrameStart, FramePacing::framePeriodSeconds(input.displayRefreshHz, frameRateCap));
        if (waitSeconds > 0.0)
        {
            pacedDelay(host, waitSeconds, input.traced);
        }
    }
    else if (!input.hadEvents)
    {
        const bool keepInteractionRedrawActive = FramePacing::isWithinInteractionGrace(host.now(), input.interactionRedrawUntil);
        // During the grace period, allow sleep if window geometry did not change last frame.
        // This prevents ~20 wasted renders after the user releases mouse while the window
        // is stationary. SDL_WaitEventTimeout wakes immediately on any event, so
        // responsiveness is unaffected. geometryChangedLastFrame reflects the previous
        // frame's onUpdate result (1-frame lag is intentional and benign).
        if (FramePacing::computeShouldSleepWhenIdle(keepInteractionRedrawActive, geometryChangedLastFrame))
        {
            // The first frame comes at the idle rate, as before: it is not a static frame yet.
            const int periodMs = FramePacing::computeIdlePeriodMs(
                {
                    .requestedFps = requestedAnimationFps,
                    .frameWithinSeconds = requestedFrameWithinSeconds,
                    .isHidden = input.isHidden,
                    .graceActive = keepInteractionRedrawActive || input.inputGraceActive || !drewFirstFrame,
                },
                {.idleMs = IDLE_FRAME_SLEEP_MS, .hiddenMs = MINIMIZED_FRAME_SLEEP_MS, .staticFloorMs = STATIC_FRAME_FLOOR_MS});
            // No longer than the next retry of a failed context replacement (#3484).
            const int sleepMs = host.boundWaitMs(FramePacing::computeIdleWaitMs(periodMs, host.now() - lastFrameStart), host.now());
            result.wait = Wait::Idle;
            result.idleWaitOutcome = waitForEvent(host, sleepMs, input.traced);
        }
    }
    return result;
}

bool FrameLoopState::regularFrame(IFrameLoopHost& host, const RegularFrameInput& input)
{
    // A wake on an event skips this render: the next iteration drains the event first, then
    // renders, paced as any input-driven frame (#1153) (#1409).
    // A held present (#882) skips it too: the owed frame follows once the hold ends.
    if (!FramePacing::computeShouldRenderRegularFrame(
            idleWakeEvent.has_value(), input.didImmediateResizeRedraw, input.skipRenderThisFrame, input.drawsNoFrames, input.holdPresent))
    {
        return false;
    }
    resizePresentOwed = false; // a frame at the current size pays any held one (#882)
    return drawFrame(host, false, input.traced);
}

void FrameLoopState::endIteration(IFrameLoopHost& host, const IterationEnd& input)
{
    // Log periodically regardless of interaction state (perf-plan #843 phase 0): idle
    // frames use a longer cadence than interaction frames (see
    // IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS above), and both share the same accumulator/p95
    // logging so idle and interactive numbers are directly comparable.
    const double perfTraceLogIntervalSeconds =
        input.isInteracting ? RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS : IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS;
    if (input.traced && ((host.now() - lastResizeTraceLogTime) >= perfTraceLogIntervalSeconds))
    {
        logResizePerfTraceSummary(resizeTraceStats, input.isInteracting ? "interaction-progress" : "idle-progress");
        // Reset only the per-interval counters, NOT the rolling percentile sample windows:
        // a periodic log within the same idle/interaction state should let those windows
        // keep accumulating past PERCENTILE_WINDOW_SIZE's threshold for a meaningful p99,
        // rather than restarting from an empty window (and a degenerate p99==max) every
        // single interval. The full `= {}` reset at actual state transitions (afterDrain())
        // still clears everything, including the rolling windows.
        resizeTraceStats.resetIntervalCounters();
        lastResizeTraceLogTime = host.now();
    }

    // Read after this iteration's render(s): what they drew decides how the next frame is paced.
    // A skipped render (or a wake that defers it), or one that drew nothing because drawing is
    // suspended after a GPU reset (#3484), leaves the previous answer standing.
    if (input.drewFrame)
    {
        requestedAnimationFps = AnimationRequest::consume();
        requestedFrameWithinSeconds = AnimationRequest::consumeFrameWithin();
        drewFirstFrame = true;
    }
    consecutiveSkippedRenders =
        FramePacing::nextConsecutiveSkippedRenders(consecutiveSkippedRenders, input.skipRenderThisFrame, input.drewFrame);

    wasTracingInteraction = input.tracingInteraction;
    wasInteracting = input.isInteracting;
}

void FrameLoopState::finishTracedLoop(const std::uint64_t end)
{
    if (loopStart == 0)
    {
        return;
    }
    const double wallMs = resizePerfElapsedMs(loopStart, end);
    resizePerfOperations().loops.record(wallMs);
    if (wallMs > 100.0)
    {
        // waitTimeout and waitOverrun at the end of the line, so the summary scripts' keys are unaffected:
        // a wait far past its own timeout was spent in SDL's message pump, not waiting (#3552).
        spdlog::info("ResizePerfLoop: beginCounter={} endCounter={} wall={:.3f} ms drain={:.3f} ms wait={:.3f} ms "
                     "vsync={:.3f} ms framePhases={:.3f} ms other={:.3f} ms waitTimeout={} ms waitOverrun={:.3f} ms",
                     loopStart,
                     end,
                     wallMs,
                     loopTiming.drainMs,
                     loopTiming.waitMs,
                     loopTiming.vsyncMs,
                     loopTiming.frameMs,
                     loopTiming.otherMs(wallMs),
                     loopTiming.waitTimeoutMs,
                     loopTiming.waitOverrunMs());
    }
}

FramePacing::IdleWaitOutcome FrameLoopState::waitForEvent(IFrameLoopHost& host, const int timeoutMs, const bool traced)
{
    // Wait with a real SDL_Event, not nullptr: the event that wakes the loop is taken off the queue here
    // and handed to the next iteration's drain, which dispatches it before rendering (#1409). With
    // nullptr the event stayed queued and the frame after the wait rendered straight after the wake,
    // showing the state from before the input. A wait that times out polls once more (#1450): a wake
    // from another thread can reach it late, with the event already queued, and that event is a wake too.
    const auto waitStart = traced ? SDL_GetPerformanceCounter() : 0;
    SDL_Event wakeEvent;
    const auto outcome = host.waitForEvent(wakeEvent, timeoutMs);
    if (outcome != FramePacing::IdleWaitOutcome::TimedOut)
    {
        idleWakeEvent = wakeEvent;
    }
    if (traced)
    {
        loopTiming.waitMs = resizePerfElapsedMs(waitStart, SDL_GetPerformanceCounter());
        loopTiming.waitTimeoutMs = timeoutMs;
    }
    return outcome;
}

void FrameLoopState::recordDeliveredFrame()
{
    // Deliver-to-deliver loop intervals (#843 measurement kit): renderFrame() stamps each presented
    // frame's end (recordResizePerfFrameEnd), and the gap from the previous one is the cadence the
    // user sees, skipped renders included. The previous frame's end is kept in resizeTraceStats, so
    // the `= {}` resets at idle<->interaction transitions drop the interval that spans both states.
    resizeTraceStats.recordDeliveredFrameEnd(resizePerfOperations().previousFrameEnd, SDL_GetPerformanceFrequency());
}

void FrameLoopState::pacedDelay(IFrameLoopHost& host, const double seconds, const bool traced)
{
    const auto waitStart = traced ? SDL_GetPerformanceCounter() : 0;
    host.delayPreciseNs(static_cast<std::uint64_t>(seconds * 1.0e9));
    if (traced)
    {
        loopTiming.waitMs = resizePerfElapsedMs(waitStart, SDL_GetPerformanceCounter());
        loopTiming.waitTimeoutMs = -1; // a paced delay, not an event wait
    }
}

bool FrameLoopState::drawFrame(IFrameLoopHost& host, const bool resizeTriggeredFrame, const bool traced)
{
    lastFrameStart = host.now();
    FrameTimes times;
    const bool drew = host.renderFrame(resizeTriggeredFrame, traced ? &times : nullptr);
    if (traced && drew)
    {
        resizeTraceStats.recordFrame(resizeTriggeredFrame, times.updateMs, times.renderMs, times.postRenderMs, times.swapMs);
        recordDeliveredFrame();
        loopTiming.frameMs = times.updateMs + times.renderMs + times.postRenderMs + times.swapMs;
    }
    return drew;
}

} // namespace Core
