/// @file test_EmptyState.cpp
/// @brief Tests for the pure arithmetic behind UI::Widgets::renderEmptyState() (#927).

#include "UI/ChartWidgets.h"
#include "UI/EmptyState.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>

namespace UI::Widgets
{
namespace
{

// The #927 case: a small message in a large pane sits in the middle of it, not the corner.
TEST(EmptyStateTest, CentersContentInLargerSpace)
{
    EXPECT_FLOAT_EQ(centeredOffset(1400.0F, 200.0F), 600.0F);
    EXPECT_FLOAT_EQ(centeredOffset(2800.0F, 300.0F), 1250.0F);
}

TEST(EmptyStateTest, OffsetIsWholePixels)
{
    EXPECT_FLOAT_EQ(centeredOffset(1001.0F, 200.0F), 400.0F);
    EXPECT_FLOAT_EQ(centeredOffset(1000.5F, 199.25F), 400.0F);
}

// Content larger than the space starts at the leading edge instead of being pushed off it.
TEST(EmptyStateTest, OversizedContentStartsAtTheEdge)
{
    EXPECT_FLOAT_EQ(centeredOffset(100.0F, 300.0F), 0.0F);
    EXPECT_FLOAT_EQ(centeredOffset(0.0F, 300.0F), 0.0F);
    EXPECT_FLOAT_EQ(centeredOffset(-50.0F, 10.0F), 0.0F);
}

TEST(EmptyStateTest, ExactFitHasNoOffset)
{
    EXPECT_FLOAT_EQ(centeredOffset(300.0F, 300.0F), 0.0F);
}

TEST(EmptyStateTest, OffsetSurvivesNonFiniteInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FLOAT_EQ(centeredOffset(nan, 100.0F), 0.0F);
    EXPECT_FLOAT_EQ(centeredOffset(1000.0F, nan), 0.0F);
    EXPECT_FLOAT_EQ(centeredOffset(inf, 100.0F), 0.0F);
}

TEST(EmptyStateTest, WrapWidthIsCappedInEms)
{
    EXPECT_FLOAT_EQ(emptyStateWrapWidth(10.0F, 2800.0F), EMPTY_STATE_WRAP_EM * 10.0F);
    EXPECT_FLOAT_EQ(emptyStateWrapWidth(20.0F, 2800.0F), EMPTY_STATE_WRAP_EM * 20.0F);
}

// A pane narrower than the cap wraps at the pane, so the text is never clipped.
TEST(EmptyStateTest, WrapWidthNeverExceedsTheSpaceAvailable)
{
    EXPECT_FLOAT_EQ(emptyStateWrapWidth(10.0F, 180.0F), 180.0F);
}

TEST(EmptyStateTest, WrapWidthSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(emptyStateWrapWidth(10.0F, 0.0F), EMPTY_STATE_WRAP_EM * 10.0F);
    EXPECT_FLOAT_EQ(emptyStateWrapWidth(10.0F, nan), EMPTY_STATE_WRAP_EM * 10.0F);
    EXPECT_GT(emptyStateWrapWidth(nan, 500.0F), 0.0F);
    EXPECT_GT(emptyStateWrapWidth(0.0F, 500.0F), 0.0F);
}

// A freshly selected process has no history; its charts say so until there is something to see.
TEST(EmptyStateTest, ChartIsCollectingUntilItHasEnoughSamples)
{
    EXPECT_TRUE(historyChartIsCollecting(0));
    EXPECT_TRUE(historyChartIsCollecting(1));
    EXPECT_TRUE(historyChartIsCollecting(HISTORY_COLLECTING_SAMPLE_COUNT - 1));
    EXPECT_FALSE(historyChartIsCollecting(HISTORY_COLLECTING_SAMPLE_COUNT));
    EXPECT_FALSE(historyChartIsCollecting(300));
}

} // namespace
} // namespace UI::Widgets
