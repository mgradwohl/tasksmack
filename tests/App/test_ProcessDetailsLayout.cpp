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

// ========== Process-control button width (#949) ==========

using ProcessDetailsLayout::ACTION_BUTTON_MIN_WIDTH_EM;
using ProcessDetailsLayout::computeActionButtonWidth;

// The floor reproduces the old fixed 180px at the reference em, so the buttons are unchanged at
// the Medium preset on an unscaled display.
TEST(ProcessDetailsLayoutTest, ActionButtonIsUnchangedAtReferenceEm)
{
    // A label of ~90px (" Terminate" with its icon at Medium) is well under the floor.
    EXPECT_FLOAT_EQ(computeActionButtonWidth(90.0F, REFERENCE_EM_PX, 1256.0F, 16.0F), 180.0F);
    EXPECT_FLOAT_EQ(ACTION_BUTTON_MIN_WIDTH_EM * REFERENCE_EM_PX, 180.0F);
}

// The #949 defect: 180px at every font. The width now tracks the em.
TEST(ProcessDetailsLayoutTest, ActionButtonScalesWithTheFont)
{
    EXPECT_FLOAT_EQ(computeActionButtonWidth(180.0F, REFERENCE_EM_PX * 2.0F, 2800.0F, 32.0F), 360.0F);
    EXPECT_FLOAT_EQ(computeActionButtonWidth(68.0F, 8.0F, 2800.0F, 12.0F), 135.0F);
}

// A label wider than the floor allows (a longer translation, say) widens all four buttons rather
// than being clipped.
TEST(ProcessDetailsLayoutTest, ActionButtonGrowsForAWideLabel)
{
    // 200px label + 1 em padding each side at a 10px em = 220px, above the 168.75px floor.
    EXPECT_FLOAT_EQ(computeActionButtonWidth(200.0F, 10.0F, 2800.0F, 16.0F), 220.0F);
}

// The content area does not scroll horizontally, so both columns must fit the pane: at Even Huger
// in a 330px window the second column (Kill, Resume) was clipped out of reach.
TEST(ProcessDetailsLayoutTest, ActionButtonsBothFitANarrowPane)
{
    const float pane = 306.0F;
    const float overhead = 32.0F;
    const float width = computeActionButtonWidth(180.0F, REFERENCE_EM_PX * 2.0F, pane, overhead);

    EXPECT_LE(2.0F * (width + overhead), pane);
    EXPECT_FLOAT_EQ(width, 121.0F);
}

TEST(ProcessDetailsLayoutTest, ActionButtonWidthIsWholePixels)
{
    const float width = computeActionButtonWidth(90.0F, 13.37F, 501.5F, 17.25F);
    EXPECT_FLOAT_EQ(width, std::floor(width));
}

TEST(ProcessDetailsLayoutTest, ActionButtonNeverCollapsesToNothing)
{
    EXPECT_GE(computeActionButtonWidth(90.0F, REFERENCE_EM_PX, 10.0F, 16.0F), 1.0F);
    EXPECT_GE(computeActionButtonWidth(90.0F, REFERENCE_EM_PX, 1.0F, 500.0F), 1.0F);
}

TEST(ProcessDetailsLayoutTest, ActionButtonSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Unusable pane width: uncapped.
    EXPECT_FLOAT_EQ(computeActionButtonWidth(90.0F, REFERENCE_EM_PX, 0.0F, 16.0F), 180.0F);
    EXPECT_FLOAT_EQ(computeActionButtonWidth(90.0F, REFERENCE_EM_PX, nan, 16.0F), 180.0F);
    EXPECT_FLOAT_EQ(computeActionButtonWidth(90.0F, REFERENCE_EM_PX, inf, 16.0F), 180.0F);

    // Unusable overhead: treated as none.
    EXPECT_FLOAT_EQ(computeActionButtonWidth(90.0F, REFERENCE_EM_PX, 1256.0F, nan), 180.0F);
    EXPECT_FLOAT_EQ(computeActionButtonWidth(90.0F, REFERENCE_EM_PX, 1256.0F, -8.0F), 180.0F);
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

} // namespace
} // namespace App
