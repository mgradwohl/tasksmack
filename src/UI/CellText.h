#pragma once

// A table cell's text that ends in an ellipsis when its column is too narrow, with the whole of it as
// the cell's tooltip (#914, #1920). A hard-clipped value is indistinguishable from one that is
// genuinely that short: "systemd-r" cut from "systemd-resolve", or a hex address missing its last
// digit, reads as a plausible value in its own right. The fit decision is pure and ImGui-free, so it
// is unit-testable on its own; cellText() draws it.

#include <algorithm>
#include <cmath>
#include <string_view>

namespace UI::Widgets
{

/// Slack allowed before a cell's text counts as clipped, in pixels.
///
/// Text widths and column widths are both fractional and arrive by different arithmetic, so text
/// that exactly fills its cell can measure a hair wider than the space. Without this, such a cell
/// would trade its last glyph for an ellipsis, or flicker between the two as the layout settles.
inline constexpr float CELL_CLIP_TOLERANCE_PX = 0.5F;

/// Tooltips for clipped cells wrap at this many ems, so a long command line reads as a paragraph
/// instead of one line running off the screen.
inline constexpr float CLIPPED_CELL_TOOLTIP_WRAP_EM = 60.0F;

/// Whether text of the given width does not fit in the cell space left for it.
///
/// @param textWidthPx   Measured width of the cell's text.
/// @param availWidthPx  Width left in the cell from the cursor to its right edge.
[[nodiscard]] inline bool isCellTextClipped(float textWidthPx, float availWidthPx) noexcept
{
    if (!std::isfinite(textWidthPx) || !std::isfinite(availWidthPx))
    {
        return false;
    }
    return textWidthPx > (availWidthPx + CELL_CLIP_TOLERANCE_PX);
}

/// Where a cell's text goes and how much of the cell it takes.
struct CellTextLayout
{
    float offsetX = 0.0F;   ///< From the cursor to the text's left edge (right alignment).
    float itemWidth = 0.0F; ///< The drawn (and hoverable) width: the text's, or the cell's when clipped.
    bool clipped = false;   ///< Drawn cut short with an ellipsis, with the whole text as its tooltip.
};

/// The layout of text @p textWidthPx wide in a cell with @p availWidthPx left. Text that fits is
/// drawn whole, at the left or flush right; text that does not fills the cell from its left edge in
/// either alignment, since the leading characters are the ones that identify the value.
[[nodiscard]] inline CellTextLayout cellTextLayout(float textWidthPx, float availWidthPx, bool rightAligned) noexcept
{
    if (isCellTextClipped(textWidthPx, availWidthPx))
    {
        return {.offsetX = 0.0F, .itemWidth = std::max(availWidthPx, 0.0F), .clipped = true};
    }
    const float offsetX =
        (rightAligned && std::isfinite(availWidthPx) && std::isfinite(textWidthPx)) ? std::max(0.0F, availWidthPx - textWidthPx) : 0.0F;
    return {.offsetX = offsetX, .itemWidth = textWidthPx, .clipped = false};
}

struct CellTextOptions
{
    /// Flush right in the cell while the text fits (numbers); a clipped cell starts at its left edge.
    bool rightAligned = false;
    /// Whether a clipped cell gets the full text as its tooltip; false for a cell whose caller draws
    /// its own (the Processes table's Services column, #1880, or a row's own tooltip).
    bool clippedTooltip = true;
    /// Whether a clipped cell reports its whole text's width to the layout, as ImGui::TextUnformatted()
    /// does, so a column's "Size to fit" still fits it; false reports only the cell's width (the
    /// Processes table, whose layout was tuned with that).
    bool layoutFullText = true;
};

/// What cellText() drew.
struct CellTextResult
{
    bool clipped = false;      ///< The text was cut short with an ellipsis.
    bool tooltipShown = false; ///< Its full-text tooltip is showing this frame.
};

/// Draws @p text (already measured as @p textWidthPx) in the current table cell: whole when it fits,
/// otherwise cut short with an ellipsis and, unless @p options say not to, with the whole text as the
/// cell's tooltip. Submits the item itself rather than calling ImGui::TextUnformatted(), so the text
/// is measured once. Defined in CellText.cpp.
CellTextResult cellText(std::string_view text, float textWidthPx, const CellTextOptions& options = {});

/// cellText() for text not yet measured: measures it in the current font first.
CellTextResult cellText(std::string_view text, const CellTextOptions& options = {});

} // namespace UI::Widgets
