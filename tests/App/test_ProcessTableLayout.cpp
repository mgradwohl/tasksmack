#include "App/Panels/ProcessTableLayout.h"

#include <gtest/gtest.h>

#include <cmath>
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

// ========== Filter box width (#965) ==========

using ProcessTableLayout::computeFilterWidth;
using ProcessTableLayout::FILTER_WIDTH_EM;

// One em at the reference configuration: the Medium preset (8pt) on a 1.0 display scale, at 96 DPI.
constexpr float REFERENCE_EM = 32.0F / 3.0F;

// The em multiple reproduces the fixed 200px it replaces, so the box is unchanged at the reference.
TEST(ProcessTableLayoutTest, FilterWidthIsUnchangedAtTheReferenceConfiguration)
{
    EXPECT_FLOAT_EQ(computeFilterWidth(/*hintWidthPx=*/95.0F, /*framePaddingPx=*/4.0F, REFERENCE_EM, /*rowWidthPx=*/1256.0F), 200.0F);
}

// The reported case: Even Huger on a 175% display, where one em is about 37px and the hint about
// 290px. A 200px box cut the hint to "Filter by nam"; the box must now hold all of it.
TEST(ProcessTableLayoutTest, FilterWidthGrowsWithTheFont)
{
    const float width = computeFilterWidth(290.0F, 9.0F, 37.33F, 1976.0F);
    EXPECT_FLOAT_EQ(width, FILTER_WIDTH_EM * 37.33F);
    EXPECT_GT(width, 290.0F + 18.0F);
}

// A hint wider than the em multiple (a longer translation, a condensed font) still fits.
TEST(ProcessTableLayoutTest, FilterWidthIsNeverNarrowerThanItsHint)
{
    EXPECT_FLOAT_EQ(computeFilterWidth(400.0F, 6.0F, 10.0F, 4000.0F), 412.0F);
}

// On a narrow pane the box gives way to the summary and the view toggle that share its row.
TEST(ProcessTableLayoutTest, FilterWidthIsCappedToHalfTheRow)
{
    EXPECT_FLOAT_EQ(computeFilterWidth(290.0F, 9.0F, 37.33F, 800.0F), 400.0F);
}

TEST(ProcessTableLayoutTest, FilterWidthSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Unknown row width: no cap.
    EXPECT_FLOAT_EQ(computeFilterWidth(95.0F, 4.0F, REFERENCE_EM, 0.0F), 200.0F);
    EXPECT_FLOAT_EQ(computeFilterWidth(95.0F, 4.0F, REFERENCE_EM, nan), 200.0F);
    EXPECT_FLOAT_EQ(computeFilterWidth(95.0F, 4.0F, REFERENCE_EM, inf), 200.0F);

    for (const float width : {
             computeFilterWidth(nan, 4.0F, 10.0F, 1000.0F),
             computeFilterWidth(95.0F, nan, 10.0F, 1000.0F),
             computeFilterWidth(95.0F, 4.0F, nan, 1000.0F),
             computeFilterWidth(95.0F, 4.0F, 0.0F, 1000.0F),
             computeFilterWidth(-95.0F, -4.0F, -10.0F, 1000.0F),
         })
    {
        EXPECT_TRUE(std::isfinite(width));
        EXPECT_GT(width, 0.0F);
    }
}

} // namespace
} // namespace App
