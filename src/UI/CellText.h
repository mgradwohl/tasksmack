#pragma once

// A table cell's text that ends in an ellipsis when its column is too narrow, with the whole of it as
// the cell's tooltip (#914, #1920). A hard-clipped value is indistinguishable from one that is
// genuinely that short: "systemd-r" cut from "systemd-resolve", or a hex address missing its last
// digit, reads as a plausible value in its own right. The fit decision is pure and ImGui-free, so it
// is unit-testable on its own; cellText() draws it.
//
// A value told apart by its end -- a network endpoint's port -- is cut in the middle instead
// (#1942): middleEllipsize() fits it, keeping its tail, and cellTextKeepingTail() draws it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

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

/// The ellipsis middleEllipsize() puts in place of what it cuts: U+2026 HORIZONTAL ELLIPSIS.
inline constexpr std::string_view MIDDLE_ELLIPSIS = "\xE2\x80\xA6";

/// @p text cut short in the middle to fit @p budgetPx: as much of its start as fits, an ellipsis, and
/// its end, never fewer than its last @p tailBytes -- the part that tells one value from another, an
/// endpoint's "]:50747" (#1942). What room there is beyond that tail is shared between the start and
/// the rest of the end, a character at a time, so "[2605:59ca:2160:bf10:6129:8032:850a:4a31]:50747"
/// reads as "[2605:59c…850a:4a31]:50747". Text that fits is returned whole.
///
/// The tail is given up last, from its front, only when not even "…<tail>" fits: what is left is
/// still its end ("…50747"), and below that just the ellipsis. Cuts are made only at UTF-8
/// character boundaries.
///
/// @param measure   The pixel width of a string, e.g. ImGui::CalcTextSize(...).x; widths are taken
///                  to grow with the text, as a font's advances do.
/// @param ellipsis  What marks the cut: MIDDLE_ELLIPSIS, or the font's own ellipsis character.
template<typename Measure>
[[nodiscard]] std::string middleEllipsize(
    std::string_view text, std::size_t tailBytes, float budgetPx, const Measure& measure, std::string_view ellipsis = MIDDLE_ELLIPSIS)
{
    if (!isCellTextClipped(measure(text), budgetPx))
    {
        return std::string(text);
    }

    // Where each character starts, and the end: cuts go only there.
    std::vector<std::size_t> starts;
    starts.reserve(text.size() + 1);
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if ((static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U)
        {
            starts.push_back(i);
        }
    }
    starts.push_back(text.size());
    const std::size_t characters = starts.size() - 1;

    // The kept tail: whole characters, at least tailBytes of them.
    const std::size_t tailFrom = text.size() - std::min(tailBytes, text.size());
    std::size_t tailCharacters = 0;
    while (tailCharacters < characters && starts[characters - tailCharacters - 1] >= tailFrom)
    {
        ++tailCharacters;
    }
    if (tailCharacters == 0 && tailFrom < text.size())
    {
        tailCharacters = 1; // the tail starts inside a character: keep that character whole
    }

    std::string candidate;
    candidate.reserve(text.size() + ellipsis.size());
    // The first @p head characters, the ellipsis, and the last @p tail.
    const auto build = [&](std::size_t head, std::size_t tail) -> std::string_view
    {
        candidate.assign(text.substr(0, starts[head])).append(ellipsis).append(text.substr(starts[characters - tail]));
        return candidate;
    };
    const auto fits = [&](std::string_view shown)
    {
        return !isCellTextClipped(measure(shown), budgetPx);
    };
    // The largest n in [0, limit] for which fitsWith(n) holds, given fitsWith(0) does and widths grow with n.
    const auto largest = [](std::size_t limit, const auto& fitsWith)
    {
        std::size_t lo = 0;
        std::size_t hi = limit;
        while (lo < hi)
        {
            const std::size_t mid = lo + ((hi - lo + 1) / 2);
            if (fitsWith(mid))
            {
                lo = mid;
            }
            else
            {
                hi = mid - 1;
            }
        }
        return lo;
    };

    if (!fits(build(0, tailCharacters)))
    {
        // Not even the ellipsis and the whole tail: as much of the tail's end as fits after it.
        const std::size_t kept = largest(tailCharacters, [&](std::size_t n) { return n == 0 || fits(build(0, n)); });
        static_cast<void>(build(0, kept));
        return candidate;
    }

    // The tail fits: share the characters before it between the start and the end, the start taking
    // the odd one. All of them would be the whole text, which does not fit.
    const std::size_t spare = characters - tailCharacters;
    const auto withSpare = [&](std::size_t n)
    {
        return build((n + 1) / 2, tailCharacters + (n / 2));
    };
    const std::size_t extra = largest(spare > 0 ? spare - 1 : 0, [&](std::size_t n) { return n == 0 || fits(withSpare(n)); });
    static_cast<void>(withSpare(extra));
    return candidate;
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
    /// Room kept at the cell's right for an item drawn after the text, which then still fits when the
    /// text is cut short: the tree view's child-count badge after a name (#838).
    float trailingReservePx = 0.0F;
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

/// cellText() for a value told apart by its end, such as a network endpoint's port (#1942): when it
/// does not fit, it is cut in the middle by middleEllipsize(), keeping at least its last @p tailBytes,
/// rather than at its end. The tooltip and layout are cellText()'s. Measures @p text first.
CellTextResult cellTextKeepingTail(std::string_view text, std::size_t tailBytes, const CellTextOptions& options = {});

} // namespace UI::Widgets
