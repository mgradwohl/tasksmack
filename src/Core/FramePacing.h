#pragma once

// Pure decision logic extracted from Application::run()'s main loop, so it can be unit-tested
// directly instead of only indirectly through a live SDL/OpenGL application instance. Each
// function takes every input explicitly rather than reading Application member state -- see
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern (also used by
// App/TitleBarGeometry.h, App/Panels/AdaptiveIntervalUtils.h).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Core::FramePacing
{

/// The frame clock: nanosecond ticks (SDL_GetTicksNS) as a double of seconds, exact to well under a
/// microsecond for centuries of uptime. It was float(SDL_GetTicks()) / 1000 -- millisecond ticks in
/// a float whose spacing grows with the value (~7.8 ms after a day, ~31 ms after three), so after a
/// day or two deltaTime and every NowBar's smoothing were quantised to a few steps (#1038).
[[nodiscard]] constexpr auto ticksNsToSeconds(std::uint64_t ticksNs) -> double
{
    return static_cast<double>(ticksNs) / 1.0e9;
}

/// A frame's deltaTime from two frame-clock readings, capped at @p maxDeltaSeconds. The difference
/// is taken in double and only then narrowed: a float delta is fine, a float clock is not (#1038).
[[nodiscard]] constexpr auto frameDeltaSeconds(double previousSeconds, double currentSeconds, float maxDeltaSeconds) -> float
{
    return static_cast<float>(std::min(currentSeconds - previousSeconds, static_cast<double>(maxDeltaSeconds)));
}

/// P0: whether the SDL event-drain loop has spent too long on the current batch and should
/// break out early, capping how much a single frame's drain can stall (e.g. on Wayland
/// compositor protocol stalls). Checked periodically during drain, not every event.
[[nodiscard]] inline auto computeShouldBreakEventDrain(double elapsedDrainMs, double drainBudgetMs) -> bool
{
    return elapsedDrainMs >= drainBudgetMs;
}

/// Whether the current frame counts as an interactive (move/resize) frame: either this
/// frame's event drain produced a resize, a prior resize/move is still within its redraw
/// grace period, or any resize event was seen this drain.
[[nodiscard]] inline auto computeIsInteracting(bool needsResizeRedraw, bool forceInteractionRedraw, std::uint32_t resizeEventCount) -> bool
{
    return needsResizeRedraw || forceInteractionRedraw || (resizeEventCount > 0);
}

/// Whether "now" is still within an interaction's redraw grace window. Shared by both the
/// forceInteractionRedraw (this frame) and keepInteractionRedrawActive (idle-sleep gate)
/// checks in Application::run(), which compare the same two timestamps.
[[nodiscard]] inline auto isWithinInteractionGrace(double nowSeconds, double interactionRedrawUntilSeconds) -> bool
{
    return nowSeconds < interactionRedrawUntilSeconds;
}

/// P1: adaptive-vsync transition to apply given the previous and current interaction state.
/// Disabling vsync at interaction start breaks the vsync/compositor-stall coupling that
/// causes drain and swap spikes on Wayland; restoring it once the interaction (and its grace
/// period) ends keeps idle frames tear-free. NoChange covers every other combination,
/// including "already disabled" (vsyncCurrentlyDisabledForInteraction) or vsync not requested
/// in the app spec at all.
enum class VsyncTransition : std::uint8_t
{
    NoChange,
    Disable,
    Restore,
};

[[nodiscard]] inline auto
computeVsyncTransition(bool wasInteracting, bool isInteracting, bool vsyncRequestedInSpec, bool vsyncCurrentlyDisabledForInteraction)
    -> VsyncTransition
{
    if (!wasInteracting && isInteracting && vsyncRequestedInSpec)
    {
        return VsyncTransition::Disable;
    }
    if (wasInteracting && !isInteracting && vsyncCurrentlyDisabledForInteraction)
    {
        return VsyncTransition::Restore;
    }
    return VsyncTransition::NoChange;
}

/// P3: whether to skip rendering this frame entirely because the event drain already
/// consumed more than a full frame budget -- avoids compounding an input stall with
/// render+swap time; the display catches up next frame. Never skips while minimized (there's
/// nothing to render there anyway, and the minimized path has its own separate throttle).
[[nodiscard]] inline auto computeSkipRenderThisFrame(double totalDrainMs, double drainSkipRenderMs, bool isMinimized) -> bool
{
    return (totalDrainMs >= drainSkipRenderMs) && !isMinimized;
}

/// Whether to sleep (rather than render immediately) when the event queue is empty.
/// Sleeping is skipped -- falling through to an immediate render -- only while an
/// interaction's grace period is active AND window geometry changed last frame; that keeps
/// the display current during resize/move burst gaps without wasting renders once the window
/// is stationary post-interaction.
[[nodiscard]] inline auto computeShouldSleepWhenIdle(bool keepInteractionRedrawActive, bool geometryChangedLastFrame) -> bool
{
    return !keepInteractionRedrawActive || !geometryChangedLastFrame;
}

/// The display refresh rate to pace frames against (#1126): @p queriedHz from the window's display
/// mode, or @p fallbackHz when SDL reports none (0 means "unspecified") or nonsense.
/// Whether a refresh rate SDL reported is usable. A NaN fails the comparison; an infinite rate would
/// make the period 0.
[[nodiscard]] constexpr bool isUsableRefreshHz(double queriedHz) noexcept
{
    return queriedHz > 0.0 && queriedHz < 10000.0;
}

[[nodiscard]] constexpr auto effectiveRefreshHz(double queriedHz, double fallbackHz) noexcept -> double
{
    return isUsableRefreshHz(queriedHz) ? queriedHz : fallbackHz;
}

/// How many display refreshes (vblanks) one frame spans when targeting @p targetFps on a display
/// refreshing at @p refreshHz: max(1, floor(refresh / target)). Floor, not round: the cadence is never
/// slower than the target, so a motion-derived request (at most half a pixel per frame) or a
/// minimum-rate request is always met -- at 165 Hz a 60 FPS target paces at 82.5, not 55. Frames paced at a whole number of
/// refreshes present on every n-th vblank, instead of the uneven one/two (75 Hz) or two/three
/// (144 Hz) vblank gaps a fixed 1/60 s period gave with vsync (#1126). An infinite target means "the
/// display rate" (1).
[[nodiscard]] inline auto vblanksPerFrame(double refreshHz, double targetFps) noexcept -> int
{
    if (!(refreshHz > 0.0) || !(targetFps > 0.0))
    {
        return 1;
    }
    // A tiny epsilon so a ratio that is a whole number up to rounding (119.99999 / 60) isn't floored one
    // short.
    return std::max(1, static_cast<int>(std::floor((refreshHz / targetFps) + 1e-6)));
}

/// The minimum time between frame starts when targeting @p targetFps on a @p refreshHz display:
/// vblanksPerFrame() whole refreshes. The average frame rate is 1 / this period, so it must be a whole
/// number of refreshes even with vsync: a shorter "aim mid-interval" period (n - 0.5 refreshes) made
/// starts drift across vblanks and presents alternate between one and two refreshes (about 96 fps at
/// 144 Hz instead of 72). With vsync blocking the swap, a frame started n refreshes after the previous
/// start lands just after a vblank and presents on the next one, so every present is n refreshes
/// apart. For n == 1 the swap itself takes the refresh, so the period is a cap that rarely waits -- and
/// still caps the rate when the swap does not block (a compositor that queues frames).
[[nodiscard]] inline auto framePeriodSeconds(double refreshHz, double targetFps) noexcept -> double
{
    if (!(refreshHz > 0.0))
    {
        return (targetFps > 0.0) ? 1.0 / targetFps : 0.0;
    }
    return static_cast<double>(vblanksPerFrame(refreshHz, targetFps)) / refreshHz;
}

/// The animation rate to pace the next frame at, from the highest rate the previous frame asked for
/// (Core::AnimationRequest::consume(), 0 = nothing moved visibly), capped at @p maxFps. Returns 0 --
/// take the idle path -- when nothing asked for more than @p idleFps: the idle path already renders
/// that often without input, so pacing a slower motion would only cost frames (#1125).
[[nodiscard]] inline auto computeAnimationRate(double requestedFps, double idleFps, double maxFps) noexcept -> double
{
    if (!(requestedFps > idleFps))
    {
        return 0.0;
    }
    return std::min(requestedFps, maxFps);
}

/// The frame rate the loop caps the next regular (not move/resize) frame at, or 0 for none: the idle
/// path, which sleeps until an event or the idle timeout.
///
/// - Hidden (minimized or occluded): 0. Nothing on screen to animate, so the hidden idle sleep
///   applies (#1125: a covered window used to keep animating at 60 fps).
/// - Input arrived this drain: @p inputFps. Input-driven frames used to be uncapped -- the display
///   rate with vsync, unbounded without (#1153) -- now they share the full animation rate.
/// - Otherwise @p animationFps (computeAnimationRate()): charts animate at a steady rate whatever the
///   input (#1037), but only as fast as their on-screen motion needs (#1125).
[[nodiscard]] inline auto computeFrameRateCap(double animationFps, bool hadInput, bool isHidden, double inputFps) noexcept -> double
{
    if (isHidden)
    {
        return 0.0;
    }
    return hadInput ? std::max(animationFps, inputFps) : animationFps;
}

/// How long to wait before starting a frame so that frames start at most once per @p periodSeconds,
/// given the time since the last frame started. Uses a plain delay, not an event wait: an event that
/// arrives meanwhile waits at most one period, and the next drain handles it.
[[nodiscard]] inline auto computeFrameWaitSeconds(double secondsSinceFrameStart, double periodSeconds) noexcept -> double
{
    return std::max(0.0, periodSeconds - secondsSinceFrameStart);
}

/// The frame's real duration, for an FPS readout: the difference between two frame-clock readings,
/// *not* capped like frameDeltaSeconds(). The cap protects animation from a jump after a stall; an
/// FPS readout fed the capped delta reports "10 FPS" for any frame slower than 100 ms (#1152).
[[nodiscard]] constexpr auto frameIntervalSeconds(double previousSeconds, double currentSeconds) noexcept -> double
{
    return std::max(0.0, currentSeconds - previousSeconds);
}

/// Idle-sleep duration: a longer sleep while hidden -- minimized or occluded (#1125), nothing
/// visible to update -- than the normal idle rate.
[[nodiscard]] inline auto computeIdleSleepMs(bool isHidden, int idleFrameSleepMs, int minimizedFrameSleepMs) -> int
{
    return isHidden ? minimizedFrameSleepMs : idleFrameSleepMs;
}

/// How long to wait before the next idle frame: until computeIdleSleepMs() after the previous frame
/// *started*, not that long after it ended. A fixed post-frame sleep added the frame's own render time
/// on top, so the nominal 20 FPS idle rate was really about 17 (#1276), slower than
/// computeAnimationRate() assumes when it leaves motion of up to 20 FPS to the idle path.
/// Rounded up to whole milliseconds (SDL_WaitEventTimeout's unit) so the deadline isn't undershot.
[[nodiscard]] inline auto computeIdleWaitMs(bool isHidden, int idleFrameSleepMs, int minimizedFrameSleepMs, double secondsSinceFrameStart)
    -> int
{
    const auto periodMs = static_cast<double>(computeIdleSleepMs(isHidden, idleFrameSleepMs, minimizedFrameSleepMs));
    const double elapsedMs = std::max(0.0, secondsSinceFrameStart) * 1000.0;
    return static_cast<int>(std::ceil(std::max(0.0, periodMs - elapsedMs)));
}

} // namespace Core::FramePacing
