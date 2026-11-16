#pragma once

// Pure width arithmetic for the process details pane, extracted so it is unit-testable without a
// live ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a small
// header" pattern (as ProcessTableLayout.h and ProcessTreeIndent.h do).

#include <algorithm>
#include <array>
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
/// top and bottom. Identity and Runtime both take the taller of their two heights, so their edges
/// line up; the Actions, Priority and I/O priority cards beside them are each as tall as their own
/// content (#3741).
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

/// The Overview's cards, in the order they flow (#4029).
enum class OverviewCard : std::uint8_t
{
    Identity,
    Runtime,
    Actions,
    Priority,
    IoPriority,
};
inline constexpr std::size_t OVERVIEW_CARD_COUNT = 5;

/// Where one Overview card goes (#4029).
struct OverviewSlot
{
    std::size_t row = 0;   ///< Its row, from 0. A card that isn't drawn keeps the row of the card before it.
    bool sameLine = false; ///< Drawn to the right of the drawn card before it (ImGui::SameLine()); else it starts a row.
    float width = 0.0F;    ///< Its width in pixels, padding included, held to the pane; 0 when it isn't drawn.
};

/// Where every Overview card goes, in OverviewCard order (#4029).
struct OverviewLayout
{
    std::array<OverviewSlot, OVERVIEW_CARD_COUNT> slots{};

    [[nodiscard]] const OverviewSlot& operator[](OverviewCard card) const
    {
        return slots.at(static_cast<std::size_t>(card));
    }

    [[nodiscard]] bool drawn(OverviewCard card) const
    {
        return (*this)[card].width > 0.0F;
    }

    /// Whether @p card is drawn on the first row, beside Identity (and Runtime).
    [[nodiscard]] bool onFirstRow(OverviewCard card) const
    {
        return drawn(card) && (*this)[card].row == 0;
    }

    /// Whether @p a and @p b are both drawn, on the same row.
    [[nodiscard]] bool sameRow(OverviewCard a, OverviewCard b) const
    {
        return drawn(a) && drawn(b) && (*this)[a].row == (*this)[b].row;
    }
};

/// Lays out the Overview's cards (#4029; #1789, #3741 before it): Identity, Runtime, Actions, Priority and
/// I/O priority, in that order, left to right, each at its own width (@p widthsPx, padding included; 0 for a
/// card that isn't drawn, which takes no gap), wrapping to the next row whenever the next card doesn't fit
/// beside the last: a card goes on the current row when the row's width so far, the gap (@p spacingPx,
/// ImGuiStyle::ItemSpacing.x) and its own width fit @p paneWidthPx. A card wider than the pane starts a row
/// of its own and is held to the pane, so nothing is clipped or overlaps. The rule is width alone, at the
/// font scale in force (every width passed in is measured at it). An unknown pane width (not finite, or not
/// above 0) puts every card on one row at its own width.
[[nodiscard]] inline OverviewLayout
layoutOverview(float paneWidthPx, float spacingPx, const std::array<float, OVERVIEW_CARD_COUNT>& widthsPx)
{
    const auto nonNegative = [](float value) noexcept
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const float spacing = nonNegative(spacingPx);
    const bool paneKnown = std::isfinite(paneWidthPx) && paneWidthPx > 0.0F;
    OverviewLayout layout;
    std::size_t row = 0;
    float rowWidth = 0.0F; // 0: nothing on this row yet
    for (std::size_t i = 0; i < OVERVIEW_CARD_COUNT; ++i)
    {
        OverviewSlot& slot = layout.slots.at(i);
        const float wanted = nonNegative(widthsPx.at(i));
        if (wanted <= 0.0F)
        {
            slot.row = row;
            continue;
        }
        const float width = paneKnown ? std::min(wanted, paneWidthPx) : wanted;
        if (rowWidth > 0.0F && (!paneKnown || rowWidth + spacing + width <= paneWidthPx))
        {
            slot = {.row = row, .sameLine = true, .width = width};
            rowWidth += spacing + width;
            continue;
        }
        if (rowWidth > 0.0F)
        {
            ++row;
        }
        slot = {.row = row, .sameLine = false, .width = width};
        rowWidth = width;
    }
    return layout;
}

/// Each drawn card's height on its row (#4029): every row as tall as its tallest card, so a row reads as one
/// band. @p naturalHeightsPx are the cards' own content heights (0 when not yet measured, or not drawn), in
/// OverviewCard order; the answer is each card's row's tallest, and 0 for a card not drawn.
[[nodiscard]] inline std::array<float, OVERVIEW_CARD_COUNT> rowHeights(const OverviewLayout& layout,
                                                                       const std::array<float, OVERVIEW_CARD_COUNT>& naturalHeightsPx)
{
    std::array<float, OVERVIEW_CARD_COUNT> tallest{}; // by row: at most one row per card
    for (std::size_t i = 0; i < OVERVIEW_CARD_COUNT; ++i)
    {
        const OverviewSlot& slot = layout.slots.at(i);
        const float natural = naturalHeightsPx.at(i);
        if (slot.width > 0.0F && std::isfinite(natural) && natural > 0.0F)
        {
            tallest.at(slot.row) = std::max(tallest.at(slot.row), natural);
        }
    }
    std::array<float, OVERVIEW_CARD_COUNT> heights{};
    for (std::size_t i = 0; i < OVERVIEW_CARD_COUNT; ++i)
    {
        const OverviewSlot& slot = layout.slots.at(i);
        heights.at(i) = slot.width > 0.0F ? tallest.at(slot.row) : 0.0F;
    }
    return heights;
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
