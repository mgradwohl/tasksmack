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
#include <limits>

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

/// Width of each of a dialog's two side-by-side action buttons (Cancel / Apply) on a row
/// @p availWidthPx wide: their preferred width, shrunk equally when the pair doesn't fit, so neither
/// button is pushed off the row. A capped dialog (#1129) can be narrower than the pair at large font
/// presets; right-aligned at their preferred width, Cancel would start left of the row's edge.
///
/// @param preferredWidthPx  From computeActionButtonWidth().
/// @param spacingPx         Gap between the two buttons (ItemSpacing.x).
/// @param availWidthPx      Width of the row.
[[nodiscard]] inline float fitActionButtonPairWidth(float preferredWidthPx, float spacingPx, float availWidthPx) noexcept
{
    const float preferred = (std::isfinite(preferredWidthPx) && preferredWidthPx > 0.0F) ? preferredWidthPx : 0.0F;
    const float spacing = (std::isfinite(spacingPx) && spacingPx > 0.0F) ? spacingPx : 0.0F;
    if (!std::isfinite(availWidthPx) || availWidthPx <= 0.0F)
    {
        return preferred;
    }
    const float fitting = std::max(0.0F, (availWidthPx - spacing) / 2.0F);
    return std::min(preferred, fitting);
}

/// Left edge of the value column in a dialog laid out as label / control rows.
///
/// Measured from the widest label rather than guessed, so the column is exactly as wide as the text
/// needs at the current font. A fixed column is wrong in both directions: too wide leaves dead space
/// between short labels and their controls, too narrow lets a long label run into them (#921).
///
/// @param widestLabelPx  Widest of the row labels, i.e. max of ImGui::CalcTextSize(label).x.
/// @param gapPx          Space between the label column and the controls.
[[nodiscard]] inline float computeValueColumnStart(float widestLabelPx, float gapPx) noexcept
{
    const float safeLabel = (std::isfinite(widestLabelPx) && widestLabelPx > 0.0F) ? widestLabelPx : 0.0F;
    const float safeGap = (std::isfinite(gapPx) && gapPx > 0.0F) ? gapPx : 0.0F;
    return safeLabel + safeGap;
}

/// Start offset for a narrower control that must share a right edge with a wider one above it.
///
/// Clamped so it can never start left of the value column: if the narrower control were ever the
/// wider of the two -- a longer translation, or an option list that grew -- the naive offset goes
/// negative relative to the column and the control is drawn over the labels.
///
/// @param valueColumnStartPx  Left edge of the value column, from computeValueColumnStart().
/// @param wideWidthPx         Width of the control defining the shared right edge.
/// @param narrowWidthPx       Width of the control being aligned to it.
[[nodiscard]] inline float computeRightAlignedStart(float valueColumnStartPx, float wideWidthPx, float narrowWidthPx) noexcept
{
    const float safeColumn = (std::isfinite(valueColumnStartPx) && valueColumnStartPx > 0.0F) ? valueColumnStartPx : 0.0F;
    const float safeWide = (std::isfinite(wideWidthPx) && wideWidthPx > 0.0F) ? wideWidthPx : 0.0F;
    const float safeNarrow = (std::isfinite(narrowWidthPx) && narrowWidthPx > 0.0F) ? narrowWidthPx : 0.0F;
    return safeColumn + std::max(0.0F, safeWide - safeNarrow);
}

/// Widen a control so its right edge meets the dialog's content edge, when another row is what
/// sets the dialog's width.
///
/// An auto-fitting dialog is as wide as its widest row. When that row is not the one holding the
/// control -- in Settings the two "ADVANCED" buttons are wider than the label-plus-combo rows -- a
/// control sized only to its own text stops short, and its right edge lines up with neither the
/// separators above it nor the buttons below (#972). The other rows are measured from text too, so
/// the width the dialog will take is known before layout, and the control can be sized to reach it.
///
/// Never narrower than the control's own content: when its row is the widest, nothing changes.
///
/// @param contentWidthPx       Width the control needs for its own text.
/// @param controlStartPx       Left edge of the control, measured from the dialog's left edge.
/// @param contentLeftPx        Left edge of the dialog's content (its window padding), same origin.
/// @param widestOtherRowPx     Width of the widest of the dialog's other rows.
[[nodiscard]] inline float
computeFilledControlWidth(float contentWidthPx, float controlStartPx, float contentLeftPx, float widestOtherRowPx) noexcept
{
    const float content = (std::isfinite(contentWidthPx) && contentWidthPx > 0.0F) ? contentWidthPx : 0.0F;
    const float start = (std::isfinite(controlStartPx) && controlStartPx > 0.0F) ? controlStartPx : 0.0F;
    const float left = (std::isfinite(contentLeftPx) && contentLeftPx > 0.0F) ? contentLeftPx : 0.0F;
    const float otherRow = (std::isfinite(widestOtherRowPx) && widestOtherRowPx > 0.0F) ? widestOtherRowPx : 0.0F;

    return std::max(content, (left + otherRow) - start);
}

/// Cap a width measured from content so its row still fits the viewport.
///
/// Measuring a control from the text it holds is right up until that text is user-supplied and
/// unbounded. Theme names are read straight from a user's TOML with no length limit, so a long one
/// would otherwise make the Settings combo -- and the auto-resizing popup around it -- arbitrarily
/// wide, pushing the combo's arrow and the buttons past the edge of the window where they cannot be
/// reached (#921 review).
///
/// The floor deliberately wins over the budget: if the row genuinely cannot fit, a control clipped
/// at a usable minimum is better than one shrunk to nothing. ImGui clips a combo's preview text, so
/// the control stays operable either way.
///
/// @param desiredWidthPx    Width the measured content asks for.
/// @param rowStartPx        Left edge of the control within the dialog.
/// @param surroundingPx     Everything else on the row that also needs space (padding, borders).
/// @param viewportWidthPx   Width of the viewport the dialog sits in.
/// @param minWidthPx        Smallest width that leaves the control usable.
[[nodiscard]] inline float
computeCappedControlWidth(float desiredWidthPx, float rowStartPx, float surroundingPx, float viewportWidthPx, float minWidthPx) noexcept
{
    const float safeDesired = (std::isfinite(desiredWidthPx) && desiredWidthPx > 0.0F) ? desiredWidthPx : 0.0F;
    const float safeMin = (std::isfinite(minWidthPx) && minWidthPx > 0.0F) ? minWidthPx : 0.0F;
    if (!std::isfinite(viewportWidthPx) || viewportWidthPx <= 0.0F)
    {
        return std::max(safeDesired, safeMin);
    }

    const float safeStart = (std::isfinite(rowStartPx) && rowStartPx > 0.0F) ? rowStartPx : 0.0F;
    const float safeSurrounding = (std::isfinite(surroundingPx) && surroundingPx > 0.0F) ? surroundingPx : 0.0F;
    const float budget = (viewportWidthPx * MAX_VIEWPORT_FRACTION) - safeStart - safeSurrounding;

    return std::max(safeMin, std::min(safeDesired, budget));
}

/// Largest extent, on one axis, a dialog may take in a viewport of @p viewportExtentPx (#1129).
///
/// Passed every frame as the maximum of ImGui::SetNextWindowSizeConstraints, so a dialog that grows
/// with the font, or a viewport that shrinks while it is open, can never push the dialog's edges --
/// and the buttons along them -- out of the window. MAX_VIEWPORT_FRACTION leaves a margin so the
/// dialog still reads as a dialog rather than a second window.
///
/// @return The cap in pixels, or a value no real size reaches when the viewport is unknown.
[[nodiscard]] inline float computeDialogMaxExtent(float viewportExtentPx) noexcept
{
    if (!std::isfinite(viewportExtentPx) || viewportExtentPx <= 0.0F)
    {
        return std::numeric_limits<float>::max();
    }
    return viewportExtentPx * MAX_VIEWPORT_FRACTION;
}

/// Whether a fixed-width leading item (an icon) and the content beside it fit on one row
/// @p availWidthPx wide, with @p gapPx between them and the content given at least
/// @p trailingMinPx. A row that does not fit is laid out stacked instead, so a narrow dialog at a
/// large font clips neither half (#1490 review).
///
/// @return true when unknown (non-finite) width leaves nothing to fit into.
[[nodiscard]] inline bool fitsSideBySide(float availWidthPx, float leadingPx, float gapPx, float trailingMinPx) noexcept
{
    const auto safe = [](float value)
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    if (!std::isfinite(availWidthPx))
    {
        return true;
    }
    return safe(leadingPx) + safe(gapPx) + safe(trailingMinPx) <= availWidthPx;
}

/// Largest extent, on one axis, of a compact dialog that should cover only @p fraction of a
/// viewport @p viewportExtentPx long, rather than the MAX_VIEWPORT_FRACTION every dialog may reach
/// (#1490). The About box caps its height this way and scrolls its contents past it, so it opens as
/// a dialog over the app instead of a second window on top of it.
///
/// @p fraction is clamped to (0, MAX_VIEWPORT_FRACTION]: never more than the shared cap, and a
/// non-positive or non-finite fraction falls back to it.
///
/// @return The cap in pixels, or a value no real size reaches when the viewport is unknown.
[[nodiscard]] inline float computeCompactDialogMaxExtent(float viewportExtentPx, float fraction) noexcept
{
    if (!std::isfinite(viewportExtentPx) || viewportExtentPx <= 0.0F)
    {
        return std::numeric_limits<float>::max();
    }
    const float safeFraction =
        (std::isfinite(fraction) && fraction > 0.0F) ? std::min(fraction, MAX_VIEWPORT_FRACTION) : MAX_VIEWPORT_FRACTION;
    return viewportExtentPx * safeFraction;
}

/// Tallest a dialog's scrolling body may be so that the rows pinned below it -- the action buttons
/// -- always stay inside the dialog, and so inside the viewport (#1129).
///
/// A dialog's height grows with the font preset and the display scale. Capped as a whole, an
/// auto-fitting dialog keeps its buttons at the bottom of its content, where at a large font in a
/// short window they could only be reached by scrolling, if at all. Capping just the body instead
/// keeps the buttons in view and lets the body scroll.
///
/// The floor wins over the budget: when even the pinned rows do not fit, a body still a couple of
/// rows tall stays usable, and the dialog's own size cap clips the rest.
///
/// @param dialogMaxHeightPx  The dialog's height cap, from computeDialogMaxExtent().
/// @param reservedPx         Everything in the dialog that is not the body: title bar, padding, the
///                           pinned rows and the spacing between them.
/// @param minBodyPx          Smallest height that leaves the body usable.
[[nodiscard]] inline float computeScrollableBodyMaxHeight(float dialogMaxHeightPx, float reservedPx, float minBodyPx) noexcept
{
    const float safeMin = (std::isfinite(minBodyPx) && minBodyPx > 0.0F) ? minBodyPx : 0.0F;
    if (!std::isfinite(dialogMaxHeightPx) || dialogMaxHeightPx <= 0.0F || dialogMaxHeightPx >= std::numeric_limits<float>::max())
    {
        return std::numeric_limits<float>::max();
    }
    const float safeReserved = (std::isfinite(reservedPx) && reservedPx > 0.0F) ? reservedPx : 0.0F;
    return std::max(safeMin, dialogMaxHeightPx - safeReserved);
}

} // namespace UI::DialogMetrics
