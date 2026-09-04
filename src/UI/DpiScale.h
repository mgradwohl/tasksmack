#pragma once

// Extracted from UILayer.cpp's anonymous-namespace pointsToPixels() so the points->pixels
// conversion math is directly unit-testable without a live SDL window (which is where the
// `scale` parameter itself comes from, via SDL_GetWindowDisplayScale()) - see #770.

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

} // namespace UI
