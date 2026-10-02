#include "App/Panels/ProcessTableLayout.h"

#include <gtest/gtest.h>

#include <limits>

namespace App
{
namespace
{

using ProcessTableLayout::computeInnerWidth;

// The #924 case: the columns sum to less than the window. Returning 0 lets ImGui size the stretch
// column against the visible width, so Command takes up the slack and nothing scrolls.
TEST(ProcessTableLayoutTest, FitsVisibleWidthWhenColumnsLeaveRoom)
{
    EXPECT_FLOAT_EQ(computeInnerWidth(/*otherColumnsWidthPx=*/700.0F, /*commandMinWidthPx=*/300.0F, /*visibleWidthPx=*/2800.0F), 0.0F);
}

TEST(ProcessTableLayoutTest, FitsVisibleWidthWhenColumnsExactlyFit)
{
    EXPECT_FLOAT_EQ(computeInnerWidth(700.0F, 300.0F, 1000.0F), 0.0F);
}

// The other half: once the other columns no longer leave Command its minimum, the table must get an
// explicit content width, or the stretch column collapses to nothing instead of scrolling.
TEST(ProcessTableLayoutTest, ScrollsWithCommandAtItsMinimumWhenColumnsOverflow)
{
    EXPECT_FLOAT_EQ(computeInnerWidth(1400.0F, 840.0F, 1250.0F), 2240.0F);
}

TEST(ProcessTableLayoutTest, ScrollsAsSoonAsCommandWouldDropBelowItsMinimum)
{
    EXPECT_FLOAT_EQ(computeInnerWidth(700.0F, 300.0F, 999.0F), 1000.0F);
}

// The other columns alone overflowing must scroll too, with room left for Command.
TEST(ProcessTableLayoutTest, ScrollsWhenOtherColumnsAloneOverflow)
{
    EXPECT_FLOAT_EQ(computeInnerWidth(1500.0F, 300.0F, 1000.0F), 1800.0F);
}

// Command hidden by the user: no minimum to reserve, but the remaining columns still scroll when
// they overflow and still fit when they do not.
TEST(ProcessTableLayoutTest, CommandMinimumOfZeroOnlyScrollsOnRealOverflow)
{
    EXPECT_FLOAT_EQ(computeInnerWidth(900.0F, 0.0F, 1000.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeInnerWidth(1100.0F, 0.0F, 1000.0F), 1100.0F);
    EXPECT_FLOAT_EQ(computeInnerWidth(1100.0F, -50.0F, 1000.0F), 1100.0F);
}

// First frame: nothing has been measured yet, so fit the visible width and let the next frame decide.
TEST(ProcessTableLayoutTest, FitsVisibleWidthBeforeAnythingIsMeasured)
{
    EXPECT_FLOAT_EQ(computeInnerWidth(0.0F, 300.0F, 0.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeInnerWidth(700.0F, 300.0F, 0.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeInnerWidth(0.0F, 300.0F, 1000.0F), 0.0F);
}

TEST(ProcessTableLayoutTest, FitsVisibleWidthOnNonFiniteInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    EXPECT_FLOAT_EQ(computeInnerWidth(nan, 300.0F, 1000.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeInnerWidth(700.0F, nan, 1000.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeInnerWidth(700.0F, 300.0F, nan), 0.0F);
    EXPECT_FLOAT_EQ(computeInnerWidth(inf, 300.0F, 1000.0F), 0.0F);
    EXPECT_FLOAT_EQ(computeInnerWidth(700.0F, 300.0F, inf), 0.0F);
}

// ========== Clipped cell text (#914) ==========

using ProcessTableLayout::CLIP_TOLERANCE_PX;
using ProcessTableLayout::isCellTextClipped;

TEST(ProcessTableLayoutTest, TextNarrowerThanCellIsNotClipped)
{
    EXPECT_FALSE(isCellTextClipped(60.0F, 120.0F));
}

TEST(ProcessTableLayoutTest, TextWiderThanCellIsClipped)
{
    EXPECT_TRUE(isCellTextClipped(130.0F, 120.0F));
}

// Text that exactly fills its cell is shown whole: it must not lose its last glyph to an ellipsis
// over sub-pixel rounding between the text measurement and the column layout.
TEST(ProcessTableLayoutTest, TextThatExactlyFitsIsNotClipped)
{
    EXPECT_FALSE(isCellTextClipped(120.0F, 120.0F));
    EXPECT_FALSE(isCellTextClipped(120.0F + CLIP_TOLERANCE_PX, 120.0F));
    EXPECT_FALSE(isCellTextClipped(120.25F, 120.0F));
}

TEST(ProcessTableLayoutTest, TextJustPastToleranceIsClipped)
{
    EXPECT_TRUE(isCellTextClipped(121.0F, 120.0F));
}

// A cell with no room left at all (a column squeezed to nothing, or a tree indent that used it up)
// clips any text that has width.
TEST(ProcessTableLayoutTest, AnyTextIsClippedInACellWithNoRoom)
{
    EXPECT_TRUE(isCellTextClipped(40.0F, 0.0F));
    EXPECT_TRUE(isCellTextClipped(40.0F, -12.0F));
}

TEST(ProcessTableLayoutTest, EmptyTextIsNeverClipped)
{
    EXPECT_FALSE(isCellTextClipped(0.0F, 0.0F));
    EXPECT_FALSE(isCellTextClipped(0.0F, 120.0F));
}

TEST(ProcessTableLayoutTest, NonFiniteWidthsAreNotClipped)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    EXPECT_FALSE(isCellTextClipped(nan, 120.0F));
    EXPECT_FALSE(isCellTextClipped(60.0F, nan));
    EXPECT_FALSE(isCellTextClipped(inf, 120.0F));
    EXPECT_FALSE(isCellTextClipped(60.0F, inf));
}

} // namespace
} // namespace App
