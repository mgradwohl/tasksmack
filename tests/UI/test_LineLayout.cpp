/// @file test_LineLayout.cpp
/// @brief Tests for the pure placement arithmetic in UI/LineLayout.h (#966, #967).

#include "UI/LineLayout.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace UI::LineLayout
{
namespace
{

// ========== placeTrailingBlock (#967) ==========

// Plenty of room: the block shares the line, right-aligned.
TEST(LineLayoutTest, TrailingBlockSharesTheLineWhenItFits)
{
    const auto placement = placeTrailingBlock(
        /*lineStartX=*/12.0F, /*leadingEndX=*/900.0F, /*rightEdgeX=*/1560.0F, /*blockWidthPx=*/360.0F, /*minGapPx=*/16.0F);
    EXPECT_TRUE(placement.sameLine);
    EXPECT_FLOAT_EQ(placement.x, 1200.0F);
}

// The reported case, with the numbers it was measured at: a 1100px window at Extra Large on a 175%
// display, where the CPU summary ran to about x=930 and the 360px block was placed at x=690 --
// 240px inside the summary. It must go on its own line instead.
TEST(LineLayoutTest, TrailingBlockDropsToItsOwnLineRatherThanOverlap)
{
    const auto placement = placeTrailingBlock(12.0F, 930.0F, 1050.0F, 360.0F, 16.0F);
    EXPECT_FALSE(placement.sameLine);
    EXPECT_FLOAT_EQ(placement.x, 690.0F); // still right-aligned
}

// Fitting is not enough: the block must also clear the leading text by the gap, or the two read as
// one run of text.
TEST(LineLayoutTest, TrailingBlockNeedsTheGapAsWellAsTheRoom)
{
    // Block would start at 700. Leading text ends at 690: it fits, but with 10px where 16 is wanted.
    EXPECT_FALSE(placeTrailingBlock(0.0F, 690.0F, 1000.0F, 300.0F, 16.0F).sameLine);
    // Exactly the gap is enough.
    EXPECT_TRUE(placeTrailingBlock(0.0F, 684.0F, 1000.0F, 300.0F, 16.0F).sameLine);
}

// A block wider than the whole line starts at the line's own start, not left of it.
TEST(LineLayoutTest, OversizedTrailingBlockStartsAtTheLineStart)
{
    const auto placement = placeTrailingBlock(12.0F, 200.0F, 300.0F, 500.0F, 16.0F);
    EXPECT_FALSE(placement.sameLine);
    EXPECT_FLOAT_EQ(placement.x, 12.0F);
}

// No leading text at all (its end is the line start): the block always shares the line.
TEST(LineLayoutTest, TrailingBlockSharesAnEmptyLine)
{
    const auto placement = placeTrailingBlock(12.0F, 12.0F, 800.0F, 300.0F, 0.0F);
    EXPECT_TRUE(placement.sameLine);
    EXPECT_FLOAT_EQ(placement.x, 500.0F);
}

TEST(LineLayoutTest, TrailingBlockSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    for (const auto& placement : {
             placeTrailingBlock(nan, 100.0F, 800.0F, 200.0F, 16.0F),
             placeTrailingBlock(0.0F, nan, 800.0F, 200.0F, 16.0F),
             placeTrailingBlock(0.0F, 100.0F, nan, 200.0F, 16.0F),
             placeTrailingBlock(0.0F, 100.0F, 800.0F, nan, 16.0F),
             placeTrailingBlock(0.0F, 100.0F, 800.0F, 200.0F, nan),
             placeTrailingBlock(0.0F, 100.0F, inf, 200.0F, 16.0F),
             placeTrailingBlock(0.0F, 100.0F, 800.0F, -50.0F, -4.0F),
         })
    {
        EXPECT_TRUE(std::isfinite(placement.x));
        EXPECT_GE(placement.x, 0.0F);
    }
}

// ========== labelColumnWidth (#966) ==========

// The column is its widest label plus a gap of one em, so it grows with the text it holds and with
// the font, with no pixel constant in between.
TEST(LineLayoutTest, LabelColumnIsTheWidestLabelPlusAnEm)
{
    EXPECT_FLOAT_EQ(labelColumnWidth(/*widestLabelPx=*/108.0F, /*emPx=*/10.0F), 118.0F);
    EXPECT_FLOAT_EQ(labelColumnWidth(378.0F, 37.0F), 378.0F + 37.0F);
}

// The reported case: at Even Huger on a 175% display "GPU Utilization:" measured about 290px and was
// drawn in a 150px column. Whatever the label measures, the column must be at least that wide.
TEST(LineLayoutTest, LabelColumnIsNeverNarrowerThanItsLabel)
{
    for (const float label : {40.0F, 150.0F, 290.0F, 900.0F})
    {
        EXPECT_GT(labelColumnWidth(label, 37.0F), label);
        EXPECT_GE(labelColumnWidth(label, 0.0F), label);
    }
}

TEST(LineLayoutTest, LabelColumnSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(labelColumnWidth(nan, 10.0F), 10.0F);
    EXPECT_FLOAT_EQ(labelColumnWidth(-5.0F, 10.0F), 10.0F);
    EXPECT_FLOAT_EQ(labelColumnWidth(100.0F, nan), 100.0F);
    EXPECT_FLOAT_EQ(labelColumnWidth(100.0F, -3.0F), 100.0F);
}

// ========== clampSpanStart (keeping the metrics overlay on screen) ==========

// A span already inside the bounds is left exactly where it is.
TEST(LineLayoutTest, SpanInsideTheBoundsIsNotMoved)
{
    EXPECT_FLOAT_EQ(clampSpanStart(/*startPx=*/60.0F, /*lengthPx=*/500.0F, /*boundsStartPx=*/0.0F, /*boundsLengthPx=*/2000.0F), 60.0F);
    EXPECT_FLOAT_EQ(clampSpanStart(1500.0F, 500.0F, 0.0F, 2000.0F), 1500.0F); // flush with the far edge
}

// The reviewed case: an overlay at x=60, 1204px wide on a 2000px viewport that then shrinks to
// 900px. The size constraint fits it to 900; its position must come back to 0 so all of it shows.
TEST(LineLayoutTest, SpanIsPulledBackWhenTheBoundsShrink)
{
    EXPECT_FLOAT_EQ(clampSpanStart(60.0F, 900.0F, 0.0F, 900.0F), 0.0F);
    // Narrower than the shrunken viewport but overhanging it: moved just far enough to fit.
    EXPECT_FLOAT_EQ(clampSpanStart(600.0F, 500.0F, 0.0F, 900.0F), 400.0F);
}

// A span before the bounds is brought up to their start.
TEST(LineLayoutTest, SpanBeforeTheBoundsMovesToTheirStart)
{
    EXPECT_FLOAT_EQ(clampSpanStart(-200.0F, 500.0F, 0.0F, 900.0F), 0.0F);
}

// A viewport whose work area does not begin at zero (a secondary monitor, or a reserved edge).
TEST(LineLayoutTest, SpanIsClampedAgainstOffsetBounds)
{
    EXPECT_FLOAT_EQ(clampSpanStart(100.0F, 500.0F, 1920.0F, 1000.0F), 1920.0F);  // left of the viewport
    EXPECT_FLOAT_EQ(clampSpanStart(2800.0F, 500.0F, 1920.0F, 1000.0F), 2420.0F); // overhanging its far edge
    EXPECT_FLOAT_EQ(clampSpanStart(2000.0F, 500.0F, 1920.0F, 1000.0F), 2000.0F); // inside: untouched
}

// A span longer than the bounds cannot fit; it starts at the bounds' start, so the leading end --
// where a window's title bar and close button are -- is the part that stays visible.
TEST(LineLayoutTest, OversizedSpanStartsAtTheBoundsStart)
{
    EXPECT_FLOAT_EQ(clampSpanStart(60.0F, 1204.0F, 0.0F, 900.0F), 0.0F);
    EXPECT_FLOAT_EQ(clampSpanStart(2500.0F, 1204.0F, 1920.0F, 900.0F), 1920.0F);
}

TEST(LineLayoutTest, SpanClampSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    EXPECT_FLOAT_EQ(clampSpanStart(nan, 500.0F, 100.0F, 900.0F), 100.0F);
    EXPECT_FLOAT_EQ(clampSpanStart(inf, 500.0F, 100.0F, 900.0F), 100.0F);
    EXPECT_FLOAT_EQ(clampSpanStart(300.0F, nan, 100.0F, 900.0F), 300.0F); // no length: only the start is bounded
    EXPECT_FLOAT_EQ(clampSpanStart(300.0F, -50.0F, 100.0F, 900.0F), 300.0F);
    EXPECT_FLOAT_EQ(clampSpanStart(300.0F, 500.0F, nan, 900.0F), 300.0F);  // unknown origin is treated as zero
    EXPECT_FLOAT_EQ(clampSpanStart(300.0F, 500.0F, 100.0F, 0.0F), 100.0F); // a minimized viewport has no room
    EXPECT_FLOAT_EQ(clampSpanStart(300.0F, 500.0F, 100.0F, nan), 100.0F);
}
} // namespace
} // namespace UI::LineLayout
