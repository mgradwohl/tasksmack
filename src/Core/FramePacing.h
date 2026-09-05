#pragma once

// Pure decision logic extracted from Application::run()'s main loop, so it can be unit-tested
// directly instead of only indirectly through a live SDL/OpenGL application instance. Each
// function takes every input explicitly rather than reading Application member state -- see
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern (also used by
// App/TitleBarGeometry.h, App/Panels/AdaptiveIntervalUtils.h).

#include <cstdint>

namespace Core::FramePacing
{

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
[[nodiscard]] inline auto isWithinInteractionGrace(float nowSeconds, float interactionRedrawUntilSeconds) -> bool
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

/// Idle-sleep duration: a longer sleep while minimized (nothing visible to update) than the
/// normal idle rate.
[[nodiscard]] inline auto computeIdleSleepMs(bool isMinimized, int idleFrameSleepMs, int minimizedFrameSleepMs) -> int
{
    return isMinimized ? minimizedFrameSleepMs : idleFrameSleepMs;
}

} // namespace Core::FramePacing
