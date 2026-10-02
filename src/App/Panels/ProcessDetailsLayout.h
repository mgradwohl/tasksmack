#pragma once

// Pure width arithmetic for the process details pane, extracted so it is unit-testable without a
// live ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a small
// header" pattern (as ProcessTableLayout.h and ProcessTreeIndent.h do).

#include "UI/DialogMetrics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

/// Whether a snapshot is of the process that was selected, and not merely of its PID (#927).
///
/// PIDs are reused. A process is selected by PID together with its unique key (a hash of the PID
/// and start time), and a snapshot carrying the same PID but a different key is a different process
/// that has been handed the old one's number. Accepting it would bring an exited process's pane
/// back to life showing -- and offering to Terminate or Kill -- something the user never selected.
///
/// A key of zero means "not known" on either side (older callers select by PID alone), and then
/// the PID is all there is to go on.
[[nodiscard]] constexpr bool
snapshotIsSelectedProcess(std::int32_t selectedPid, std::uint64_t selectedKey, std::int32_t snapshotPid, std::uint64_t snapshotKey) noexcept
{
    if (selectedPid != snapshotPid)
    {
        return false;
    }
    return (selectedKey == 0) || (snapshotKey == 0) || (selectedKey == snapshotKey);
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

/// Content width the Confirm Action dialog may use: the viewport, less a margin, less the dialog's
/// own padding.
///
/// The dialog auto-fits its content, so nothing else bounds it. Its question names the process and
/// its two buttons have a font-relative floor; at Even Huger on a scaled display either can be
/// wider than a small main window, and an auto-fitting popup wider than the window is simply
/// clipped -- for the one dialog that confirms Terminate and Kill.
///
/// @param viewportWidthPx   Width of the viewport the dialog is centred in.
/// @param viewportFraction  Largest share of the viewport the dialog may take (0..1].
/// @param dialogPaddingPx   The dialog's horizontal window padding, one side.
/// @return Width in pixels, or 0 when the viewport is unknown: "no budget", not "no room".
[[nodiscard]] inline float computeConfirmContentBudget(float viewportWidthPx, float viewportFraction, float dialogPaddingPx) noexcept
{
    if (!std::isfinite(viewportWidthPx) || viewportWidthPx <= 0.0F || !std::isfinite(viewportFraction) || viewportFraction <= 0.0F)
    {
        return 0.0F;
    }
    const float padding = (std::isfinite(dialogPaddingPx) && dialogPaddingPx > 0.0F) ? dialogPaddingPx : 0.0F;
    return std::max(0.0F, (viewportWidthPx * std::min(viewportFraction, 1.0F)) - (padding * 2.0F));
}

/// Width of each of the Confirm Action dialog's two buttons: the width they want, held to half the
/// dialog's content budget so the pair always fits side by side.
///
/// @param wantedWidthPx    Width each button asks for (its font-relative floor, or its label).
/// @param contentBudgetPx  From computeConfirmContentBudget(); non-positive means "unbounded".
/// @param spacingPx        Gap between the two buttons (ImGuiStyle::ItemSpacing.x).
[[nodiscard]] inline float computeConfirmButtonWidth(float wantedWidthPx, float contentBudgetPx, float spacingPx) noexcept
{
    const float wanted = (std::isfinite(wantedWidthPx) && wantedWidthPx > 0.0F) ? wantedWidthPx : 0.0F;
    if (!std::isfinite(contentBudgetPx) || contentBudgetPx <= 0.0F)
    {
        return wanted;
    }
    const float spacing = (std::isfinite(spacingPx) && spacingPx > 0.0F) ? spacingPx : 0.0F;
    return std::min(wanted, std::max(0.0F, (contentBudgetPx - spacing) * 0.5F));
}
} // namespace App::ProcessDetailsLayout
