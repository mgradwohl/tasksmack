#include "App/DialogGeometry.h"
#include "App/Panels/ProcessDetailsLayout.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace App
