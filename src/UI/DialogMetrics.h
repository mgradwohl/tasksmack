#pragma once

// Font-derived geometry for the application's modal dialogs.
//
// #936 made the ImGuiStyle sizes scale, but a dialog that sets its own width, icon or button size in
// pixels is still pinned to one font size on one display density -- see #935 (About), #937
// (elevation notice) and #921 (Settings). The helpers here express that geometry in ems instead.
//
// One em is ImGui::GetFontSize(), which is documented as the scaled font height in pixels *after*
// global scale factors are applied, so it already accounts for both the Font Size setting and the
// display's density on Windows and Linux alike, with no per-platform branching.
//
// The em multiples at the call sites were derived from the pixel values they replace, so each
// dialog looks unchanged at the reference configuration and scales from there. At the Medium preset
// on a 1.0 display scale the body font is 8pt, which is exactly 32/3 px at 96 DPI, so for example
// the About box's former 96px icon is exactly 9 em and its 32px margin exactly 3 em.
//
// For anything sized to hold text, prefer measuring the text with ImGui::CalcTextSize() over an em
// multiple: it is self-documenting ("wide enough for 'Metric Refresh Rate'") and absorbs the
// glyph-metric differences between platforms automatically.
//
// The pure arithmetic lives here rather than at the call sites so it is unit-testable without a live
// ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a small header"
// pattern (as DpiScale.h, StyleScale.h and RateAxis.h do).

#include <algorithm>
#include <cmath>

namespace UI::DialogMetrics
{

/// Largest share of the viewport a dialog may claim.
///
/// Without this a font-derived width grows without bound: at the Even Huger preset on a scaled
/// display an em is roughly four times its reference size, so a dialog authored as 45 em would ask
/// for ~1900px and run off a small window entirely.
inline constexpr float MAX_VIEWPORT_FRACTION = 0.9F;

/// Horizontal breathing room inside an action button, either side of its label, in ems.
inline constexpr float BUTTON_LABEL_PADDING_EM = 1.0F;

/// Width of a dialog authored as a multiple of the body font, clamped so it still fits the window.
///
/// @param emPx             One em, i.e. ImGui::GetFontSize().
/// @param widthEm          Authored width in ems.
/// @param viewportWidthPx  Width of the viewport the dialog is centred in.
/// @return Width in pixels, never wider than MAX_VIEWPORT_FRACTION of the viewport.
[[nodiscard]] inline float computeDialogWidth(float emPx, float widthEm, float viewportWidthPx) noexcept
{
    const float safeEm = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;
    const float safeWidthEm = (std::isfinite(widthEm) && widthEm > 0.0F) ? widthEm : 1.0F;
    const float wanted = safeEm * safeWidthEm;

    if (!std::isfinite(viewportWidthPx) || viewportWidthPx <= 0.0F)
    {
        return wanted;
    }
    return std::min(wanted, viewportWidthPx * MAX_VIEWPORT_FRACTION);
}

/// Width of a dialog's action button: wide enough for its label, and never below a floor.
///
/// The floor is what keeps a short label like "OK" from collapsing to a button narrower than it is
/// tall, which is both ugly and a poor pointer target. The label term is what keeps a longer label
/// from being clipped once the font grows.
///
/// @param labelWidthPx  Measured label width, i.e. ImGui::CalcTextSize(label).x.
/// @param emPx          One em, i.e. ImGui::GetFontSize().
/// @param minWidthEm    Floor in ems, chosen per dialog to preserve its established proportions.
[[nodiscard]] inline float computeActionButtonWidth(float labelWidthPx, float emPx, float minWidthEm) noexcept
{
    const float safeEm = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;
    const float safeLabel = (std::isfinite(labelWidthPx) && labelWidthPx > 0.0F) ? labelWidthPx : 0.0F;
    const float safeMinEm = (std::isfinite(minWidthEm) && minWidthEm > 0.0F) ? minWidthEm : 0.0F;

    const float forLabel = safeLabel + (BUTTON_LABEL_PADDING_EM * 2.0F * safeEm);
    return std::max(forLabel, safeMinEm * safeEm);
}

} // namespace UI::DialogMetrics
