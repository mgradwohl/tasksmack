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

// ========== Selected-process identity (#927) ==========

using ProcessDetailsLayout::snapshotIsSelectedProcess;

TEST(ProcessDetailsLayoutTest, SamePidAndKeyIsTheSelectedProcess)
{
    EXPECT_TRUE(snapshotIsSelectedProcess(/*selectedPid=*/4242, /*selectedKey=*/0xABCDU, /*snapshotPid=*/4242, /*snapshotKey=*/0xABCDU));
}

// The reviewed defect: the PID has been reused. Same number, different process -- it must not be
// taken for the one the user selected, or an exited process's pane and its Terminate/Kill buttons
// come back aimed at something else.
TEST(ProcessDetailsLayoutTest, ReusedPidWithDifferentKeyIsNotTheSelectedProcess)
{
    EXPECT_FALSE(snapshotIsSelectedProcess(4242, 0xABCDU, 4242, 0x1234U));
}

TEST(ProcessDetailsLayoutTest, DifferentPidIsNeverTheSelectedProcess)
{
    EXPECT_FALSE(snapshotIsSelectedProcess(4242, 0xABCDU, 4243, 0xABCDU));
    EXPECT_FALSE(snapshotIsSelectedProcess(4242, 0, 4243, 0));
}

// A key of zero means "not known"; the PID is then all there is to compare.
TEST(ProcessDetailsLayoutTest, UnknownKeyFallsBackToPid)
{
    EXPECT_TRUE(snapshotIsSelectedProcess(4242, 0, 4242, 0x1234U));
    EXPECT_TRUE(snapshotIsSelectedProcess(4242, 0xABCDU, 4242, 0));
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
// The Overview's Actions block (#1493): one stack of rows, as wide as its widest row. The Windows
// block at the reference em: Priority [class] [Apply] current: Below Normal is about 430px, padded 16.
constexpr float ACTIONS_CONTENT = 446.0F;
constexpr float INFO_ROW = 776.0F; // Identity and Runtime at their 384px cap, 8px apart
constexpr float SPACING = 8.0F;

[[nodiscard]] ProcessDetailsLayout::ActionsBlockLayout actionsLayoutIn(float paneWidth, float content = ACTIONS_CONTENT)
{
    return ProcessDetailsLayout::computeActionsBlockLayout(paneWidth, INFO_ROW, SPACING, content);
}

// The issue's window: a wide pane leaves room to the right of Identity and Runtime, and the block takes
// its content's width there -- not a share of the pane.
TEST(ProcessDetailsLayoutTest, ActionsBlockSitsBesideIdentityAndRuntimeAtItsContentWidth)
{
    const auto layout = actionsLayoutIn(1900.0F);
    EXPECT_TRUE(layout.besideInfo);
    EXPECT_FLOAT_EQ(layout.width, ACTIONS_CONTENT);
    EXPECT_FLOAT_EQ(actionsLayoutIn(4000.0F).width, ACTIONS_CONTENT); // More room changes nothing
}

// Exactly enough room still counts as room; a pixel less wraps it, still at its content width.
TEST(ProcessDetailsLayoutTest, ActionsBlockWrapsAtTheFirstPixelItWouldNotFit)
{
    const float exact = INFO_ROW + SPACING + ACTIONS_CONTENT;
    EXPECT_TRUE(actionsLayoutIn(exact).besideInfo);
    const auto wrapped = actionsLayoutIn(exact - 1.0F);
    EXPECT_FALSE(wrapped.besideInfo);
    EXPECT_FLOAT_EQ(wrapped.width, ACTIONS_CONTENT);
}

// Wrapped in a pane narrower than its content, the block is held to the pane, so nothing in it runs
// out of reach to the right (the button row wraps inside it).
TEST(ProcessDetailsLayoutTest, ActionsBlockIsHeldToANarrowPane)
{
    const auto layout = actionsLayoutIn(300.0F);
    EXPECT_FALSE(layout.besideInfo);
    EXPECT_FLOAT_EQ(layout.width, 300.0F);
}

// Beside the row the block may be no taller than Identity/Runtime, so the charts keep their height; one
// that would have to scroll there (Linux's slider and I/O priority rows) wraps below, shown whole. An
// unknown height (before the first draw) does not keep it off the row.
TEST(ProcessDetailsLayoutTest, ActionsBlockTallerThanTheRowWrapsBelowIt)
{
    constexpr float ROW = 136.0F;
    EXPECT_TRUE(ProcessDetailsLayout::computeActionsBlockLayout(1900.0F, INFO_ROW, SPACING, ACTIONS_CONTENT, 64.0F, ROW).besideInfo);
    EXPECT_TRUE(ProcessDetailsLayout::computeActionsBlockLayout(1900.0F, INFO_ROW, SPACING, ACTIONS_CONTENT, ROW, ROW).besideInfo);
    const auto tall = ProcessDetailsLayout::computeActionsBlockLayout(1900.0F, INFO_ROW, SPACING, ACTIONS_CONTENT, ROW + 1.0F, ROW);
    EXPECT_FALSE(tall.besideInfo);
    EXPECT_FLOAT_EQ(tall.width, ACTIONS_CONTENT);
    EXPECT_TRUE(ProcessDetailsLayout::computeActionsBlockLayout(1900.0F, INFO_ROW, SPACING, ACTIONS_CONTENT, 0.0F, ROW).besideInfo);
    EXPECT_TRUE(ProcessDetailsLayout::computeActionsBlockLayout(1900.0F, INFO_ROW, SPACING, ACTIONS_CONTENT, 500.0F, 0.0F).besideInfo);
}

// An unknown pane width (no frame yet) is unconstrained, like the helpers above; negative or non-finite
// measurements count as zero rather than producing a NaN width.
TEST(ProcessDetailsLayoutTest, ActionsBlockSurvivesDegenerateInput)
{
    const auto unknown = actionsLayoutIn(std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE(unknown.besideInfo);
    EXPECT_FLOAT_EQ(unknown.width, ACTIONS_CONTENT);
    EXPECT_TRUE(actionsLayoutIn(0.0F).besideInfo);

    const auto garbage = ProcessDetailsLayout::computeActionsBlockLayout(
        1000.0F, std::numeric_limits<float>::infinity(), -5.0F, std::numeric_limits<float>::quiet_NaN());
    EXPECT_FALSE(std::isnan(garbage.width));
    EXPECT_FLOAT_EQ(garbage.width, 0.0F);
}

} // namespace
} // namespace App
