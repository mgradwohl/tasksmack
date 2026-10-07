#pragma once

// Pure text and placement arithmetic behind the shared chrome widgets in UI/ChromeWidgets.h: section
// headers, the gap between sections and the dialog footer (#1200). Extracted so it is unit-testable
// without a live ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a
// small header" pattern (as DialogMetrics.h and LineLayout.h do).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <string_view>

namespace UI::ChromeLayout
{

// ---------------------------------------------------------------------------------------------------
// Section headers
// ---------------------------------------------------------------------------------------------------

/// Space between a section header's icon and its title, as the headers have always had it.
inline constexpr std::string_view HEADER_ICON_GAP = "  ";

/// "<icon>  <title>" in @p out, cut at the buffer's end (on a UTF-8 boundary) if it does not fit.
///
/// A header names its section and nothing else: a sample count or a state goes in the header's
/// tooltip or in its quieter detail text, never in this label (#1200). The headers used to append
/// "(300 samples)" here, a debug counter at the same weight as the title.
///
/// @return The label, a view into @p out.
[[nodiscard]] inline std::string_view composeHeaderLabel(std::span<char> out, std::string_view icon, std::string_view title) noexcept
{
    std::size_t length = 0;
    const auto append = [&](std::string_view part)
    {
        const std::size_t room = out.size() - length;
        std::size_t take = std::min(part.size(), room);
        // Never end part-way through a UTF-8 sequence: back up to the lead byte the cut splits.
        if (take < part.size())
        {
            while (take > 0 && (static_cast<unsigned char>(part[take]) & 0xC0U) == 0x80U)
            {
                --take;
            }
        }
        std::copy_n(part.begin(), take, out.begin() + static_cast<std::ptrdiff_t>(length));
        length += take;
        return take == part.size();
    };
    if (!icon.empty())
    {
        if (append(icon))
        {
            (void) append(HEADER_ICON_GAP);
        }
    }
    (void) append(title);
    return {out.data(), length};
}

/// "300 samples" (or "1 sample"): how much history a chart is drawn from, for its header's tooltip.
///
/// @return The text, a view into @p out; cut at the buffer's end if it does not fit.
[[nodiscard]] inline std::string_view formatSampleCount(std::span<char> out, std::size_t samples)
{
    const auto result = (samples == 1)
                          ? std::format_to_n(out.data(), static_cast<std::ptrdiff_t>(out.size()), "1 sample in this chart")
                          : std::format_to_n(out.data(), static_cast<std::ptrdiff_t>(out.size()), "{} samples in this chart", samples);
    const auto written = static_cast<std::size_t>(std::min<std::ptrdiff_t>(result.size, static_cast<std::ptrdiff_t>(out.size())));
    return {out.data(), written};
}

// ---------------------------------------------------------------------------------------------------
// Vertical rhythm
// ---------------------------------------------------------------------------------------------------

/// The gap between two sections, in item spacings: the gap sectionGap() leaves, counting the item
/// spacing ImGui adds after its own item. Settings separated its sections with four Spacing() calls
/// and the elevation notice with two; one value now stands for "a new section starts here".
inline constexpr float SECTION_GAP_ITEM_SPACINGS = 4.0F;

/// Height of the empty item sectionGap() submits, so that with the item spacing ImGui adds after it
/// the gap is SECTION_GAP_ITEM_SPACINGS item spacings. Scales with the style, so with the font.
[[nodiscard]] inline float sectionGapItemHeight(float itemSpacingY) noexcept
{
    const float spacing = (std::isfinite(itemSpacingY) && itemSpacingY > 0.0F) ? itemSpacingY : 0.0F;
    return spacing * (SECTION_GAP_ITEM_SPACINGS - 1.0F);
}

// ---------------------------------------------------------------------------------------------------
// Dialog footer
// ---------------------------------------------------------------------------------------------------

/// What a dialog footer's button row has to hold.
struct DialogFooterInput
{
    float availWidth = 0.0F;           ///< Content width the row has now (GetContentRegionAvail().x).
    float maxRowWidth = 0.0F;          ///< Most the row may grow an auto-fitting dialog to; <= 0: availWidth.
    float spacing = 0.0F;              ///< Gap between buttons (ItemSpacing.x).
    float preferredButtonWidth = 0.0F; ///< Each action button's width, if the row has room.
    std::size_t actionCount = 1;       ///< 1 (the primary action) or 2 (a secondary, then the primary).
    float leadingWidth = 0.0F;         ///< A left-aligned extra button (Settings' Reset); 0: none.
};

/// Where the footer's buttons go. X positions are relative to the row's start.
struct DialogFooterPlacement
{
    float rowWidth = 0.0F;        ///< Width of the row the actions are right-aligned in.
    float buttonWidth = 0.0F;     ///< Width of each action button.
    float secondaryX = 0.0F;      ///< Left edge of the secondary action (Cancel); meaningful with two actions.
    float primaryX = 0.0F;        ///< Left edge of the primary action, the rightmost button.
    bool leadingOnOwnRow = false; ///< The leading button takes a row of its own above the actions.
};

/// Lay out a dialog's footer: [leading]  ...  [secondary][primary], the primary action always the
/// rightmost button and its right edge on the row's right edge (#1200).
///
/// Every dialog used to place its buttons its own way: About centred OK, the elevation notice
/// right-aligned it, Settings right-aligned Cancel|Save, and the process-action confirm put its
/// action first and Cancel second, left-aligned.
///
/// The action buttons keep their preferred width while it fits and shrink equally when it does not,
/// so neither is pushed off the row (#1129). An auto-fitting dialog is only as wide as its widest
/// row, so a row wider than the content may grow the dialog up to @p maxRowWidth; past that the
/// buttons shrink. The leading button keeps its width and moves to its own row when all of them do
/// not fit on one (#1341 review).
[[nodiscard]] inline DialogFooterPlacement placeDialogFooter(const DialogFooterInput& input) noexcept
{
    const auto clean = [](float value)
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const float avail = clean(input.availWidth);
    const float spacing = clean(input.spacing);
    const float preferred = clean(input.preferredButtonWidth);
    const float leading = clean(input.leadingWidth);
    const float count = (input.actionCount >= 2) ? 2.0F : 1.0F;

    const float actionsPreferred = (preferred * count) + (spacing * (count - 1.0F));
    const float leadingPart = (leading > 0.0F) ? (leading + spacing) : 0.0F;
    const float wanted = leadingPart + actionsPreferred;
    const float cap = clean(input.maxRowWidth) > 0.0F ? clean(input.maxRowWidth) : avail;
    const float rowWidth = std::max(avail, std::min(wanted, cap));

    const bool leadingOnOwnRow = (leading > 0.0F) && (wanted > rowWidth);
    const float leadingShare = (leading > 0.0F && !leadingOnOwnRow) ? leadingPart : 0.0F;
    const float actionsAvail = std::max(0.0F, rowWidth - leadingShare);
    const float buttonWidth = std::min(preferred, std::max(0.0F, (actionsAvail - (spacing * (count - 1.0F))) / count));
    const float actionsWidth = (buttonWidth * count) + (spacing * (count - 1.0F));

    const float firstX = std::max(leadingShare, rowWidth - actionsWidth);
    const float primaryX = firstX + ((count - 1.0F) * (buttonWidth + spacing));
    return {
        .rowWidth = rowWidth, .buttonWidth = buttonWidth, .secondaryX = firstX, .primaryX = primaryX, .leadingOnOwnRow = leadingOnOwnRow};
}

/// Height of a dialog footer below the item before it: a spacing, the separator, a spacing and the
/// button row -- plus a row for the leading button when it has one of its own. A dialog that pins its
/// footer below a scrolling body reserves this much for it (#1129).
///
/// @param itemSpacingY  ImGuiStyle::ItemSpacing.y.
/// @param frameHeight   ImGui::GetFrameHeight(), one button row.
[[nodiscard]] inline float dialogFooterHeight(float itemSpacingY, float frameHeight, bool leadingOnOwnRow) noexcept
{
    const float spacing = (std::isfinite(itemSpacingY) && itemSpacingY > 0.0F) ? itemSpacingY : 0.0F;
    const float frame = (std::isfinite(frameHeight) && frameHeight > 0.0F) ? frameHeight : 0.0F;
    // Spacing(): one item spacing. Separator(): one pixel and an item spacing. Spacing() again. Then
    // the buttons; the leading button's own row, when it has one, adds a row and its item spacing.
    constexpr float SEPARATOR_PX = 1.0F;
    return (spacing * 3.0F) + SEPARATOR_PX + frame + (leadingOnOwnRow ? (frame + spacing) : 0.0F);
}

} // namespace UI::ChromeLayout
