#pragma once

// Pure width arithmetic for the Processes table, extracted so the fill-or-scroll decision is
// unit-testable without a live ImGui context, following CONTRIBUTING.md's "extract the pure decision
// logic into a small header" pattern (as ProcessTableFlags.h and ProcessTreeIndent.h do).

#include <algorithm>
#include <cmath>

namespace App::ProcessTableLayout
{

/// Inner width to pass to ImGui::BeginTable() so the Command column fills the table without ever
/// collapsing (#924).
///
/// The table scrolls horizontally, and every column used to be fixed-width, so when the columns
/// summed to less than the window the remainder was simply left blank -- about half the window at
/// the Small preset. Command is the natural column to absorb that slack: it is unbounded text and
/// already last. But a plain stretch column cannot do it alone, because under ImGuiTableFlags_ScrollX
/// a stretch column is sized against the *visible* width: it fills when the other columns leave
/// room, and collapses to nothing as soon as they do not.
///
/// ImGui's answer is the inner_width parameter: an explicit content width that stretch columns are
/// sized against instead. So Command is a stretch column, and this picks the inner width:
///
///   - the other columns leave room for Command's minimum  -> 0, "fit the visible width", and
///     Command takes whatever is left with no scrollbar;
///   - they do not                                         -> exactly enough for the other columns
///     plus Command's minimum, so Command keeps a readable width and the table scrolls.
///
/// @param otherColumnsWidthPx   Width of everything in the table that is not Command's own content:
///                              the other enabled columns plus all cell padding and spacing.
///                              Measured from the previous frame's layout.
/// @param commandMinWidthPx     Narrowest Command may be before the table scrolls instead.
/// @param visibleWidthPx        Width of the table's visible area, from the previous frame.
/// @return 0 to let ImGui fit the visible width, otherwise the content width to scroll within.
[[nodiscard]] inline float computeInnerWidth(float otherColumnsWidthPx, float commandMinWidthPx, float visibleWidthPx) noexcept
{
    // No measurement yet (first frame), or a degenerate one: fit the visible width. The next frame
    // has real numbers and corrects it.
    if (!std::isfinite(otherColumnsWidthPx) || !std::isfinite(commandMinWidthPx) || !std::isfinite(visibleWidthPx) ||
        otherColumnsWidthPx <= 0.0F || visibleWidthPx <= 0.0F)
    {
        return 0.0F;
    }

    const float needed = otherColumnsWidthPx + (commandMinWidthPx > 0.0F ? commandMinWidthPx : 0.0F);
    return (needed > visibleWidthPx) ? needed : 0.0F;
}

/// Slack allowed before a cell's text counts as clipped, in pixels.
///
/// Text widths and column widths are both fractional and arrive by different arithmetic, so text
/// that exactly fills its cell can measure a hair wider than the space. Without this, such a cell
/// would trade its last glyph for an ellipsis, or flicker between the two as the layout settles.
inline constexpr float CLIP_TOLERANCE_PX = 0.5F;

/// Whether text of the given width does not fit in the cell space left for it (#914).
///
/// A clipped cell is drawn with an ellipsis and gets a tooltip carrying the full value, so a value
/// that has been cut is distinguishable from one that is genuinely that short: "systemd-r" cut from
/// "systemd-resolve" reads as a plausible name in its own right, and the row it identifies is what
/// Terminate and Kill act on.
///
/// @param textWidthPx   Measured width of the cell's text.
/// @param availWidthPx  Width left in the cell from the cursor to its right edge.
[[nodiscard]] inline bool isCellTextClipped(float textWidthPx, float availWidthPx) noexcept
{
    if (!std::isfinite(textWidthPx) || !std::isfinite(availWidthPx))
    {
        return false;
    }
    return textWidthPx > (availWidthPx + CLIP_TOLERANCE_PX);
}

/// Where a decimal-aligned cell's number and unit go (#1201), in pixels from the cell's left edge.
struct UnitAlignedCellLayout
{
    float numberX = 0.0F;   ///< Left edge of the number ("512.0")
    float unitX = 0.0F;     ///< Left edge of the unit (" MiB"): the start of the column's unit slot
    float itemWidth = 0.0F; ///< From the number's left edge to the unit's right edge
    bool fits = false;      ///< False when the number and the unit slot don't fit: draw the text clipped instead
};

/// Lays out a cell of a mixed-unit column ("512.0 B", "1.5 KiB", "3.2 MiB") so the decimal points
/// line up: the unit sits in a slot as wide as the column's widest unit at the cell's right edge,
/// and the number is right-aligned against that slot. Every number has one decimal digit, and
/// digits are tabular (UI/TabularDigits.h), so equal-width fractions put every decimal point at the
/// same x.
///
/// @param numberWidthPx    Measured width of the number, "512.0".
/// @param unitWidthPx      Measured width of this cell's unit, " MiB".
/// @param unitSlotWidthPx  Width of the widest unit the column can show; a wider unit widens the slot.
/// @param availWidthPx     Width left in the cell from the cursor to its right edge.
[[nodiscard]] inline UnitAlignedCellLayout
layoutUnitAlignedCell(float numberWidthPx, float unitWidthPx, float unitSlotWidthPx, float availWidthPx) noexcept
{
    if (!std::isfinite(numberWidthPx) || !std::isfinite(unitWidthPx) || !std::isfinite(unitSlotWidthPx) || !std::isfinite(availWidthPx))
    {
        return {};
    }
    const float number = std::max(numberWidthPx, 0.0F);
    const float unit = std::max(unitWidthPx, 0.0F);
    const float slot = std::max(unitSlotWidthPx, unit);
    if (isCellTextClipped(number + slot, availWidthPx))
    {
        return {};
    }
    const float unitX = std::max(availWidthPx - slot, number);
    return {.numberX = unitX - number, .unitX = unitX, .itemWidth = number + unit, .fits = true};
}

/// Width of the filter box above the table, in ems: 200px at the reference em (32/3 px), which is
/// the fixed pixel width it replaces, so the box is unchanged at the reference configuration.
inline constexpr float FILTER_WIDTH_EM = 18.75F;

/// Largest share of the toolbar row the filter box may take. The row also carries the process
/// summary and the view toggle, right-aligned; the box must not grow into them on a narrow pane.
inline constexpr float FILTER_MAX_ROW_FRACTION = 0.5F;

/// Width of the "Filter by name..." box (#965).
///
/// It was a fixed 200px. At Even Huger on a 175% display that is under five and a half ems: the
/// hint was cut to "Filter by nam" and the box held nine typed characters. The box is now a number
/// of ems, never narrower than its own hint, and never more than half the row.
///
/// @param hintWidthPx     Measured width of the hint text.
/// @param framePaddingPx  Horizontal frame padding on one side (ImGuiStyle::FramePadding.x).
/// @param emPx            One em, i.e. ImGui::GetFontSize().
/// @param rowWidthPx      Width of the toolbar row; non-positive or non-finite means "unknown".
[[nodiscard]] inline float computeFilterWidth(float hintWidthPx, float framePaddingPx, float emPx, float rowWidthPx) noexcept
{
    const float em = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;
    const float hint = (std::isfinite(hintWidthPx) && hintWidthPx > 0.0F) ? hintWidthPx : 0.0F;
    const float padding = (std::isfinite(framePaddingPx) && framePaddingPx > 0.0F) ? framePaddingPx : 0.0F;

    const float forHint = hint + (padding * 2.0F);
    const float wanted = (FILTER_WIDTH_EM * em > forHint) ? (FILTER_WIDTH_EM * em) : forHint;
    if (!std::isfinite(rowWidthPx) || rowWidthPx <= 0.0F)
    {
        return wanted;
    }

    const float cap = rowWidthPx * FILTER_MAX_ROW_FRACTION;
    return (wanted < cap) ? wanted : cap;
}

/// Narrowest toolbar row on which the filter box and the controls after it do not overlap (#1207).
///
/// The filter box takes computeFilterWidth(): its wanted width, capped at half the row. So the row
/// fits once it holds the wanted box and the rest side by side, or once half of it holds the rest
/// (the box then shrinks to the other half) -- whichever is narrower -- and never so narrow that
/// half of it cannot show the box's hint.
///
/// @param filterWantedPx   computeFilterWidth() with the row unknown: the box uncapped.
/// @param filterForHintPx  The hint plus its frame padding: the box's own floor.
/// @param restPx           Everything after the box on that row, spacing included.
[[nodiscard]] inline float computeToolbarMinimumWidth(float filterWantedPx, float filterForHintPx, float restPx) noexcept
{
    const auto atLeastZero = [](float value)
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const float rest = atLeastZero(restPx);
    const float wanted = atLeastZero(filterWantedPx);
    const float sideBySide = wanted + rest;
    const float halfForRest = rest * 2.0F;
    const float fits = (sideBySide < halfForRest) ? sideBySide : halfForRest;
    const float forHint = atLeastZero(filterForHintPx) * 2.0F;
    return (fits > forHint) ? fits : forHint;
}

} // namespace App::ProcessTableLayout
