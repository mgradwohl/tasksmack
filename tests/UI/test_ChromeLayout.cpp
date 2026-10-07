/// @file test_ChromeLayout.cpp
/// @brief Tests for the section-header text, section gap and dialog-footer placement in
/// UI/ChromeLayout.h (#1200).

#include "UI/ChromeLayout.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
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
            .leadingWidth = leading};
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
    const auto placement = placeDialogFooter(
        {.availWidth = nan, .maxRowWidth = nan, .spacing = nan, .preferredButtonWidth = nan, .actionCount = 0, .leadingWidth = nan});
    EXPECT_FLOAT_EQ(placement.rowWidth, 0.0F);
    EXPECT_FLOAT_EQ(placement.buttonWidth, 0.0F);
    EXPECT_FALSE(placement.leadingOnOwnRow);
}

TEST(ChromeLayoutTest, FooterHeightCountsSeparatorSpacingAndRows)
{
    EXPECT_FLOAT_EQ(dialogFooterHeight(4.0F, 20.0F, false), 33.0F);        // 3 x 4 + 1 + 20
    EXPECT_FLOAT_EQ(dialogFooterHeight(4.0F, 20.0F, true), 33.0F + 24.0F); // and a row with its spacing
    EXPECT_FLOAT_EQ(dialogFooterHeight(std::numeric_limits<float>::quiet_NaN(), -1.0F, false), 1.0F);
}

} // namespace
} // namespace UI::ChromeLayout
