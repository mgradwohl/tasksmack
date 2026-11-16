#include "App/DialogGeometry.h"
#include "App/Panels/ProcessDetailsLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace App
{
namespace
{

using ProcessDetailsLayout::computeInfoBlockWidth;
using ProcessDetailsLayout::INFO_BLOCK_MAX_WIDTH_EM;

// The #925 case: a wide window. The block stops at its cap instead of taking half the pane, so the
// label and its right-aligned value stay within a readable distance of each other.
TEST(ProcessDetailsLayoutTest, WideWindowCapsTheBlock)
{
    // Half of a 2827px window, as measured in the issue.
    const float width = computeInfoBlockWidth(REFERENCE_EM_PX, 1400.0F, 200.0F);
    EXPECT_FLOAT_EQ(width, INFO_BLOCK_MAX_WIDTH_EM * REFERENCE_EM_PX);
    EXPECT_FLOAT_EQ(width, 384.0F);
}

// Widening the window further changes nothing: the layout no longer degrades with more space.
TEST(ProcessDetailsLayoutTest, BlockWidthStopsGrowingWithTheWindow)
{
    const float atWide = computeInfoBlockWidth(REFERENCE_EM_PX, 1400.0F, 200.0F);
    const float atWider = computeInfoBlockWidth(REFERENCE_EM_PX, 4000.0F, 200.0F);
    EXPECT_FLOAT_EQ(atWide, atWider);
}

// A narrow window is unchanged from before: the block takes the half it is given.
TEST(ProcessDetailsLayoutTest, NarrowWindowUsesTheSpaceAvailable)
{
    EXPECT_FLOAT_EQ(computeInfoBlockWidth(REFERENCE_EM_PX, 300.0F, 200.0F), 300.0F);
}

TEST(ProcessDetailsLayoutTest, CapScalesWithTheFont)
{
    EXPECT_FLOAT_EQ(computeInfoBlockWidth(REFERENCE_EM_PX * 2.0F, 4000.0F, 200.0F), 768.0F);
}

// An unusually long value (a long process name or publisher) widens the block rather than being
// clipped while there is room to show it.
TEST(ProcessDetailsLayoutTest, LongContentWidensTheBlockPastTheCap)
{
    EXPECT_FLOAT_EQ(computeInfoBlockWidth(REFERENCE_EM_PX, 1400.0F, 600.0F), 600.0F);
}

TEST(ProcessDetailsLayoutTest, LongContentNeverExceedsTheSpaceAvailable)
{
    EXPECT_FLOAT_EQ(computeInfoBlockWidth(REFERENCE_EM_PX, 500.0F, 900.0F), 500.0F);
}

TEST(ProcessDetailsLayoutTest, SurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Unusable available width: fall back to the capped width.
    EXPECT_FLOAT_EQ(computeInfoBlockWidth(REFERENCE_EM_PX, 0.0F, 200.0F), 384.0F);
    EXPECT_FLOAT_EQ(computeInfoBlockWidth(REFERENCE_EM_PX, nan, 200.0F), 384.0F);
    EXPECT_FLOAT_EQ(computeInfoBlockWidth(REFERENCE_EM_PX, inf, 200.0F), 384.0F);

    // Unusable content width: ignored.
    EXPECT_FLOAT_EQ(computeInfoBlockWidth(REFERENCE_EM_PX, 1400.0F, nan), 384.0F);
    EXPECT_FLOAT_EQ(computeInfoBlockWidth(REFERENCE_EM_PX, 1400.0F, -5.0F), 384.0F);

    // Unusable em: still a finite, positive width within the space available.
    for (const float em : {0.0F, -8.0F, nan, inf})
    {
        const float width = computeInfoBlockWidth(em, 1400.0F, 200.0F);
        EXPECT_GT(width, 0.0F);
        EXPECT_LE(width, 1400.0F);
    }
}

// ========== Section cards (#1537) ==========

using ProcessDetailsLayout::computeInfoCardHeight;

// A card holds its header line above its rows, inside its padding top and bottom.
TEST(ProcessDetailsLayoutTest, InfoCardIsItsHeaderAndRowsPadded)
{
    EXPECT_FLOAT_EQ(computeInfoCardHeight(6.0F, 17.0F, 8.0F), (7.0F * 17.0F) + 16.0F);
    EXPECT_FLOAT_EQ(computeInfoCardHeight(0.0F, 17.0F, 8.0F), 17.0F + 16.0F);
}

TEST(ProcessDetailsLayoutTest, InfoCardSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FLOAT_EQ(computeInfoCardHeight(nan, 17.0F, 8.0F), 17.0F + 16.0F);
    EXPECT_FLOAT_EQ(computeInfoCardHeight(-3.0F, 17.0F, -1.0F), 17.0F);
    EXPECT_FLOAT_EQ(computeInfoCardHeight(inf, inf, inf), 0.0F);
}

// ========== Process-control buttons (#949, #1493) ==========

using ProcessDetailsLayout::computeActionButtonRowWidth;
using ProcessDetailsLayout::computeActionButtonWidth;

// The #1493 look: each button as wide as the widest label shown, padded, not a 180px floor or half the
// block -- so Terminate and Kill sit side by side at their natural width.
TEST(ProcessDetailsLayoutTest, ActionButtonIsItsWidestLabelPadded)
{
    EXPECT_FLOAT_EQ(computeActionButtonWidth(70.0F, 8.0F), 86.0F);
    // It tracks the font through the label and the padding.
    EXPECT_FLOAT_EQ(computeActionButtonWidth(140.0F, 16.0F), 172.0F);
}

TEST(ProcessDetailsLayoutTest, ActionButtonWidthIsWholePixels)
{
    const float width = computeActionButtonWidth(90.37F, 4.5F);
    EXPECT_FLOAT_EQ(width, std::ceil(width));
    EXPECT_GE(width, 90.37F + 9.0F);
}

TEST(ProcessDetailsLayoutTest, ActionButtonRowIsTheButtonsAndTheGapsBetweenThem)
{
    EXPECT_FLOAT_EQ(computeActionButtonRowWidth(86.0F, 2, 8.0F), 180.0F); // Terminate and Kill (Windows)
    EXPECT_FLOAT_EQ(computeActionButtonRowWidth(86.0F, 4, 8.0F), 368.0F); // and Suspend and Resume (Linux)
    EXPECT_FLOAT_EQ(computeActionButtonRowWidth(86.0F, 1, 8.0F), 86.0F);
    EXPECT_FLOAT_EQ(computeActionButtonRowWidth(86.0F, 0, 8.0F), 0.0F); // No action: no row
}

TEST(ProcessDetailsLayoutTest, ActionButtonsSurviveDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FLOAT_EQ(computeActionButtonWidth(nan, 8.0F), 16.0F);
    EXPECT_FLOAT_EQ(computeActionButtonWidth(70.0F, -1.0F), 70.0F);
    EXPECT_FLOAT_EQ(computeActionButtonWidth(inf, inf), 0.0F);
    EXPECT_FLOAT_EQ(computeActionButtonRowWidth(nan, 2, nan), 0.0F);
}

TEST(ProcessDetailsLayoutTest, TraceButtonCountsOnlyWhenWiderThanTheRow)
{
    // The trace button (#182) follows the row when there is room and starts its own row when not, so
    // the buttons need the wider of the two, never their sum.
    EXPECT_FLOAT_EQ(ProcessDetailsLayout::computeActionButtonsWidth(368.0F, 0.0F), 368.0F);   // Hidden (Windows)
    EXPECT_FLOAT_EQ(ProcessDetailsLayout::computeActionButtonsWidth(368.0F, 210.0F), 368.0F); // Linux: under the row
    EXPECT_FLOAT_EQ(ProcessDetailsLayout::computeActionButtonsWidth(180.0F, 210.0F), 210.0F); // wider than a short row
    EXPECT_FLOAT_EQ(ProcessDetailsLayout::computeActionButtonsWidth(0.0F, 210.0F), 210.0F);   // the only button
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(ProcessDetailsLayout::computeActionButtonsWidth(nan, -5.0F), 0.0F);
    EXPECT_FLOAT_EQ(ProcessDetailsLayout::computeActionButtonsWidth(368.0F, std::numeric_limits<float>::infinity()), 368.0F);
}

// ========== Selected-process identity (#927) ==========

using ProcessDetailsLayout::snapshotIsSelectedProcess;

TEST(ProcessDetailsLayoutTest, SamePidAndStartTimeIsTheSelectedProcess)
{
    EXPECT_TRUE(snapshotIsSelectedProcess(
        /*selectedPid=*/4242, /*selectedStartTicks=*/0xABCDU, /*snapshotPid=*/4242, /*snapshotStartTicks=*/0xABCDU));
}

// The reviewed defect: the PID has been reused. Same number, different process -- it must not be
// taken for the one the user selected, or an exited process's pane and its Terminate/Kill buttons
// come back aimed at something else.
TEST(ProcessDetailsLayoutTest, ReusedPidWithDifferentStartTimeIsNotTheSelectedProcess)
{
    EXPECT_FALSE(snapshotIsSelectedProcess(4242, 0xABCDU, 4242, 0x1234U));
}

TEST(ProcessDetailsLayoutTest, DifferentPidIsNeverTheSelectedProcess)
{
    EXPECT_FALSE(snapshotIsSelectedProcess(4242, 0xABCDU, 4243, 0xABCDU));
    EXPECT_FALSE(snapshotIsSelectedProcess(4242, 0, 4243, 0));
}

// The identity is compared exactly (#1503): an unknown (0) start time is not a wildcard, so a reused
// PID whose start time could or could not be read is still a different process. A process whose
// start time is unreadable on both sides is still itself.
TEST(ProcessDetailsLayoutTest, StartTimeIsComparedExactly)
{
    EXPECT_FALSE(snapshotIsSelectedProcess(4242, 0, 4242, 0x1234U));
    EXPECT_FALSE(snapshotIsSelectedProcess(4242, 0xABCDU, 4242, 0));
    EXPECT_TRUE(snapshotIsSelectedProcess(4242, 0, 4242, 0));
}

// ========== Confirm Action dialog (#971 review) ==========

using ProcessDetailsLayout::computeConfirmButtonWidth;
using ProcessDetailsLayout::computeConfirmContentBudget;

// The budget is the viewport, less the margin every dialog keeps, less the dialog's own padding.
TEST(ProcessDetailsLayoutTest, ConfirmBudgetIsTheViewportLessMarginAndPadding)
{
    EXPECT_FLOAT_EQ(computeConfirmContentBudget(/*viewportWidthPx=*/1000.0F, /*viewportFraction=*/0.9F, /*dialogPaddingPx=*/20.0F), 860.0F);
}

// The reviewed case: Even Huger on a 175% display, where each button wants 420px, in a window at
// its minimum width of 811px. Two buttons and their spacing (about 868px) do not fit; capped, the
// pair must fit the budget exactly.
TEST(ProcessDetailsLayoutTest, ConfirmButtonsFitSideBySideInAMinimumWidthWindow)
{
    const float padding = 28.0F;
    const float spacing = 28.0F;
    const float budget = computeConfirmContentBudget(811.0F, 0.9F, padding);
    const float width = computeConfirmButtonWidth(/*wantedWidthPx=*/420.0F, budget, spacing);

    EXPECT_LT(width, 420.0F);
    EXPECT_LE((width * 2.0F) + spacing, budget + 0.01F);
    EXPECT_LE((width * 2.0F) + spacing + (padding * 2.0F), 811.0F);
}

// With room to spare the buttons keep the width they asked for.
TEST(ProcessDetailsLayoutTest, ConfirmButtonsKeepTheirWidthWhenThereIsRoom)
{
    const float budget = computeConfirmContentBudget(2000.0F, 0.9F, 28.0F);
    EXPECT_FLOAT_EQ(computeConfirmButtonWidth(420.0F, budget, 28.0F), 420.0F);
    EXPECT_FLOAT_EQ(computeConfirmButtonWidth(120.0F, computeConfirmContentBudget(1280.0F, 0.9F, 8.0F), 8.0F), 120.0F);
}

// An unknown viewport means "no budget", and the buttons are left alone rather than collapsed.
TEST(ProcessDetailsLayoutTest, ConfirmButtonsAreUnboundedWithoutAViewport)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(computeConfirmContentBudget(0.0F, 0.9F, 8.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeConfirmContentBudget(nan, 0.9F, 8.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeConfirmContentBudget(1000.0F, 0.0F, 8.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeConfirmButtonWidth(420.0F, 0.0F, 28.0F), 420.0F);
    EXPECT_FLOAT_EQ(computeConfirmButtonWidth(420.0F, nan, 28.0F), 420.0F);
}

TEST(ProcessDetailsLayoutTest, ConfirmSizingSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(computeConfirmContentBudget(1000.0F, 5.0F, nan), 1000.0F); // fraction held to 1
    EXPECT_FLOAT_EQ(computeConfirmContentBudget(100.0F, 0.9F, 500.0F), 0.0F);  // padding exceeds the viewport
    EXPECT_FLOAT_EQ(computeConfirmButtonWidth(nan, 800.0F, 28.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeConfirmButtonWidth(420.0F, 20.0F, 28.0F), 0.0F); // budget smaller than the spacing
    EXPECT_FLOAT_EQ(computeConfirmButtonWidth(420.0F, 800.0F, nan), 400.0F);
}

// The Overview's cards flow (#4029): Identity, Runtime, Actions, Priority, I/O priority, left to right, each at
// its own width, wrapping to the next row when the next card doesn't fit. The Windows cards at the reference em:
// Identity and Runtime at their 384px cap, [Terminate] [Kill] about 200px and the five-stop class slider about
// 340px, each padded 16; the I/O priority card as wide as Priority.
using ProcessDetailsLayout::OverviewCard;
constexpr float INFO_CARD = 384.0F;
constexpr float ACTIONS_CARD = 216.0F;
constexpr float PRIORITY_CARD = 356.0F;
constexpr float SPACING = 8.0F;
constexpr std::array<float, ProcessDetailsLayout::OVERVIEW_CARD_COUNT> CARDS{
    INFO_CARD, INFO_CARD, ACTIONS_CARD, PRIORITY_CARD, PRIORITY_CARD};
// The width the first row needs for Identity, Runtime and Actions, with their gaps.
constexpr float THREE_CARDS = INFO_CARD + SPACING + INFO_CARD + SPACING + ACTIONS_CARD;
constexpr float ALL_CARDS = THREE_CARDS + SPACING + PRIORITY_CARD + SPACING + PRIORITY_CARD;

[[nodiscard]] ProcessDetailsLayout::OverviewLayout flowIn(float paneWidth,
                                                          const std::array<float, ProcessDetailsLayout::OVERVIEW_CARD_COUNT>& cards = CARDS)
{
    return ProcessDetailsLayout::layoutOverview(paneWidth, SPACING, cards);
}

// Wide enough: every card on the first row, left to right, each at its own width, not a share of the pane.
TEST(ProcessDetailsLayoutTest, EveryCardSharesTheFirstRowWhenTheyFit)
{
    const auto layout = flowIn(ALL_CARDS);
    for (const OverviewCard card :
         {OverviewCard::Identity, OverviewCard::Runtime, OverviewCard::Actions, OverviewCard::Priority, OverviewCard::IoPriority})
    {
        EXPECT_TRUE(layout.onFirstRow(card)) << static_cast<int>(card);
    }
    EXPECT_FALSE(layout[OverviewCard::Identity].sameLine);
    EXPECT_TRUE(layout[OverviewCard::IoPriority].sameLine);
    EXPECT_FLOAT_EQ(layout[OverviewCard::Actions].width, ACTIONS_CARD);
    EXPECT_FLOAT_EQ(flowIn(4000.0F)[OverviewCard::Priority].width, PRIORITY_CARD); // more room changes nothing
}

// The issue's case (#4029): Actions fits beside Runtime, so it goes there; Priority and I/O priority wrap to the
// next row, side by side, each card on its own -- not Actions and a Priority column moved as one.
TEST(ProcessDetailsLayoutTest, ActionsTakesTheFirstRowWhenItFitsAndTheRestWrapSideBySide)
{
    const auto layout = flowIn(THREE_CARDS);
    EXPECT_TRUE(layout.onFirstRow(OverviewCard::Actions));
    EXPECT_EQ(layout[OverviewCard::Priority].row, 1U);
    EXPECT_FALSE(layout[OverviewCard::Priority].sameLine) << "Priority starts the second row";
    EXPECT_TRUE(layout.sameRow(OverviewCard::Priority, OverviewCard::IoPriority));
    EXPECT_TRUE(layout[OverviewCard::IoPriority].sameLine) << "I/O priority to the right of Priority, not under it";

    // A pixel less, and Actions wraps too, with Priority and I/O priority after it on that row.
    const auto wrapped = flowIn(THREE_CARDS - 1.0F);
    EXPECT_FALSE(wrapped.onFirstRow(OverviewCard::Actions));
    EXPECT_TRUE(wrapped.sameRow(OverviewCard::Actions, OverviewCard::Priority));
    EXPECT_TRUE(wrapped.sameRow(OverviewCard::Priority, OverviewCard::IoPriority));
}

// Each card wraps on its own: Priority can share the first row with Actions while I/O priority wraps alone.
TEST(ProcessDetailsLayoutTest, EachCardWrapsOnItsOwn)
{
    const auto layout = flowIn(THREE_CARDS + SPACING + PRIORITY_CARD);
    EXPECT_TRUE(layout.onFirstRow(OverviewCard::Priority));
    EXPECT_EQ(layout[OverviewCard::IoPriority].row, 1U);
    EXPECT_FALSE(layout[OverviewCard::IoPriority].sameLine);
}

// Narrow: one card a row, each held to the pane, so nothing in it runs out of reach to the right.
TEST(ProcessDetailsLayoutTest, ANarrowPaneHasOneCardARowHeldToIt)
{
    const auto layout = flowIn(300.0F);
    for (std::size_t i = 0; i < ProcessDetailsLayout::OVERVIEW_CARD_COUNT; ++i)
    {
        EXPECT_EQ(layout.slots.at(i).row, i);
        EXPECT_FALSE(layout.slots.at(i).sameLine);
    }
    EXPECT_FLOAT_EQ(layout[OverviewCard::Identity].width, 300.0F);
    EXPECT_FLOAT_EQ(layout[OverviewCard::Actions].width, ACTIONS_CARD);
}

// The rule is in the widths measured at the font in force: a larger font widens every card, and the same pane
// that held three cards on the first row holds fewer.
TEST(ProcessDetailsLayoutTest, TheFlowFollowsTheFontScale)
{
    constexpr float SCALE = 1.5F;
    std::array<float, ProcessDetailsLayout::OVERVIEW_CARD_COUNT> scaled{};
    std::ranges::transform(CARDS, scaled.begin(), [](float width) { return width * SCALE; });
    EXPECT_TRUE(flowIn(THREE_CARDS).onFirstRow(OverviewCard::Actions));
    EXPECT_FALSE(ProcessDetailsLayout::layoutOverview(THREE_CARDS, SPACING * SCALE, scaled).onFirstRow(OverviewCard::Actions));
}

// A card the platform doesn't have (no priority control, no buttons) isn't drawn and takes no gap.
TEST(ProcessDetailsLayoutTest, AnAbsentCardTakesNoRoom)
{
    const auto noActions =
        flowIn(INFO_CARD + SPACING + INFO_CARD + SPACING + PRIORITY_CARD, {INFO_CARD, INFO_CARD, 0.0F, PRIORITY_CARD, 0.0F});
    EXPECT_FALSE(noActions.drawn(OverviewCard::Actions));
    EXPECT_FALSE(noActions.drawn(OverviewCard::IoPriority));
    EXPECT_TRUE(noActions.onFirstRow(OverviewCard::Priority)) << "Priority takes Actions' place, with no gap for it";
    EXPECT_TRUE(noActions[OverviewCard::Priority].sameLine);
}

// An unknown pane width (no frame yet) puts every card on one row; negative or non-finite measurements count as
// zero (the card isn't drawn) rather than producing a NaN width.
TEST(ProcessDetailsLayoutTest, TheFlowSurvivesDegenerateInput)
{
    const auto unknown = flowIn(std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE(unknown.onFirstRow(OverviewCard::IoPriority));
    EXPECT_FLOAT_EQ(unknown[OverviewCard::Priority].width, PRIORITY_CARD);
    EXPECT_TRUE(flowIn(0.0F).onFirstRow(OverviewCard::IoPriority));

    const auto garbage = ProcessDetailsLayout::layoutOverview(
        1000.0F, -5.0F, {std::numeric_limits<float>::infinity(), -1.0F, std::numeric_limits<float>::quiet_NaN(), 100.0F, 0.0F});
    EXPECT_FALSE(garbage.drawn(OverviewCard::Identity));
    EXPECT_FALSE(garbage.drawn(OverviewCard::Runtime));
    EXPECT_FALSE(garbage.drawn(OverviewCard::Actions));
    EXPECT_FLOAT_EQ(garbage[OverviewCard::Priority].width, 100.0F);
    EXPECT_FALSE(garbage[OverviewCard::Priority].sameLine) << "the first card drawn starts the row";
}

// Every row is as tall as its tallest card (#4029); a card not drawn has no height, and an unmeasured one (0)
// takes its row's.
TEST(ProcessDetailsLayoutTest, EachRowIsAsTallAsItsTallestCard)
{
    const auto layout = flowIn(THREE_CARDS); // Identity, Runtime, Actions | Priority, I/O priority
    const auto heights = ProcessDetailsLayout::rowHeights(layout, {200.0F, 200.0F, 260.0F, 180.0F, 0.0F});
    EXPECT_FLOAT_EQ(heights.at(0), 260.0F);
    EXPECT_FLOAT_EQ(heights.at(1), 260.0F);
    EXPECT_FLOAT_EQ(heights.at(2), 260.0F) << "Actions, the tallest, sets the first row";
    EXPECT_FLOAT_EQ(heights.at(3), 180.0F);
    EXPECT_FLOAT_EQ(heights.at(4), 180.0F) << "I/O priority, not yet measured, takes its row's height";

    const auto noIo = flowIn(THREE_CARDS, {INFO_CARD, INFO_CARD, ACTIONS_CARD, PRIORITY_CARD, 0.0F});
    EXPECT_FLOAT_EQ(ProcessDetailsLayout::rowHeights(noIo, {200.0F, 200.0F, 100.0F, 180.0F, 500.0F}).at(4), 0.0F)
        << "a card not drawn has no height, and doesn't stretch its row";
    EXPECT_FLOAT_EQ(ProcessDetailsLayout::rowHeights(noIo, {200.0F, 200.0F, 100.0F, 180.0F, 500.0F}).at(3), 180.0F);
}

} // namespace
} // namespace App
