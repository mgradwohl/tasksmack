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

/// Height of one of the Overview's section cards (#1537) holding its header line and @p rows rows of
/// text: one text line each (ImGui::GetTextLineHeightWithSpacing()), inside the card's window padding
/// top and bottom. Identity and Runtime both take the taller of their two heights, and the cards
/// beside them share the row's height (computeRowCardHeight()), so all their edges line up.
///
/// @param rows                 The card's rows, not counting the header.
/// @param lineHeightPx         ImGui::GetTextLineHeightWithSpacing().
/// @param windowPaddingYPx     ImGuiStyle::WindowPadding.y, one side.
[[nodiscard]] inline float computeInfoCardHeight(float rows, float lineHeightPx, float windowPaddingYPx) noexcept
{
    const auto nonNegative = [](float value) noexcept
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    return ((nonNegative(rows) + 1.0F) * nonNegative(lineHeightPx)) + (2.0F * nonNegative(windowPaddingYPx));
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

/// Width the Actions block's buttons need: the process-control row @p actionRowPx, or the "Trace
/// system calls (strace)" button @p traceButtonPx (0 where it is hidden) when that is wider (#182). The
/// trace button follows the row when the block has room for both and starts a row of its own when it
/// does not, so the block never needs the two side by side.
[[nodiscard]] inline float computeActionButtonsWidth(float actionRowPx, float traceButtonPx) noexcept
{
    const auto nonNegative = [](float value) noexcept
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    return std::max(nonNegative(actionRowPx), nonNegative(traceButtonPx));
}

/// Width of the Linux nice-slider row in the Overview's Priority card, in ems: "High", a usable
/// gradient track and "Low", which the slider fits to the width it is given (#1493).
inline constexpr float PRIORITY_SLIDER_ROW_WIDTH_EM = 28.0F;

/// Where the Overview's Actions and Priority cards go (#1789).
struct TopRowLayout
{
    /// Both cards are on the Identity/Runtime row, to the right of Runtime; otherwise they wrap to a
    /// row of their own below it.
    bool besideInfo = true;
    /// Priority is to the right of Actions; otherwise it is under it. Always so beside Identity and
    /// Runtime, and on the wrapped row whenever the pane is wide enough for the two side by side.
    bool priorityBesideActions = true;
    /// The Actions card's width in pixels, padding included; 0 when it is not drawn.
    float actionsWidth = 0.0F;
    /// The Priority card's width in pixels, padding included; 0 when it is not drawn.
    float priorityWidth = 0.0F;
};

/// Lays out the Overview's top row (#1789): Identity and Runtime, then the Actions card (the
/// process-control buttons) and the Priority card (the priority row(s)), each at its content width.
///
/// The rule is width alone, at the font scale in force (every width passed in is measured at it):
/// the four cards share one row when the pane is at least
///   Identity + Runtime + Actions + Priority + 3 x ItemSpacing.x
/// wide (@p infoRowWidthPx already counts the gap between Identity and Runtime). Otherwise Actions
/// and Priority wrap to a row below Identity and Runtime -- side by side when the pane fits
/// Actions + ItemSpacing.x + Priority, else one under the other, each held to the pane -- so nothing
/// is clipped or overlaps. A card of width 0 is not drawn and takes no gap.
///
/// The cards on a row share the tallest one's height (computeRowCardHeight()): a card taller than
/// Identity and Runtime makes the row taller, rather than being kept off it as the single Actions
/// block once was (#1493).
///
/// @param paneWidthPx       Width of the pane the row is laid out in.
/// @param infoRowWidthPx    Width Identity and Runtime take, with the gap between them.
/// @param spacingPx         Gap between two cards on a row (ImGuiStyle::ItemSpacing.x).
/// @param actionsWidthPx    The Actions card's content width, padded; 0 when it is not drawn.
/// @param priorityWidthPx   The Priority card's content width, padded; 0 when it is not drawn.
/// @return Everything on the top row at its content width when the pane width is unknown.
[[nodiscard]] inline TopRowLayout
chooseTopRowLayout(float paneWidthPx, float infoRowWidthPx, float spacingPx, float actionsWidthPx, float priorityWidthPx) noexcept
{
    const auto nonNegative = [](float value) noexcept
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const float actions = nonNegative(actionsWidthPx);
    const float priority = nonNegative(priorityWidthPx);
    const float spacing = nonNegative(spacingPx);
    const float between = (actions > 0.0F && priority > 0.0F) ? spacing : 0.0F;
    const float controls = actions + between + priority;
    const TopRowLayout atContentWidth{
        .besideInfo = true, .priorityBesideActions = true, .actionsWidth = actions, .priorityWidth = priority};
    if (!std::isfinite(paneWidthPx) || paneWidthPx <= 0.0F)
    {
        return atContentWidth;
    }
    const float lead = controls > 0.0F ? spacing : 0.0F;
    if (nonNegative(infoRowWidthPx) + lead + controls <= paneWidthPx)
    {
        return atContentWidth;
    }
    if (controls <= paneWidthPx)
    {
        return {.besideInfo = false, .priorityBesideActions = true, .actionsWidth = actions, .priorityWidth = priority};
    }
    return {.besideInfo = false,
            .priorityBesideActions = false,
            .actionsWidth = std::min(actions, paneWidthPx),
            .priorityWidth = std::min(priority, paneWidthPx)};
}

/// Height every card on one row of the Overview shares (#1789): the tallest of @p baseHeightPx (the
/// Identity/Runtime card height on the top row, 0 on the wrapped row) and the Actions and Priority
/// cards' content heights as last drawn (0 when not drawn yet, or not on this row). 0 when nothing is
/// known: each card then sizes itself to its content.
[[nodiscard]] inline float computeRowCardHeight(float baseHeightPx, float actionsHeightPx, float priorityHeightPx) noexcept
{
    const auto nonNegative = [](float value) noexcept
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    return std::max({nonNegative(baseHeightPx), nonNegative(actionsHeightPx), nonNegative(priorityHeightPx)});
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
