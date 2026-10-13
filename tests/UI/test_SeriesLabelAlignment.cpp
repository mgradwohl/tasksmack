/// @file test_SeriesLabelAlignment.cpp
/// @brief Tests for the pure series-label alignment decisions in UI/SeriesLabelAlignment.h (#1882):
/// which side each label of a single-axis or dual-axis chart aligns to, and where a value strip's
/// left- and right-aligned groups go on its line.

#include "UI/SeriesLabelAlignment.h"

#include <gtest/gtest.h>

#include <limits>

namespace UI::SeriesLabelAlignment
{
namespace
{

// ========== labelSide ==========

// One Y axis, its scale on the left (every chart today): every label is left-aligned.
TEST(SeriesLabelAlignmentTest, SingleLeftAxisAlignsEveryLabelLeft)
{
    constexpr ChartAxes AXES{.primary = AxisSide::Left};
    EXPECT_EQ(labelSide(AXES, false), AxisSide::Left);
    static_assert(labelSide(ChartAxes{}, false) == AxisSide::Left, "a chart's scale is on the left by default");
}

// One Y axis with its scale on the right: every label is right-aligned.
TEST(SeriesLabelAlignmentTest, SingleRightAxisAlignsEveryLabelRight)
{
    constexpr ChartAxes AXES{.primary = AxisSide::Right};
    EXPECT_EQ(labelSide(AXES, false), AxisSide::Right);
}

// Two Y axes: the primary (left) axis's series are left-aligned, the secondary (right) axis's
// series right-aligned -- each on the side of the scale it is read against.
TEST(SeriesLabelAlignmentTest, DualAxisAlignsEachLabelToItsOwnAxis)
{
    constexpr ChartAxes AXES{.primary = AxisSide::Left};
    EXPECT_EQ(labelSide(AXES, false), AxisSide::Left);
    EXPECT_EQ(labelSide(AXES, true), AxisSide::Right);
}

// The secondary axis is drawn opposite the primary one, wherever that is.
TEST(SeriesLabelAlignmentTest, SecondaryAxisIsOppositeAPrimaryOnTheRight)
{
    constexpr ChartAxes AXES{.primary = AxisSide::Right};
    EXPECT_EQ(labelSide(AXES, false), AxisSide::Right);
    EXPECT_EQ(labelSide(AXES, true), AxisSide::Left);
    EXPECT_EQ(oppositeSide(AxisSide::Left), AxisSide::Right);
    EXPECT_EQ(oppositeSide(AxisSide::Right), AxisSide::Left);
}

// ========== placeStrip ==========

// Single left axis: the labels follow the heading on its line.
TEST(SeriesLabelAlignmentTest, LeftLabelsFollowTheHeadingOnItsLine)
{
    const StripPlacement placement = placeStrip(
        /*lineStartX=*/8.0F, /*headingEndX=*/120.0F, /*rightEdgeX=*/700.0F, /*leftWidthPx=*/300.0F, /*rightWidthPx=*/0.0F, /*gapPx=*/16.0F);
    EXPECT_TRUE(placement.sameLine);
    EXPECT_TRUE(placement.oneRow);
    EXPECT_FLOAT_EQ(placement.leftX, 136.0F);
}

// Single right axis: the labels end at the chart's right edge, as the strip always did before #1882.
TEST(SeriesLabelAlignmentTest, RightLabelsEndAtTheChartsRightEdge)
{
    const StripPlacement placement = placeStrip(8.0F, 120.0F, 700.0F, 0.0F, 300.0F, 16.0F);
    EXPECT_TRUE(placement.sameLine);
    EXPECT_TRUE(placement.oneRow);
    EXPECT_FLOAT_EQ(placement.rightX, 400.0F);
}

// Dual axis: left labels after the heading, right labels flush right, all on the heading's line.
TEST(SeriesLabelAlignmentTest, DualAxisSplitsTheLabelsAcrossTheHeadingsLine)
{
    const StripPlacement placement = placeStrip(8.0F, 120.0F, 700.0F, 200.0F, 150.0F, 16.0F);
    EXPECT_TRUE(placement.sameLine);
    EXPECT_TRUE(placement.oneRow);
    EXPECT_FLOAT_EQ(placement.leftX, 136.0F);
    EXPECT_FLOAT_EQ(placement.rightX, 550.0F);
}

// The heading's line has room for the strip only with a gap after the heading and between the
// groups: the same room the right-aligned strip needed, so no chart gets taller.
TEST(SeriesLabelAlignmentTest, AStripThatCrowdsTheHeadingTakesALineOfItsOwn)
{
    // 120 + 16 + 200 + 16 + 348 = 700: just fits.
    EXPECT_TRUE(placeStrip(8.0F, 120.0F, 700.0F, 200.0F, 348.0F, 16.0F).sameLine);
    // One pixel more and it drops below, the left group at the line's start, the right still flush.
    const StripPlacement placement = placeStrip(8.0F, 120.0F, 700.0F, 200.0F, 349.0F, 16.0F);
    EXPECT_FALSE(placement.sameLine);
    EXPECT_TRUE(placement.oneRow);
    EXPECT_FLOAT_EQ(placement.leftX, 8.0F);
    EXPECT_FLOAT_EQ(placement.rightX, 351.0F);
}

// Wider than the chart: no single row; the left group starts the line and wraps, and a right group
// that fits a row on its own stays flush right while one that does not starts at the line's start.
TEST(SeriesLabelAlignmentTest, AStripWiderThanTheChartWraps)
{
    const StripPlacement placement = placeStrip(8.0F, 120.0F, 400.0F, 300.0F, 200.0F, 16.0F);
    EXPECT_FALSE(placement.sameLine);
    EXPECT_FALSE(placement.oneRow);
    EXPECT_FLOAT_EQ(placement.leftX, 8.0F);
    EXPECT_FLOAT_EQ(placement.rightX, 200.0F);
    EXPECT_FLOAT_EQ(placeStrip(8.0F, 120.0F, 400.0F, 0.0F, 500.0F, 16.0F).rightX, 8.0F);
}

// No heading to share (an unplaced strip, or a heading wider than the row): a line of its own.
TEST(SeriesLabelAlignmentTest, NoHeadingRoomMeansALineOfItsOwn)
{
    const StripPlacement placement = placeStrip(8.0F, std::numeric_limits<float>::max(), 700.0F, 100.0F, 100.0F, 16.0F);
    EXPECT_FALSE(placement.sameLine);
    EXPECT_TRUE(placement.oneRow);
    EXPECT_FLOAT_EQ(placement.leftX, 8.0F);
    EXPECT_FLOAT_EQ(placement.rightX, 600.0F);
}

// Non-finite or negative inputs never place a group off the line or report a fit that is not there.
TEST(SeriesLabelAlignmentTest, BadInputsStayOnTheLine)
{
    constexpr float NAN_VALUE = std::numeric_limits<float>::quiet_NaN();
    const StripPlacement placement = placeStrip(NAN_VALUE, NAN_VALUE, NAN_VALUE, -5.0F, NAN_VALUE, NAN_VALUE);
    EXPECT_FLOAT_EQ(placement.leftX, 0.0F);
    EXPECT_FLOAT_EQ(placement.rightX, 0.0F);
    EXPECT_TRUE(placement.oneRow); // nothing to place
}

} // namespace
} // namespace UI::SeriesLabelAlignment
