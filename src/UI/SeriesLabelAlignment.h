#pragma once

// Pure decisions for where a chart's series labels (its value strip, the chart's only key) go:
// which side each label aligns to, from the chart's Y axes, and where the two groups of labels sit
// on the strip's line. Extracted so they are unit-testable without a live ImGui context, following
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern (as LineLayout.h
// does for the heading's trailing block).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace UI::SeriesLabelAlignment
{

/// A side of a chart: where a Y axis draws its scale, and where a series label aligns.
enum class AxisSide : std::uint8_t
{
    Left,
    Right,
};

/// A chart's Y axes as its labels see them (#1882): the side its primary (Y1) scale is drawn on. A
/// second scale (UI::Widgets::setupSecondaryRateAxis()) is always drawn opposite it, as is a third
/// (UI::Widgets::setupRightRateAxis() on ImAxis_Y3, #1916), and the series on either are the ones
/// whose labels end in UI::Widgets::SECONDARY_AXIS_MARK.
struct ChartAxes
{
    AxisSide primary = AxisSide::Left;
};

[[nodiscard]] constexpr AxisSide oppositeSide(AxisSide side) noexcept
{
    return (side == AxisSide::Left) ? AxisSide::Right : AxisSide::Left;
}

/// The side a series' label aligns to: the side of the axis it is read against (#1882). On a chart
/// with one Y axis every label takes that axis's side; on a dual-axis chart the primary axis's series
/// take its side and the secondary axis's series (@p onSecondaryAxis) the other.
[[nodiscard]] constexpr AxisSide labelSide(ChartAxes axes, bool onSecondaryAxis) noexcept
{
    return onSecondaryAxis ? oppositeSide(axes.primary) : axes.primary;
}

/// Where a value strip's two groups of labels go: the left-aligned group and the right-aligned one.
struct StripPlacement
{
    /// Both groups share the line of the item drawn just before the strip (the chart's heading).
    bool sameLine = false;
    /// Both groups fit one row; otherwise the left group wraps from the line's start and the right
    /// group starts a row of its own.
    bool oneRow = true;
    /// Left edge of the left-aligned group: just after the heading on its line, else the line's start.
    float leftX = 0.0F;
    /// Left edge of the right-aligned group: its right edge on @p rightEdgeX, never left of the line's
    /// start (a group wider than the row starts there and wraps).
    float rightX = 0.0F;
};

/// Places a value strip whose left-aligned labels are @p leftWidthPx wide and right-aligned labels
/// @p rightWidthPx wide (each group's entries and the gaps between them) on a row from @p lineStartX
/// to @p rightEdgeX (the chart's right edge), with at least @p gapPx between the heading (ending at
/// @p headingEndX) and the left group, and between the two groups (#1882).
///
/// The strip shares the heading's line when the heading, the left group and the right group all fit
/// on it with their gaps -- the same room the right-aligned strip it replaced needed (#1300), so a
/// chart's height does not change. Otherwise it takes a line of its own: the left group at the line's
/// start, the right group flush right. A strip wider than its row wraps instead (oneRow false).
///
/// All positions are in one coordinate space; the caller picks it (window-local X, which is what
/// ImGui::SameLine() and ImGui::SetCursorPosX() take).
[[nodiscard]] inline StripPlacement
placeStrip(float lineStartX, float headingEndX, float rightEdgeX, float leftWidthPx, float rightWidthPx, float gapPx) noexcept
{
    const float start = std::isfinite(lineStartX) ? lineStartX : 0.0F;
    const float rightEdge = std::isfinite(rightEdgeX) ? std::max(start, rightEdgeX) : start;
    const float headingEnd = std::isfinite(headingEndX) ? headingEndX : start;
    const float left = (std::isfinite(leftWidthPx) && leftWidthPx > 0.0F) ? leftWidthPx : 0.0F;
    const float right = (std::isfinite(rightWidthPx) && rightWidthPx > 0.0F) ? rightWidthPx : 0.0F;
    const float gap = (std::isfinite(gapPx) && gapPx > 0.0F) ? gapPx : 0.0F;
    const float between = (left > 0.0F && right > 0.0F) ? gap : 0.0F;

    StripPlacement placement;
    placement.rightX = std::max(start, rightEdge - right);
    placement.oneRow = (left + between + right) <= (rightEdge - start);
    placement.sameLine = placement.oneRow && (headingEnd + gap + left + between + right) <= rightEdge;
    placement.leftX = placement.sameLine ? (headingEnd + gap) : start;
    return placement;
}

} // namespace UI::SeriesLabelAlignment
