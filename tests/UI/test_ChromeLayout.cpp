/// @file test_ChromeLayout.cpp
/// @brief Tests for the section-header text, section gap and dialog-footer placement in
/// UI/ChromeLayout.h (#1200).

#include "UI/ChromeLayout.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace UI::ChromeLayout
{
namespace
{

// ========== Section header text ==========

TEST(ChromeLayoutTest, HeaderLabelIsIconAndTitleOnly)
{
    std::array<char, 64> buffer{};
    EXPECT_EQ(composeHeaderLabel(buffer, "*", "CPU Usage"), "*  CPU Usage");
    EXPECT_EQ(composeHeaderLabel(buffer, {}, "Interface Status"), "Interface Status");
}

// The headers used to read "CPU Usage (300 samples)": the count was part of the title, at its
// weight. The label is built from the icon and title alone, so no header can show one.
TEST(ChromeLayoutTest, HeaderLabelNeverCarriesASampleCount)
{
    std::array<char, 128> buffer{};
    for (const std::string_view title : {"CPU Usage", "Memory & Swap", "Network Throughput", "Disk I/O History", "GPU Core & Video"})
    {
        const std::string_view label = composeHeaderLabel(buffer, "*", title);
        EXPECT_FALSE(label.contains("sample")) << label;
        EXPECT_FALSE(label.contains('(')) << label;
    }
}

TEST(ChromeLayoutTest, HeaderLabelIsCutOnACharacterBoundary)
{
    // "\xC3\xA9" is one character (e-acute); a 3-byte buffer holds "ab" and must not keep half of it.
    std::array<char, 3> buffer{};
    const std::string_view label = composeHeaderLabel(buffer, {}, "ab\xC3\xA9");
    EXPECT_EQ(label, "ab");
    // An icon glyph that does not fit is left out whole, with no gap after it.
    std::array<char, 2> tiny{};
    EXPECT_EQ(composeHeaderLabel(tiny, "\xEF\x8B\x9B", "X"), "X");
}

TEST(ChromeLayoutTest, SampleCountGoesInTheTooltipText)
{
    std::array<char, 64> buffer{};
    EXPECT_EQ(formatSampleCount(buffer, 300), "300 samples in this chart");
    EXPECT_EQ(formatSampleCount(buffer, 1), "1 sample in this chart");
    EXPECT_EQ(formatSampleCount(buffer, 0), "0 samples in this chart");
    std::array<char, 5> small{};
    EXPECT_EQ(formatSampleCount(small, 300), "300 s");
}

// ========== Section gap ==========

TEST(ChromeLayoutTest, SectionGapIsFourItemSpacingsCountingTheOneImGuiAdds)
{
    // Settings separated its sections with four Spacing() calls: four item spacings.
    EXPECT_FLOAT_EQ(sectionGapItemHeight(4.0F) + 4.0F, 16.0F);
    EXPECT_FLOAT_EQ(sectionGapItemHeight(7.0F) + 7.0F, 28.0F); // scales with the style
    EXPECT_FLOAT_EQ(sectionGapItemHeight(std::numeric_limits<float>::quiet_NaN()), 0.0F);
    EXPECT_FLOAT_EQ(sectionGapItemHeight(-3.0F), 0.0F);
}

// ========== Dialog footer ==========

DialogFooterInput twoActions(float avail, float preferred, float leading = 0.0F, float maxRow = 0.0F)
{
    return {.availWidth = avail,
            .maxRowWidth = maxRow,
            .spacing = 8.0F,
            .preferredButtonWidth = preferred,
            .actionCount = 2,
            .leadingWidth = leading,
            .scrollbarWidth = 0.0F};
}

// The primary action is the rightmost button, its right edge on the row's, with the secondary
// immediately to its left. The confirm dialog used to put its action first ([Kill][Cancel]),
// left-aligned, and About centred its OK.
TEST(ChromeLayoutTest, PrimaryActionIsRightmostAndRightAligned)
{
    const auto placement = placeDialogFooter(twoActions(400.0F, 100.0F));
    EXPECT_FLOAT_EQ(placement.rowWidth, 400.0F);
    EXPECT_FLOAT_EQ(placement.buttonWidth, 100.0F);
    EXPECT_FLOAT_EQ(placement.primaryX + placement.buttonWidth, 400.0F);
    EXPECT_FLOAT_EQ(placement.secondaryX + placement.buttonWidth + 8.0F, placement.primaryX);
    EXPECT_LT(placement.secondaryX, placement.primaryX);
}

TEST(ChromeLayoutTest, SingleActionIsRightAligned)
{
    DialogFooterInput input = twoActions(400.0F, 120.0F);
    input.actionCount = 1;
    const auto placement = placeDialogFooter(input);
    EXPECT_FLOAT_EQ(placement.primaryX, 280.0F); // not centred at 140
    EXPECT_FLOAT_EQ(placement.primaryX + placement.buttonWidth, 400.0F);
}

TEST(ChromeLayoutTest, ActionsShrinkEquallyRatherThanLeaveTheRow)
{
    const auto placement = placeDialogFooter(twoActions(150.0F, 100.0F));
    EXPECT_FLOAT_EQ(placement.buttonWidth, 71.0F); // (150 - 8) / 2
    EXPECT_FLOAT_EQ(placement.secondaryX, 0.0F);
    EXPECT_FLOAT_EQ(placement.primaryX + placement.buttonWidth, 150.0F);
}

TEST(ChromeLayoutTest, LeadingButtonSharesTheRowWhenAllFit)
{
    const auto placement = placeDialogFooter(twoActions(500.0F, 100.0F, 120.0F));
    EXPECT_FALSE(placement.leadingOnOwnRow);
    EXPECT_FLOAT_EQ(placement.buttonWidth, 100.0F);
    EXPECT_FLOAT_EQ(placement.primaryX + placement.buttonWidth, 500.0F);
    EXPECT_GE(placement.secondaryX, 128.0F); // right of the leading button and its gap
}

// Settings at the minimum window and the largest font (#1341 review): Reset to defaults takes a row
// of its own, and Cancel and Save keep their width on the row below.
TEST(ChromeLayoutTest, LeadingButtonTakesItsOwnRowWhenTheyDoNotFit)
{
    const auto placement = placeDialogFooter(twoActions(300.0F, 100.0F, 120.0F));
    EXPECT_TRUE(placement.leadingOnOwnRow);
    EXPECT_FLOAT_EQ(placement.buttonWidth, 100.0F);
    EXPECT_FLOAT_EQ(placement.primaryX + placement.buttonWidth, 300.0F);
}

// An auto-fitting dialog is only as wide as its content; a footer wider than a short question grows
// it, up to the budget, instead of squeezing its buttons to the question's width.
TEST(ChromeLayoutTest, RowMayGrowAnAutoFittingDialogUpToItsBudget)
{
    const auto grown = placeDialogFooter(twoActions(150.0F, 120.0F, 0.0F, 1000.0F));
    EXPECT_FLOAT_EQ(grown.rowWidth, 248.0F);
    EXPECT_FLOAT_EQ(grown.buttonWidth, 120.0F);

    const auto capped = placeDialogFooter(twoActions(150.0F, 120.0F, 0.0F, 200.0F));
    EXPECT_FLOAT_EQ(capped.rowWidth, 200.0F);
    EXPECT_FLOAT_EQ(capped.buttonWidth, 96.0F);
    EXPECT_FLOAT_EQ(capped.primaryX + capped.buttonWidth, 200.0F);
}

TEST(ChromeLayoutTest, FooterPlacementSurvivesBadInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto placement = placeDialogFooter({.availWidth = nan,
                                              .maxRowWidth = nan,
                                              .spacing = nan,
                                              .preferredButtonWidth = nan,
                                              .actionCount = 0,
                                              .leadingWidth = nan,
                                              .scrollbarWidth = nan});
    EXPECT_FLOAT_EQ(placement.rowWidth, 0.0F);
    EXPECT_FLOAT_EQ(placement.buttonWidth, 0.0F);
    EXPECT_FALSE(placement.leadingOnOwnRow);
}

// The Settings footer at each font preset, in a dialog exactly as wide as that footer's row (its
// combos are widened to it), with a vertical scrollbar taking its width from the content: Reset to
// defaults stays beside Cancel | Save, and the height reserved for the footer is the one-row height.
// A scrollbar that narrowed the row used to wrap Reset, which grew the footer by a row, overflowed
// the dialog and kept the scrollbar (#1200 review).
TEST(ChromeLayoutTest, SettingsFooterStaysOnOneRowWhateverTheScrollbarAtEveryPreset)
{
    constexpr float REFERENCE_EM = 32.0F / 3.0F; // Medium's 8pt at 96 DPI
    for (const float bodyPt : {7.0F, 8.0F, 10.0F, 12.0F, 14.0F, 16.0F})
    {
        for (const float displayScale : {1.0F, 1.5F, 2.0F})
        {
            const float em = bodyPt * (96.0F / 72.0F) * displayScale;
            const float styleScale = em / REFERENCE_EM;
            const float spacing = 8.0F * styleScale;
            const float scrollbar = 14.0F * styleScale;
            const float itemSpacingY = 4.0F * styleScale;
            const float frameHeight = em + (2.0F * 3.0F * styleScale);
            const float preferred = 9.375F * em;                          // SETTINGS_BUTTON_MIN_EM
            const float reset = (8.2F * em) + (2.0F * 4.0F * styleScale); // "Reset to defaults" + frame padding
            const float rowWidth = reset + (2.0F * preferred) + (2.0F * spacing);

            const DialogFooterInput input{.availWidth = rowWidth - scrollbar,
                                          .maxRowWidth = 0.0F,
                                          .spacing = spacing,
                                          .preferredButtonWidth = preferred,
                                          .actionCount = 2,
                                          .leadingWidth = reset,
                                          .scrollbarWidth = scrollbar};
            const auto layout = layoutDialogFooter(input, itemSpacingY, frameHeight);
            EXPECT_FALSE(layout.placement.leadingOnOwnRow) << bodyPt << "pt x" << displayScale;
            EXPECT_FLOAT_EQ(layout.height, dialogFooterHeight(itemSpacingY, frameHeight, false)) << bodyPt << "pt x" << displayScale;
            // The actions stay right of Reset and inside the row.
            EXPECT_GE(layout.placement.secondaryX, reset + spacing - 0.001F);
            EXPECT_LE(layout.placement.primaryX + layout.placement.buttonWidth, input.availWidth + 0.001F);

            // The same dialog without the scrollbar decides the same way.
            DialogFooterInput noScrollbar = input;
            noScrollbar.availWidth = rowWidth;
            noScrollbar.scrollbarWidth = 0.0F;
            EXPECT_EQ(placeDialogFooter(noScrollbar).leadingOnOwnRow, layout.placement.leadingOnOwnRow);

            // ImGui truncates content sizes to whole pixels (ImTrunc64 of the cursor extent), so a
            // dialog measured to be exactly as wide as the row is up to a pixel narrower than it.
            // That alone wrapped Reset in the captured dialog.
            DialogFooterInput truncated = noScrollbar;
            truncated.availWidth = std::floor(rowWidth) - 0.001F;
            EXPECT_FALSE(placeDialogFooter(truncated).leadingOnOwnRow) << bodyPt << "pt x" << displayScale;
        }
    }
}

// Where the three genuinely do not fit -- the minimum window at the largest font -- Reset takes its
// own row, and the reserved height includes that row.
TEST(ChromeLayoutTest, SettingsFooterWrapsWhereItMustAndReservesTheRow)
{
    const DialogFooterInput input{.availWidth = 300.0F,
                                  .maxRowWidth = 0.0F,
                                  .spacing = 8.0F,
                                  .preferredButtonWidth = 120.0F,
                                  .actionCount = 2,
                                  .leadingWidth = 110.0F,
                                  .scrollbarWidth = 14.0F};
    const auto layout = layoutDialogFooter(input, 4.0F, 20.0F);
    EXPECT_TRUE(layout.placement.leadingOnOwnRow);
    EXPECT_FLOAT_EQ(layout.height, dialogFooterHeight(4.0F, 20.0F, true));
    EXPECT_GT(layout.height, dialogFooterHeight(4.0F, 20.0F, false));
}

// ImGui truncates a constrained window's size; rounding up first keeps a fractional title bar from
// leaving the dialog a fraction short of its contents (#1461).
TEST(ChromeLayoutTest, DialogExtentRoundsUpToAWholePixel)
{
    EXPECT_FLOAT_EQ(wholePixelDialogExtent(279.375F), 280.0F);
    EXPECT_FLOAT_EQ(wholePixelDialogExtent(280.0F), 280.0F);
    EXPECT_FLOAT_EQ(wholePixelDialogExtent(0.25F), 1.0F);
}

TEST(ChromeLayoutTest, FooterHeightCountsSeparatorSpacingAndRows)
{
    EXPECT_FLOAT_EQ(dialogFooterHeight(4.0F, 20.0F, false), 33.0F);        // 3 x 4 + 1 + 20
    EXPECT_FLOAT_EQ(dialogFooterHeight(4.0F, 20.0F, true), 33.0F + 24.0F); // and a row with its spacing
    EXPECT_FLOAT_EQ(dialogFooterHeight(std::numeric_limits<float>::quiet_NaN(), -1.0F, false), 1.0F);
}

} // namespace
} // namespace UI::ChromeLayout
