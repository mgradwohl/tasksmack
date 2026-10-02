#pragma once

// Pure placement arithmetic for a line that carries text at both ends, extracted so it is
// unit-testable without a live ImGui context, following CONTRIBUTING.md's "extract the pure decision
// logic into a small header" pattern (as DialogMetrics.h and StyleScale.h do).

#include <algorithm>
#include <cmath>

namespace UI::LineLayout
{

/// Where a right-aligned block goes on a line that already has text at its left.
struct TrailingBlockPlacement
{
    bool sameLine = true; ///< False: the block does not fit beside the leading text; put it on its own line.
    float x = 0.0F;       ///< Left edge of the block, in the same coordinate space as the inputs.
};

/// Place a right-aligned block after leading text without ever drawing one over the other (#967).
///
/// The system Overview positioned its "Processes: N  Up: ..." block purely from the right edge, so
/// on a window too narrow for both it was drawn straight over the CPU summary on the same line and
/// neither could be read. The block now shares the line only when it fits with a gap to spare, and
/// otherwise drops to a line of its own, still right-aligned.
///
/// All positions are in one coordinate space; the caller picks it (window-local X, which is what
/// ImGui::SameLine() and ImGui::SetCursorPosX() take).
///
/// @param lineStartX    Left edge of the line: where a block on its own line may start at the earliest.
/// @param leadingEndX   Right edge of the text already on the line.
/// @param rightEdgeX    Right edge the block is aligned to.
/// @param blockWidthPx  Width of the block.
/// @param minGapPx      Least space to leave between the leading text and the block.
[[nodiscard]] inline TrailingBlockPlacement
placeTrailingBlock(float lineStartX, float leadingEndX, float rightEdgeX, float blockWidthPx, float minGapPx) noexcept
{
    const float start = std::isfinite(lineStartX) ? lineStartX : 0.0F;
    const float width = (std::isfinite(blockWidthPx) && blockWidthPx > 0.0F) ? blockWidthPx : 0.0F;
    const float gap = (std::isfinite(minGapPx) && minGapPx > 0.0F) ? minGapPx : 0.0F;
    const float rightEdge = std::isfinite(rightEdgeX) ? rightEdgeX : start;
    const float leadingEnd = std::isfinite(leadingEndX) ? leadingEndX : start;

    // Right-aligned, but never left of the line's own start: a block wider than the line starts at
    // the leading edge and is clipped at the trailing one, rather than being pushed off the left.
    const float alignedX = std::max(start, rightEdge - width);

    return {.sameLine = (leadingEnd + gap) <= alignedX, .x = alignedX};
}

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
