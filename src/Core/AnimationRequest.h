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
// between two of them, so sub-pixel motion no longer holds the app at 60 fps. requestForEasing() does
// the same for an element easing toward a new value (a NowBar's fill, a rescaling axis), with the
// coarser MAX_EASING_PIXELS_PER_FRAME (#2626).
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

/// The most an easing element -- a NowBar's fill edge, a rate axis rescaling its chart -- may step
/// between two frames, in pixels, before it needs another frame (#2626). Coarser than
/// MAX_MOTION_PIXELS_PER_FRAME: an ease is a short settle toward a new sample, not a line that must
/// stay continuous, and every visible bar starts one each refresh. At half a pixel, the small
/// settles of an idle machine's NowBars asked for 30-60 fps for much of every second, so the
/// app rendered about 34 fps at idle instead of the 20 fps idle rate (v0.8.0 eased every bar at 20 fps).
/// At this tolerance a bar asks for more than the idle rate only while it moves faster than
/// 4 px x 20 fps = 80 px/s: a real change, which still eases at up to the full animation rate.
inline constexpr double MAX_EASING_PIXELS_PER_FRAME = 4.0;

/// The frame rate an element moving at @p pixelsPerSecond on screen needs so that it moves at most
/// @p maxPixelsPerFrame (by default MAX_MOTION_PIXELS_PER_FRAME) per frame. 0 for no motion, a
/// negative speed or NaN; an infinite speed gives an infinite rate, which asks for the loop's full rate
/// (like FULL_RATE).
[[nodiscard]] constexpr auto framesPerSecondForMotion(double pixelsPerSecond,
                                                      double maxPixelsPerFrame = MAX_MOTION_PIXELS_PER_FRAME) noexcept -> double
{
    // A NaN fails the comparison, so it asks for nothing.
    if (!(pixelsPerSecond > 0.0) || !(maxPixelsPerFrame > 0.0))
    {
        return 0.0;
    }
    return pixelsPerSecond / maxPixelsPerFrame;
}

/// The frame rate an element easing at @p pixelsPerSecond on screen needs: at most
/// MAX_EASING_PIXELS_PER_FRAME per frame (#2626).
[[nodiscard]] constexpr auto framesPerSecondForEasing(double pixelsPerSecond) noexcept -> double
{
    return framesPerSecondForMotion(pixelsPerSecond, MAX_EASING_PIXELS_PER_FRAME);
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

/// Ask for the rate an element easing at @p pixelsPerSecond on screen needs (framesPerSecondForEasing()).
inline void requestForEasing(double pixelsPerSecond) noexcept
{
    request(framesPerSecondForEasing(pixelsPerSecond));
}

/// The highest rate asked for since the last call (0 if nothing asked), clearing the request.
[[nodiscard]] inline auto consume() noexcept -> double
{
    const double rate = Detail::requestedRate();
    Detail::requestedRate() = 0.0;
    return rate;
}

} // namespace Core::AnimationRequest
