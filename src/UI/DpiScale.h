#pragma once

// Extracted from UILayer.cpp's anonymous-namespace pointsToPixels() so the points->pixels
// conversion math is directly unit-testable without a live SDL window (which is where the
// `scale` parameter itself comes from, via SDL_GetWindowDisplayScale()) - see #770.

#include <cmath>

namespace UI
{

/// Convert typographic points to pixels given a display scale factor.
/// Standard: 1 point = 1/72 inch; base DPI assumed 96 (Windows/Linux standard), so
/// effective DPI = 96 * scale.
[[nodiscard]] constexpr float computePointsToPixels(float points, float scale) noexcept
{
    constexpr float BASE_DPI = 96.0F;
    return points * (BASE_DPI * scale) / 72.0F;
}

/// Smallest difference between two display scales treated as a change. SDL computes the scale in
/// floating point, so an exact comparison is meaningless at that precision (and flagged by CodeQL).
inline constexpr float DISPLAY_SCALE_EPSILON = 1e-4F;

/// Whether a freshly measured display scale differs from the one the fonts and style were built at.
///
/// False for a measurement that is not a usable scale: SDL_GetWindowDisplayScale() returns 0.0 on
/// failure, and rebuilding the fonts at a zero or NaN density would make all text vanish (#943).
[[nodiscard]] inline bool displayScaleChanged(float current, float measured) noexcept
{
    if (!std::isfinite(measured) || measured <= 0.0F)
    {
        return false;
    }
    return std::abs(measured - current) >= DISPLAY_SCALE_EPSILON;
}

} // namespace UI
