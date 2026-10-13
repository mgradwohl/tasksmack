#include "App/DialogGeometry.h"
#include "App/Panels/ProcessDetailsLayout.h"

#include <gtest/gtest.h>

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

// ========== Exited process (#927) ==========

using ProcessDetailsLayout::selectedProcessHasExited;

// A process that was being shown and is now missing from the process list has exited.
TEST(ProcessDetailsLayoutTest, MissingAfterBeingSeenIsExited)
{
    EXPECT_TRUE(selectedProcessHasExited(/*hasSelection=*/true, /*hadSnapshot=*/true, /*snapshotPresent=*/false));
}

// Missing before any snapshot has arrived is the lookup still in progress, not an exit: the pane
// must not announce that a just-selected process has exited.
TEST(ProcessDetailsLayoutTest, MissingBeforeBeingSeenIsNotExited)
{
    EXPECT_FALSE(selectedProcessHasExited(true, false, false));
}

TEST(ProcessDetailsLayoutTest, PresentProcessIsNotExited)
{
    EXPECT_FALSE(selectedProcessHasExited(true, true, true));
    EXPECT_FALSE(selectedProcessHasExited(true, false, true));
}

TEST(ProcessDetailsLayoutTest, NoSelectionIsNeverExited)
{
    EXPECT_FALSE(selectedProcessHasExited(false, true, false));
    EXPECT_FALSE(selectedProcessHasExited(false, false, false));
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

// The Overview's top row (#1789): Identity, Runtime, then the Actions and Priority cards. The Windows
// cards at the reference em: [Terminate] [Kill] about 200px and the five-stop class slider about 340px,
// each padded 16.
constexpr float ACTIONS_CARD = 216.0F;
constexpr float PRIORITY_CARD = 356.0F;
constexpr float INFO_ROW = 776.0F; // Identity and Runtime at their 384px cap, 8px apart
constexpr float SPACING = 8.0F;
// The width at which all four fit: Identity + Runtime (with their gap) + gap + Actions + gap + Priority.
constexpr float ONE_ROW_WIDTH = INFO_ROW + SPACING + ACTIONS_CARD + SPACING + PRIORITY_CARD;

[[nodiscard]] ProcessDetailsLayout::TopRowLayout topRowIn(float paneWidth, float actions = ACTIONS_CARD, float priority = PRIORITY_CARD)
{
    return ProcessDetailsLayout::chooseTopRowLayout(paneWidth, INFO_ROW, SPACING, actions, priority);
}

// The issue's window: wide enough, so Actions and Priority sit to the right of Identity and Runtime,
// Priority to the right of Actions, each at its content width -- not a share of the pane.
TEST(ProcessDetailsLayoutTest, TopRowHoldsAllFourCardsWhenTheyFit)
{
    const auto layout = topRowIn(2000.0F);
    EXPECT_TRUE(layout.besideInfo);
    EXPECT_TRUE(layout.priorityBesideActions);
    EXPECT_FLOAT_EQ(layout.actionsWidth, ACTIONS_CARD);
    EXPECT_FLOAT_EQ(layout.priorityWidth, PRIORITY_CARD);
    EXPECT_FLOAT_EQ(topRowIn(4000.0F).priorityWidth, PRIORITY_CARD); // More room changes nothing
}

// Exactly enough room still counts as room; a pixel less wraps both cards to a row of their own, side
// by side while the pane holds the two.
TEST(ProcessDetailsLayoutTest, TopRowWrapsAtTheFirstPixelItWouldNotFit)
{
    EXPECT_TRUE(topRowIn(ONE_ROW_WIDTH).besideInfo);
    const auto wrapped = topRowIn(ONE_ROW_WIDTH - 1.0F);
    EXPECT_FALSE(wrapped.besideInfo);
    EXPECT_TRUE(wrapped.priorityBesideActions);
    EXPECT_FLOAT_EQ(wrapped.actionsWidth, ACTIONS_CARD);
    EXPECT_FLOAT_EQ(wrapped.priorityWidth, PRIORITY_CARD);
}

// Too narrow for Actions and Priority side by side: one under the other, each held to the pane, so
// nothing in them runs out of reach to the right.
TEST(ProcessDetailsLayoutTest, NarrowPaneStacksTheCardsHeldToIt)
{
    const float sideBySide = ACTIONS_CARD + SPACING + PRIORITY_CARD;
    EXPECT_TRUE(topRowIn(sideBySide).priorityBesideActions);
    const auto stacked = topRowIn(sideBySide - 1.0F);
    EXPECT_FALSE(stacked.besideInfo);
    EXPECT_FALSE(stacked.priorityBesideActions);
    EXPECT_FLOAT_EQ(stacked.actionsWidth, ACTIONS_CARD);
    EXPECT_FLOAT_EQ(stacked.priorityWidth, PRIORITY_CARD);

    const auto tiny = topRowIn(300.0F);
    EXPECT_FALSE(tiny.priorityBesideActions);
    EXPECT_FLOAT_EQ(tiny.actionsWidth, ACTIONS_CARD);
    EXPECT_FLOAT_EQ(tiny.priorityWidth, 300.0F);
}

// The rule is in the widths measured at the font in force: a larger font scale widens every card, and
// the same pane that held them on one row wraps them.
TEST(ProcessDetailsLayoutTest, TopRowFollowsTheFontScale)
{
    constexpr float SCALE = 1.5F;
    EXPECT_TRUE(topRowIn(ONE_ROW_WIDTH).besideInfo);
    const auto scaled = ProcessDetailsLayout::chooseTopRowLayout(
        ONE_ROW_WIDTH, INFO_ROW * SCALE, SPACING * SCALE, ACTIONS_CARD * SCALE, PRIORITY_CARD * SCALE);
    EXPECT_FALSE(scaled.besideInfo);
}

// A card the platform does not have (no priority control, or no buttons) is not drawn and takes no gap.
TEST(ProcessDetailsLayoutTest, AnAbsentCardTakesNoRoom)
{
    const float actionsOnly = INFO_ROW + SPACING + ACTIONS_CARD;
    const auto noPriority = topRowIn(actionsOnly, ACTIONS_CARD, 0.0F);
    EXPECT_TRUE(noPriority.besideInfo);
    EXPECT_FLOAT_EQ(noPriority.priorityWidth, 0.0F);
    EXPECT_FALSE(topRowIn(actionsOnly - 1.0F, ACTIONS_CARD, 0.0F).besideInfo);

    const auto noButtons = topRowIn(INFO_ROW + SPACING + PRIORITY_CARD, 0.0F, PRIORITY_CARD);
    EXPECT_TRUE(noButtons.besideInfo);
    EXPECT_FLOAT_EQ(noButtons.actionsWidth, 0.0F);
}

// An unknown pane width (no frame yet) is unconstrained, like the helpers above; negative or non-finite
// measurements count as zero rather than producing a NaN width.
TEST(ProcessDetailsLayoutTest, TopRowSurvivesDegenerateInput)
{
    const auto unknown = topRowIn(std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE(unknown.besideInfo);
    EXPECT_FLOAT_EQ(unknown.actionsWidth, ACTIONS_CARD);
    EXPECT_TRUE(topRowIn(0.0F).besideInfo);

    const auto garbage = ProcessDetailsLayout::chooseTopRowLayout(
        1000.0F, std::numeric_limits<float>::infinity(), -5.0F, std::numeric_limits<float>::quiet_NaN(), -1.0F);
    EXPECT_FALSE(std::isnan(garbage.actionsWidth));
    EXPECT_FLOAT_EQ(garbage.actionsWidth, 0.0F);
    EXPECT_FLOAT_EQ(garbage.priorityWidth, 0.0F);
}

// The cards on a row share the tallest one's height: a Priority card taller than Identity and Runtime
// makes the whole row taller rather than wrapping (#1789), and an unknown height (0, not drawn yet)
// counts for nothing.
TEST(ProcessDetailsLayoutTest, RowCardHeightIsTheTallestCards)
{
    using ProcessDetailsLayout::computeRowCardHeight;
    EXPECT_FLOAT_EQ(computeRowCardHeight(136.0F, 80.0F, 170.0F), 170.0F);
    EXPECT_FLOAT_EQ(computeRowCardHeight(136.0F, 80.0F, 0.0F), 136.0F);
    EXPECT_FLOAT_EQ(computeRowCardHeight(0.0F, 0.0F, 0.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeRowCardHeight(std::numeric_limits<float>::quiet_NaN(), -4.0F, 90.0F), 90.0F);
}

// A card's rows are centred in the room under its header (#1874): half the room they leave, in whole
// pixels, and nothing while their height is unknown or they fill the room.
TEST(ProcessDetailsLayoutTest, CentredRowsOffsetIsHalfTheRoomLeft)
{
    using ProcessDetailsLayout::computeCentredRowsOffset;
    EXPECT_FLOAT_EQ(computeCentredRowsOffset(120.0F, 40.0F), 40.0F);
    EXPECT_FLOAT_EQ(computeCentredRowsOffset(121.0F, 40.0F), 40.0F); // Floored to a whole pixel
    EXPECT_FLOAT_EQ(computeCentredRowsOffset(40.0F, 40.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeCentredRowsOffset(30.0F, 40.0F), 0.0F); // Rows taller than the room
    EXPECT_FLOAT_EQ(computeCentredRowsOffset(120.0F, 0.0F), 0.0F); // Not measured yet
    EXPECT_FLOAT_EQ(computeCentredRowsOffset(std::numeric_limits<float>::quiet_NaN(), 40.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeCentredRowsOffset(120.0F, std::numeric_limits<float>::infinity()), 0.0F);
    EXPECT_FLOAT_EQ(computeCentredRowsOffset(-5.0F, 40.0F), 0.0F);
}

} // namespace
} // namespace App
