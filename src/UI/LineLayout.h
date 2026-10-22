#pragma once

// Pure placement arithmetic for label columns and on-screen spans, extracted so it is
// unit-testable without a live ImGui context, following CONTRIBUTING.md's "extract the pure decision
// logic into a small header" pattern (as DialogMetrics.h and StyleScale.h do).

#include <algorithm>
#include <cmath>

namespace UI::LineLayout
{

/// Space between the widest label in a label column and the value column beside it, in ems.
inline constexpr float LABEL_COLUMN_GAP_EM = 1.0F;

/// Width of the label column in a "Label:  value" table, from the widest label it holds (#966).
///
/// These columns were pixel literals (150px, 120px, 30px) on tables that are not resizable, so ImGui
/// re-applied the literal every frame and the column never tracked the font: at Extra Large on a
/// 175% display "GPU Utilization:" was cut to "GPU Utilizatior" and at Even Huger to "GPU Utiliza".
/// A measured column is exactly as wide as its text at any font, plus a gap that scales with it.
///
/// @param widestLabelPx  Widest label in the column, i.e. max of ImGui::CalcTextSize(label).x.
/// @param emPx           One em, i.e. ImGui::GetFontSize().
[[nodiscard]] inline float labelColumnWidth(float widestLabelPx, float emPx) noexcept
{
    const float widest = (std::isfinite(widestLabelPx) && widestLabelPx > 0.0F) ? widestLabelPx : 0.0F;
    const float em = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 0.0F;
    return widest + (LABEL_COLUMN_GAP_EM * em);
}

/// Where a span of `lengthPx` should start so that it lies inside the bounds, moved as little as
/// possible from `startPx`. One axis of keeping a floating window on screen.
///
/// A window that was placed while the viewport was large can be left partly or wholly outside it
/// once the viewport shrinks, with its title bar and close button out of reach. This pulls it back:
/// unchanged when it already fits, flush with the nearer edge when it overhangs, and flush with the
/// leading edge when it is longer than the bounds (so the part that holds the title bar stays
/// visible and the excess is cut off at the trailing end).
///
/// @param startPx        Where the span currently starts.
/// @param lengthPx       Its length.
/// @param boundsStartPx  Start of the bounds (a viewport's work position on this axis).
/// @param boundsLengthPx Length of the bounds (the viewport's work size on this axis).
[[nodiscard]] inline float clampSpanStart(float startPx, float lengthPx, float boundsStartPx, float boundsLengthPx) noexcept
{
    const float boundsStart = std::isfinite(boundsStartPx) ? boundsStartPx : 0.0F;
    const float boundsLength = (std::isfinite(boundsLengthPx) && boundsLengthPx > 0.0F) ? boundsLengthPx : 0.0F;
    const float length = (std::isfinite(lengthPx) && lengthPx > 0.0F) ? lengthPx : 0.0F;
    if (!std::isfinite(startPx))
    {
        return boundsStart;
    }

    // The latest the span may start and still end inside the bounds; never before the bounds'
    // own start, which is what a span longer than the bounds falls back to.
    const float latestStart = std::max(boundsStart, boundsStart + boundsLength - length);
    return std::clamp(startPx, boundsStart, latestStart);
}
} // namespace UI::LineLayout
