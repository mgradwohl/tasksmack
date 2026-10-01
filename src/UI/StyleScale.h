#pragma once

// Pure scale-factor arithmetic for ImGuiStyle sizing, extracted so it is unit-testable without a
// live ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a small
// header" pattern (as DpiScale.h, RateAxis.h and ProcessTreeIndent.h do).

#include <algorithm>
#include <cmath>

namespace UI
{

/// Point size the base ImGuiStyle literals in Theme::applyImGuiStyle() were authored against: the
/// Medium preset, on a 1.0 display scale. Keeping this explicit is what makes the scale factor
/// exactly 1.0 for the default configuration, so the app looks unchanged there.
inline constexpr float STYLE_REFERENCE_PT = 8.0F;

/// Floor on the scale factor. Only a guard against a degenerate font or display scale producing a
/// zero-size style; the smallest real preset (Small, 6pt) is 0.75, well above it.
inline constexpr float STYLE_SCALE_MIN = 0.25F;

/// Factor the ImGuiStyle size literals are multiplied by, so padding, spacing, indents, scrollbars
/// and grab sizes track both the chosen font size and the display's density.
///
/// ImGui does not scale the style itself: without this every padding and spacing value stays frozen
/// at the pixel literals, so text grows with the Font Size setting while the chrome around it does
/// not, and a scaled display (common on Windows) gets proportionally undersized chrome (#936).
///
/// Deliberately NOT applied via ImGuiStyle::ScaleAllSizes(), which is the obvious route and is
/// wrong here for two measured reasons.
///
/// It scales WindowMinSize, and ImGui clamps every non-child window up to that minimum
/// (CalcWindowMinSize). The custom title bar is a 32px window, so at the Even Huger preset
/// ScaleAllSizes raised WindowMinSize.y to 64 and ImGui stretched the title-bar window to 64px,
/// where it painted over the main tab row beneath it -- the tab row was not failing to draw, it was
/// covered. Measured: title-bar background ended at y=32 unscaled, y=64 with ScaleAllSizes.
///
/// It is also not idempotent. applyImGuiStyle() re-assigns the fields it authors from their
/// literals on every call, but ScaleAllSizes touches many it does not -- _MainScale, WindowMinSize,
/// TabMinWidthBase, TabMinWidthShrink, TabBarOverlineSize and more -- so each theme or font change
/// doubled them again: WindowMinSize.y went 32 -> 64 -> 128 across three calls at startup.
///
/// Multiplying the authored literals instead is idempotent by construction, touches only fields
/// this application owns, and avoids ScaleAllSizes' ImTrunc, which would truncate the 1px border
/// sizes to zero for any factor below 1.0 and so forced a clamp that stopped the Small preset
/// shrinking at all.
///
/// @param regularPt     Body-text point size of the active font preset (Theme::fontConfig()).
/// @param displayScale  Display scale from SDL_GetWindowDisplayScale(); 1.0 at 96 DPI.
[[nodiscard]] inline float computeStyleScale(float regularPt, float displayScale) noexcept
{
    // Guard against a zero/garbage scale from a headless or not-yet-mapped window.
    const float safeScale = (std::isfinite(displayScale) && displayScale > 0.0F) ? displayScale : 1.0F;
    const float safePt = (std::isfinite(regularPt) && regularPt > 0.0F) ? regularPt : STYLE_REFERENCE_PT;

    return std::max(STYLE_SCALE_MIN, (safePt / STYLE_REFERENCE_PT) * safeScale);
}

} // namespace UI
