#pragma once

// A per-frame "something on screen is moving" request from the UI to the main loop (#1037), carrying
// the frame rate the motion needs (#1125).
//
// History charts scroll every frame (x is "seconds before now") and NowBars ease toward their
// targets, so while one visibly moves the loop renders at a steady animation rate instead of idling.
// How fast is the motion's business: a 300 s chart 1000 px wide scrolls ~3 px/s, which needs far
// fewer frames than a NowBar jumping to a new sample, so each request carries a rate and the loop
// paces at the highest one (Core::FramePacing::computeAnimationRate). requestForMotion() derives the
// rate from on-screen speed: enough frames that nothing moves more than MAX_MOTION_PIXELS_PER_FRAME
// between two of them, so sub-pixel motion no longer holds the app at 60 fps.
//
// The UI calls request() while drawing a frame; Application::run() calls consume() after it, and
// paces the next frame from the answer. A direct value rather than a Core::Event: like
// Application::signalWindowGeometryChanged(), it is a hint to the frame loop, not a notification
// for layers, and it must not depend on an Application instance (chart code runs in tests without
// one). UI thread only.

#include <algorithm>
#include <limits>

namespace Core::AnimationRequest
{

/// Ask for this to get the loop's full animation rate (capped by the loop, see Application.cpp).
inline constexpr double FULL_RATE = std::numeric_limits<double>::infinity();

/// The most a moving element may travel between two frames, in pixels, before it needs another frame.
/// Half a pixel keeps anti-aliased lines visibly continuous while a slow chart idles.
inline constexpr double MAX_MOTION_PIXELS_PER_FRAME = 0.5;

/// The frame rate an element moving at @p pixelsPerSecond on screen needs so that it moves at most
/// MAX_MOTION_PIXELS_PER_FRAME per frame. 0 for no motion, a negative speed or NaN; an infinite speed
/// gives an infinite rate, which asks for the loop's full rate (like FULL_RATE).
[[nodiscard]] constexpr auto framesPerSecondForMotion(double pixelsPerSecond) noexcept -> double
{
    // A NaN fails the comparison, so it asks for nothing.
    if (!(pixelsPerSecond > 0.0))
    {
        return 0.0;
    }
    return pixelsPerSecond / MAX_MOTION_PIXELS_PER_FRAME;
}

namespace Detail
{
[[nodiscard]] inline auto requestedRate() noexcept -> double&
{
    static double rate = 0.0;
    return rate;
}
} // namespace Detail

/// Ask for the next frame at @p framesPerSecond or faster: call while drawing something that moves.
/// Several requests in one frame keep the highest. Non-positive (and NaN) rates ask for nothing.
inline void request(double framesPerSecond) noexcept
{
    if (framesPerSecond > 0.0)
    {
        Detail::requestedRate() = std::max(Detail::requestedRate(), framesPerSecond);
    }
}

/// Ask for the full animation rate: for motion whose speed is not known (an easing axis).
inline void request() noexcept
{
    request(FULL_RATE);
}

/// Ask for the rate an element moving at @p pixelsPerSecond on screen needs (framesPerSecondForMotion()).
inline void requestForMotion(double pixelsPerSecond) noexcept
{
    request(framesPerSecondForMotion(pixelsPerSecond));
}

/// The highest rate asked for since the last call (0 if nothing asked), clearing the request.
[[nodiscard]] inline auto consume() noexcept -> double
{
    const double rate = Detail::requestedRate();
    Detail::requestedRate() = 0.0;
    return rate;
}

} // namespace Core::AnimationRequest
