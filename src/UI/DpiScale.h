#pragma once

// Extracted from UILayer.cpp's anonymous-namespace pointsToPixels() so the points->pixels
// conversion math is directly unit-testable without a live SDL window (which is where the
// `scale` parameter itself comes from, via SDL_GetWindowDisplayScale()) - see #770.

#include "Core/WindowGeometry.h"

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

/// Title-bar height in points. Everything drawn in the bar -- the application icon, the window and
/// app buttons, and their glyphs -- is derived from it, so they all scale together. It ignores the
/// Font Size setting on purpose: the title bar is chrome, not content.
inline constexpr float TITLE_BAR_PT = 24.0F;

/// The title bar's height in window units at @p displayScale, rounded to a whole unit so the bar's
/// edges and the controls inside it land on pixels. Shared by the normal font build and its
/// built-in-font fallback, so a failed rebuild still resizes the bar to the new scale (#1169).
[[nodiscard]] inline float computeTitleBarHeightPx(float displayScale) noexcept
{
    return std::round(computePointsToPixels(TITLE_BAR_PT, displayScale));
}

/// Font Awesome glyph size for the title bar's controls, as a share of the bar height: roughly the
/// wordmark's cap height, with even clearance above and below (see UILayer::loadAllFonts()).
inline constexpr float CHROME_ICON_RATIO = 0.55F;

/// The chrome icon font's pixel size for a title bar @p titleBarPx tall.
[[nodiscard]] inline float computeChromeIconPx(float titleBarPx) noexcept
{
    return std::round(titleBarPx * CHROME_ICON_RATIO);
}
/// The UI scale in window units: SDL's display scale divided by the window's pixel density.
///
/// The window is created with SDL_WINDOW_HIGH_PIXEL_DENSITY, so on native Wayland (and macOS) window
/// coordinates are logical and the pixel density equals the compositor scale -- and imgui_impl_sdl3
/// already renders at that density (DisplayFramebufferScale), with ImGui rasterising fonts to match.
/// Scaling fonts, style metrics and chrome by the full display scale *as well* applied it twice:
/// 4x text at 200% instead of 2x (#1096). On Windows and X11 the density is 1, so nothing changes.
/// A density that is not a usable number (0 on failure, NaN) falls back to the display scale alone.
/// The same rule as Core::WindowGeometry::windowUnitScale(), which the saved window size uses (#1168).
[[nodiscard]] inline float windowUnitScale(float displayScale, float pixelDensity) noexcept
{
    return Core::WindowGeometry::windowUnitScale(displayScale, pixelDensity);
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
