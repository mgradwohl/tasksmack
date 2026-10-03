/// @file test_HistoryPlotHeight.cpp
/// @brief Tests for UI::Widgets::computeFillPlotHeight(), the one height rule shared by the tabs
/// that stack several history charts (#922, #923).

#include "UI/HistoryPlotHeight.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>

namespace UI::Widgets
{
namespace
{

// One em at the Medium preset on a 1.0 display scale: 8pt at 96 DPI.
constexpr float REFERENCE_EM_PX = 32.0F / 3.0F;

TEST(HistoryPlotHeightTest, BoundsAtReferenceEm)
{
    // Chart text at the body size (Medium since #1194): 11.25 chart ems.
    EXPECT_FLOAT_EQ(historyPlotMinHeight(REFERENCE_EM_PX), 120.0F);
    EXPECT_FLOAT_EQ(historyPlotMinHeight(REFERENCE_EM_PX, REFERENCE_EM_PX), 120.0F);
    EXPECT_FLOAT_EQ(historyPlotMaxHeight(REFERENCE_EM_PX), 360.0F);
}

TEST(HistoryPlotHeightTest, BoundsScaleWithTheFont)
{
    // With small chart text the body-em floor governs: 180px at twice the reference em, the size
    // these charts were fixed at before.
    EXPECT_FLOAT_EQ(historyPlotMinHeight(REFERENCE_EM_PX * 2.0F, REFERENCE_EM_PX), 180.0F);
    EXPECT_FLOAT_EQ(historyPlotMinHeight(REFERENCE_EM_PX * 2.0F), 240.0F);
    EXPECT_FLOAT_EQ(historyPlotMaxHeight(REFERENCE_EM_PX * 2.0F), 720.0F);
    EXPECT_FLOAT_EQ(historyPlotMinHeight(8.0F), 90.0F);
    EXPECT_FLOAT_EQ(historyPlotMaxHeight(8.0F), 270.0F);
}

// The #922 case: a tall window. The charts share the height instead of stopping at a fixed size and
// leaving the rest empty.
TEST(HistoryPlotHeightTest, ChartsShareTheAvailableHeight)
{
    // 1100px region, 160px of headings and padding, four charts.
    // (1100 - 160 - 2) / 4 = 234.5, rounded down to a whole pixel.
    const float height = computeFillPlotHeight(REFERENCE_EM_PX, 1100.0F, 160.0F, 4);
    EXPECT_FLOAT_EQ(height, 234.0F);

    // And together with the non-plot height they fit the region.
    EXPECT_LE((height * 4.0F) + 160.0F, 1100.0F);
}

// The #923 half of the rule: past the maximum, more room does not make the charts taller.
TEST(HistoryPlotHeightTest, GrowthStopsAtTheMaximum)
{
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 4000.0F, 160.0F, 4), 360.0F);
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 9000.0F, 160.0F, 4), 360.0F);
}

// A short window: the charts hold their minimum and the tab scrolls, rather than squashing flat.
TEST(HistoryPlotHeightTest, ShortRegionHoldsTheMinimum)
{
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 400.0F, 160.0F, 4), 120.0F);
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 100.0F, 160.0F, 4), 120.0F);
}

// #1194: the floor is also measured in ems of the chart's own text, so moving chart text up to the
// body size makes the minimum chart taller instead of clipping its legend. With chart text at
// three quarters of the body (Medium before #1194) it is the 90px it was.
TEST(HistoryPlotHeightTest, MinimumFollowsTheChartText)
{
    EXPECT_FLOAT_EQ(historyPlotMinHeight(REFERENCE_EM_PX, REFERENCE_EM_PX * 0.75F), 90.0F);
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 100.0F, 160.0F, 4, REFERENCE_EM_PX * 0.75F), 90.0F);
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 100.0F, 160.0F, 4, REFERENCE_EM_PX), 120.0F);
    // An unusable chart em falls back to the body em.
    EXPECT_FLOAT_EQ(historyPlotMinHeight(REFERENCE_EM_PX, std::numeric_limits<float>::quiet_NaN()), 120.0F);
    EXPECT_FLOAT_EQ(historyPlotMinHeight(REFERENCE_EM_PX, -1.0F), 120.0F);
}

// Review of #1219: from Large up the chart text is a step smaller than the body, as it was before
// #1194, so those presets keep their old floor: 180px at Even Huger (16pt body, 14pt charts).
TEST(HistoryPlotHeightTest, LargerPresetsKeepTheBodyFloor)
{
    const float evenHugerEm = REFERENCE_EM_PX * 2.0F;
    EXPECT_FLOAT_EQ(historyPlotMinHeight(evenHugerEm, evenHugerEm * 14.0F / 16.0F), 180.0F);
    const float largeEm = REFERENCE_EM_PX * 1.25F;
    EXPECT_FLOAT_EQ(historyPlotMinHeight(largeEm, largeEm * 0.8F), 112.5F);
}

// The floor never exceeds the ceiling, even with chart text larger than the body text.
TEST(HistoryPlotHeightTest, MinimumNeverExceedsTheMaximum)
{
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 100.0F, 160.0F, 4, REFERENCE_EM_PX * 10.0F), 360.0F);
}

// #923 reported Small more than doubling the chart height (920px against 400px at Even Huger),
// because a smaller font left more room and the charts took all of it. The cap bounds that: at
// Small the charts stop at their own maximum however much room the smaller font frees up.
TEST(HistoryPlotHeightTest, SmallFontChartsStopAtTheirMaximumInATallRegion)
{
    const float smallEm = 8.0F;
    EXPECT_FLOAT_EQ(computeFillPlotHeight(smallEm, 1300.0F, 100.0F, 1), historyPlotMaxHeight(smallEm));
    EXPECT_FLOAT_EQ(computeFillPlotHeight(smallEm, 1300.0F, 100.0F, 1), 270.0F);
}

// The result is always a whole number of pixels, in every branch. A fractional height is what made
// the charts shimmer: ImGui rounds each item to whole pixels, the caller's measurement of the
// non-plot height absorbed that rounding, and the height cycled from frame to frame.
TEST(HistoryPlotHeightTest, ResultIsAlwaysWholePixels)
{
    for (const float em : {8.0F, REFERENCE_EM_PX, 13.33F, 21.0F, 21.33F})
    {
        for (const float available : {0.0F, 300.0F, 611.5F, 1197.25F, 1409.0F, 5000.0F})
        {
            for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{4}})
            {
                const float height = computeFillPlotHeight(em, available, 257.25F, count);
                EXPECT_FLOAT_EQ(height, std::floor(height)) << "em=" << em << " available=" << available << " count=" << count;
            }
        }
    }
}

// The caller feeds back "what the frame used, less the plots" as the next frame's non-plot height.
// With whole-pixel plots that measurement does not depend on the plot height, so the height must
// settle after one step and stay put -- the regression test for the shimmer described above, using
// the numbers it was observed with (Even Huger, 2147x1409: 1198px region, ~257px of non-plot).
TEST(HistoryPlotHeightTest, FeedbackSettlesAndStaysPut)
{
    const float em = 21.0F;
    const float chartEm = em * 14.0F / 16.0F; // Even Huger's charts use Huge's 14pt (#1194)
    const float available = 1198.0F;
    const float trueNonPlot = 257.0F;
    const std::size_t count = 4;

    float nonPlot = 0.0F;
    std::size_t measuredCount = 0;
    float previous = -1.0F;
    int changesAfterSettling = 0;
    for (int frame = 0; frame < 12; ++frame)
    {
        const float height = computeFillPlotHeight(em, available, nonPlot, measuredCount, chartEm);
        if (frame >= 2 && height != previous)
        {
            ++changesAfterSettling;
        }
        previous = height;

        // What ImGui would lay out: each plot on whole pixels, plus the fixed non-plot content.
        const float used = trueNonPlot + (static_cast<float>(count) * std::round(height));
        nonPlot = used - (static_cast<float>(count) * height);
        measuredCount = count;
    }

    EXPECT_EQ(changesAfterSettling, 0);
    EXPECT_FLOAT_EQ(previous, 234.0F);
}

TEST(HistoryPlotHeightTest, FewerChartsGetMoreHeightEach)
{
    const float three = computeFillPlotHeight(REFERENCE_EM_PX, 900.0F, 120.0F, 3);
    const float four = computeFillPlotHeight(REFERENCE_EM_PX, 900.0F, 120.0F, 4);
    EXPECT_GT(three, four);
}

// First frame: nothing measured. Under-fill at the minimum rather than guess and overflow.
TEST(HistoryPlotHeightTest, NothingMeasuredYetGivesTheMinimum)
{
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 1100.0F, 0.0F, 0), 120.0F);
}

TEST(HistoryPlotHeightTest, NegativeNonPlotHeightIsTreatedAsZero)
{
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 802.0F, -50.0F, 4), 200.0F);
}

TEST(HistoryPlotHeightTest, SurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, nan, 160.0F, 4), 120.0F);
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, inf, 160.0F, 4), 120.0F);
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 0.0F, 160.0F, 4), 120.0F);
    EXPECT_FLOAT_EQ(computeFillPlotHeight(REFERENCE_EM_PX, 1100.0F, nan, 4), 120.0F);

    for (const float em : {0.0F, -8.0F, nan, inf})
    {
        const float height = computeFillPlotHeight(em, 1100.0F, 160.0F, 4);
        EXPECT_GT(height, 0.0F);
        EXPECT_LE(height, HISTORY_PLOT_MAX_HEIGHT_EM);
    }
}

} // namespace
} // namespace UI::Widgets
