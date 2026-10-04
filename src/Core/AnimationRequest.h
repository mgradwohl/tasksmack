#pragma once

// A per-frame "something on screen is animating" request from the UI to the main loop (#1037).
//
// History charts scroll every frame (x is "seconds before now") and NowBars ease toward their
// targets, so while one is visible the loop renders at a steady animation rate instead of idling.
// The UI calls request() while drawing a frame; Application::run() calls consume() after it, and
// paces the next frame from the answer. A direct flag rather than a Core::Event: like
// Application::signalWindowGeometryChanged(), it is a hint to the frame loop, not a notification
// for layers, and it must not depend on an Application instance (chart code runs in tests without
// one). UI thread only.

namespace Core::AnimationRequest
{

namespace Detail
{
[[nodiscard]] inline auto flag() noexcept -> bool&
{
    static bool requested = false;
    return requested;
}
} // namespace Detail

/// Ask for the next frame at the animation rate: call while drawing something that moves.
inline void request() noexcept
{
    Detail::flag() = true;
}

/// Whether anything asked since the last call, clearing the request.
[[nodiscard]] inline auto consume() noexcept -> bool
{
    const bool requested = Detail::flag();
    Detail::flag() = false;
    return requested;
}

} // namespace Core::AnimationRequest
