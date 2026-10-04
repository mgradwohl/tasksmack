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

/// The UI scale in window units: SDL's display scale divided by the window's pixel density.
///
/// The window is created with SDL_WINDOW_HIGH_PIXEL_DENSITY, so on native Wayland (and macOS) window
/// coordinates are logical and the pixel density equals the compositor scale -- and imgui_impl_sdl3
/// already renders at that density (DisplayFramebufferScale), with ImGui rasterising fonts to match.
/// Scaling fonts, style metrics and chrome by the full display scale *as well* applied it twice:
/// 4x text at 200% instead of 2x (#1096). On Windows and X11 the density is 1, so nothing changes.
/// A density that is not a usable number (0 on failure, NaN) falls back to the display scale alone.
[[nodiscard]] inline float windowUnitScale(float displayScale, float pixelDensity) noexcept
{
    if (!std::isfinite(pixelDensity) || pixelDensity <= 0.0F)
    {
        return displayScale;
    }
    return displayScale / pixelDensity;
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
