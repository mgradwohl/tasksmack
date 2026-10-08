#pragma once

// Pure width arithmetic for the process details pane, extracted so it is unit-testable without a
// live ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a small
// header" pattern (as ProcessTableLayout.h and ProcessTreeIndent.h do).

#include <algorithm>
#include <cmath>
#include <cstddef>
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

/// Width of each process-control button (Terminate, Kill, Suspend, Resume): the widest label among
/// those shown, with the frame padding on both sides, so the buttons are equal and as narrow as their
/// labels allow (#1493). They were a 180px em floor apiece (#949), then stretched to half the Actions
/// block, which left wide empty buttons beside Identity and Runtime.
///
/// @param widestLabelPx    Widest label among the buttons shown, i.e. ImGui::CalcTextSize(label).x.
/// @param framePaddingXPx  ImGuiStyle::FramePadding.x.
[[nodiscard]] inline float computeActionButtonWidth(float widestLabelPx, float framePaddingXPx) noexcept
{
    const auto nonNegative = [](float value) noexcept
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    return std::ceil(nonNegative(widestLabelPx) + (2.0F * nonNegative(framePaddingXPx)));
}

/// Width of @p count buttons @p buttonWidthPx wide on one row, @p spacingPx apart.
[[nodiscard]] inline float computeActionButtonRowWidth(float buttonWidthPx, std::size_t count, float spacingPx) noexcept
{
    if (count == 0)
    {
        return 0.0F;
    }
    const auto nonNegative = [](float value) noexcept
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const auto buttons = static_cast<float>(count);
    return (buttons * nonNegative(buttonWidthPx)) + ((buttons - 1.0F) * nonNegative(spacingPx));
}

/// Width of the Linux nice-slider row in the Overview's Actions block, in ems: "High", a usable
/// gradient track and "Low", which the slider fits to the width it is given (#1493).
inline constexpr float PRIORITY_SLIDER_ROW_WIDTH_EM = 28.0F;

/// Where the Overview's Actions block goes (#1493).
struct ActionsBlockLayout
{
    /// A third block on the Identity/Runtime row; otherwise it wraps onto its own row below them.
    bool besideInfo = true;
    /// The block's width in pixels, padding included.
    float width = 0.0F;
};

/// Lays out the Overview's Actions block: one left-aligned stack of rows -- the process-control
/// buttons, then the priority row(s) -- as wide as its widest row. It goes beside Identity and
/// Runtime when it fits in what their row leaves, both across and down: beside them it may be no
/// taller than they are, so the charts keep their height, and a block that would have to scroll there
/// (Linux's nice slider with the I/O priority row) wraps below them instead, shown whole. Wrapped, it
/// is held to the pane.
///
/// @param paneWidthPx      Width of the pane the row is laid out in.
/// @param infoRowWidthPx   Width Identity and Runtime take, with the gap between them.
/// @param spacingPx        Gap between two blocks on the row (ImGuiStyle::ItemSpacing.x).
/// @param contentWidthPx   Width the block's widest row needs, with the block's padding.
/// @param contentHeightPx  Height the block's rows need, with its padding, as last drawn; 0 when not
///                         known yet, which does not keep it off the row.
/// @param rowHeightPx      Height of the Identity/Runtime children; 0 when not known.
/// @return Beside the row at its content width when the pane width is unknown.
[[nodiscard]] inline ActionsBlockLayout computeActionsBlockLayout(float paneWidthPx,
                                                                  float infoRowWidthPx,
                                                                  float spacingPx,
                                                                  float contentWidthPx,
                                                                  float contentHeightPx = 0.0F,
                                                                  float rowHeightPx = 0.0F) noexcept
{
    const auto nonNegative = [](float value) noexcept
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const float content = nonNegative(contentWidthPx);
    if (!std::isfinite(paneWidthPx) || paneWidthPx <= 0.0F)
    {
        return {.besideInfo = true, .width = content};
    }
    const float contentHeight = nonNegative(contentHeightPx);
    const float rowHeight = nonNegative(rowHeightPx);
    const bool fitsDown = contentHeight == 0.0F || rowHeight == 0.0F || contentHeight <= rowHeight;
    if (fitsDown && nonNegative(infoRowWidthPx) + nonNegative(spacingPx) + content <= paneWidthPx)
    {
        return {.besideInfo = true, .width = content};
    }
    return {.besideInfo = false, .width = std::min(content, paneWidthPx)};
}

/// Whether a snapshot is of the process that was selected, and not merely of its PID (#927).
///
/// PIDs are reused. A process is selected by PID together with its start time, and a snapshot
/// carrying the same PID but another start time is a different process that has been handed the old
/// one's number. Accepting it would bring an exited process's pane back to life showing -- and
/// offering to Terminate or Kill -- something the user never selected.
///
/// Both are compared exactly, never the uniqueKey hash of the two (#1503): a hash collision must not
/// make another process the selected one. A start time the probe could not read is 0 on both sides
/// for the same process, so it still matches itself.
[[nodiscard]] constexpr bool snapshotIsSelectedProcess(std::int32_t selectedPid,
                                                       std::uint64_t selectedStartTicks,
                                                       std::int32_t snapshotPid,
                                                       std::uint64_t snapshotStartTicks) noexcept
{
    return (selectedPid == snapshotPid) && (selectedStartTicks == snapshotStartTicks);
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
