#pragma once

// Pure width arithmetic for the process details pane, extracted so it is unit-testable without a
// live ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a small
// header" pattern (as ProcessTableLayout.h and ProcessTreeIndent.h do).

#include "UI/DialogMetrics.h"

#include <algorithm>
#include <cmath>

namespace App::ProcessDetailsLayout
{

/// Widest an Identity/Runtime block normally gets, in ems.
///
/// Each block is a label column and a right-aligned value column. They used to take half the window
/// each, so on a wide window a label and its value sat at opposite ends of ~1200px of nothing and
/// the eye had to carry across it (#925) -- a layout that got harder to read the more room it was
/// given. 36 em is 384px at the reference em: comfortably wider than any label plus a typical value
/// (a PID, a timestamp, "Above Normal (nice: -5)"), and narrow enough to read as one row.
inline constexpr float INFO_BLOCK_MAX_WIDTH_EM = 36.0F;

/// Width of one Identity/Runtime block.
///
/// The block is its capped width, widened only as far as an unusually long value actually needs
/// (a long process name or publisher), and never wider than the half of the pane it has to share.
///
/// @param emPx              One em, i.e. ImGui::GetFontSize().
/// @param availableWidthPx  The most the block may take: half the pane, less the gap between blocks.
/// @param contentWidthPx    Width the block's own content needs: label column, widest value, padding.
/// @return Width in pixels, at most availableWidthPx when that is a usable size.
[[nodiscard]] inline float computeInfoBlockWidth(float emPx, float availableWidthPx, float contentWidthPx) noexcept
{
    const float em = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;
    const float content = (std::isfinite(contentWidthPx) && contentWidthPx > 0.0F) ? contentWidthPx : 0.0F;

    const float wanted = std::max(INFO_BLOCK_MAX_WIDTH_EM * em, content);
    if (!std::isfinite(availableWidthPx) || availableWidthPx <= 0.0F)
    {
        return wanted;
    }
    return std::min(wanted, availableWidthPx);
}

/// Floor on the width of the process-control buttons (Terminate, Kill, Pause, Resume), in ems:
/// 180px at the reference em, the fixed width they had before (#949).
inline constexpr float ACTION_BUTTON_MIN_WIDTH_EM = 16.875F;

/// Gap between the two columns of process-control buttons, in ems: 8px at the reference em.
inline constexpr float ACTION_BUTTON_GUTTER_EM = 0.75F;

/// The process-control buttons are laid out two to a row.
inline constexpr float ACTION_BUTTON_COLUMNS = 2.0F;

/// Width shared by all four process-control buttons.
///
/// They were a fixed 180px, so they ignored the Font Size setting and the display's density: 22.5 em
/// at Small and 8.4 em at Even Huger, and half the width of the priority slider beneath them once
/// that began to scale (#938). The width is now the widest label with the dialogs' padding, never
/// below the em floor that reproduces 180px at the reference font, so the four stay equal and keep
/// their proportions.
///
/// It is also capped to the pane. The content area does not scroll horizontally, so a second column
/// that does not fit is clipped and its buttons cannot be reached -- Kill and Resume, on a narrow
/// window at a large font.
///
/// @param widestLabelPx       Widest of the four button labels, i.e. ImGui::CalcTextSize(label).x.
/// @param emPx                One em, i.e. ImGui::GetFontSize().
/// @param availableWidthPx    Width of the pane the two columns must fit in.
/// @param columnOverheadPx    Width each column takes beyond its button: the gutter, plus that
///                            column's share of the table's spacing between columns.
/// @return Width in pixels. At least one pixel, so a degenerate pane cannot produce a zero-sized
///         button; never wider than half the pane allows when that is a usable size.
[[nodiscard]] inline float
computeActionButtonWidth(float widestLabelPx, float emPx, float availableWidthPx, float columnOverheadPx) noexcept
{
    const float wanted = UI::DialogMetrics::computeActionButtonWidth(widestLabelPx, emPx, ACTION_BUTTON_MIN_WIDTH_EM);
    if (!std::isfinite(availableWidthPx) || availableWidthPx <= 0.0F)
    {
        return wanted;
    }

    const float overhead = (std::isfinite(columnOverheadPx) && columnOverheadPx > 0.0F) ? columnOverheadPx : 0.0F;
    const float perColumn = (availableWidthPx / ACTION_BUTTON_COLUMNS) - overhead;
    return std::max(1.0F, std::floor(std::min(wanted, perColumn)));
}

} // namespace App::ProcessDetailsLayout
