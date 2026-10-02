#pragma once

// Pure width arithmetic for the process details pane, extracted so it is unit-testable without a
// live ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a small
// header" pattern (as ProcessTableLayout.h and ProcessTreeIndent.h do).

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

/// Whether the selected process has just gone missing and should now be shown as exited (#927).
///
/// The pane is handed the selected process's snapshot every frame, or nothing when the process is
/// not in the current process list. Nothing arriving before any snapshot has been seen is the
/// lookup still in progress; nothing arriving *after* one has been seen is the process exiting.
/// The pane used to treat both the same and kept drawing the last snapshot it had, so an exited
/// process went on looking alive -- with its Terminate and Kill buttons still aimed at a PID the
/// system is free to hand to something else.
///
/// @param hasSelection      A process is selected.
/// @param hadSnapshot       A snapshot of it has been received since it was selected.
/// @param snapshotPresent   A snapshot of it was provided this frame.
[[nodiscard]] constexpr bool selectedProcessHasExited(bool hasSelection, bool hadSnapshot, bool snapshotPresent) noexcept
{
    return hasSelection && hadSnapshot && !snapshotPresent;
}

} // namespace App::ProcessDetailsLayout
