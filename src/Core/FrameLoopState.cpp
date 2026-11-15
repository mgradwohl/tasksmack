#include "Core/FrameLoopState.h"

#include "Core/AnimationRequest.h"
#include "Core/FramePacing.h"
#include "Core/ResizePerfOperation.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>

#include <cstdint>

namespace Core
{

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
