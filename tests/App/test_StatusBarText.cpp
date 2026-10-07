/// @file test_StatusBarText.cpp
/// @brief Tests for the status bar's live text and its narrow-window fitting (App/StatusBarText.h, #1200)

#include "App/StatusBarText.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>

namespace App::StatusBarText
{
namespace
{

// ========== Text ==========

TEST(StatusBarTextTest, ProcessCountIsPluralisedAndGrouped)
{
    EXPECT_EQ(processCountText(1), "1 process");
    EXPECT_EQ(processCountText(2), "2 processes");
    // Grouped as the rest of the UI groups counts.
    EXPECT_EQ(processCountText(1234), UI::Format::formatIntLocalized(std::size_t{1234}) + " processes");
}

TEST(StatusBarTextTest, NoProcessesYetSaysItIsCollecting)
{
    EXPECT_TRUE(processCountText(0).starts_with("Collecting"));
}

TEST(StatusBarTextTest, IntervalIsInTheUnitItWasSetIn)
{
    EXPECT_EQ(updateIntervalText(250), "Updates every 250 ms");
    EXPECT_EQ(updateIntervalText(1000), "Updates every 1 s");
    EXPECT_EQ(updateIntervalText(5000), "Updates every 5 s");
    EXPECT_EQ(updateIntervalText(1500), "Updates every 1.5 s");
    EXPECT_EQ(updateIntervalText(0), ""); // before the first RefreshRateChangedEvent
    EXPECT_EQ(updateIntervalText(-5), "");
}

// The status bar said "Ready" for the whole session.
TEST(StatusBarTextTest, NeverSaysReady)
{
    EXPECT_NE(processCountText(0), "Ready");
    EXPECT_NE(processCountText(300), "Ready");
}

// ========== Fitting ==========

// Widths as at the reference font: "312 processes", "Updates every 1 s", the separator, the FPS.
constexpr std::array<float, 2> SEGMENTS{80.0F, 100.0F};
constexpr float SEPARATOR_PX = 15.0F;
constexpr float READOUT_PX = 110.0F;
constexpr float GAP_PX = 8.0F;

TEST(StatusBarTextTest, EverythingFitsInAWideBar)
{
    const auto fit = fitStatusBar(SEGMENTS, SEPARATOR_PX, READOUT_PX, GAP_PX, 1200.0F);
    EXPECT_EQ(fit.segmentsShown, 2U);
    EXPECT_FALSE(fit.truncateFirst);
    EXPECT_TRUE(fit.showReadout);
}

TEST(StatusBarTextTest, ReadoutIsHiddenWhenRenderMetricsIsOff)
{
    const auto fit = fitStatusBar(SEGMENTS, SEPARATOR_PX, 0.0F, GAP_PX, 1200.0F);
    EXPECT_EQ(fit.segmentsShown, 2U);
    EXPECT_FALSE(fit.showReadout);
}

// The readout is the first thing to go: it was drawn over "Ready" and the buttons in a narrow
// window (#1207), and the text beside it is what a user reads.
TEST(StatusBarTextTest, ReadoutGoesFirst)
{
    // 80 + 15 + 100 = 195 for the text; the readout needs 8 + 110 more.
    EXPECT_TRUE(fitStatusBar(SEGMENTS, SEPARATOR_PX, READOUT_PX, GAP_PX, 313.0F).showReadout); // exactly
    const auto fit = fitStatusBar(SEGMENTS, SEPARATOR_PX, READOUT_PX, GAP_PX, 312.0F);
    EXPECT_FALSE(fit.showReadout);
    EXPECT_EQ(fit.segmentsShown, 2U);
}

TEST(StatusBarTextTest, IntervalGoesBeforeTheProcessCount)
{
    const auto fit = fitStatusBar(SEGMENTS, SEPARATOR_PX, READOUT_PX, GAP_PX, 194.0F);
    EXPECT_EQ(fit.segmentsShown, 1U);
    EXPECT_FALSE(fit.truncateFirst);
    EXPECT_FALSE(fit.showReadout); // never with a segment dropped
}

TEST(StatusBarTextTest, ProcessCountIsCutShortRatherThanDropped)
{
    const auto fit = fitStatusBar(SEGMENTS, SEPARATOR_PX, READOUT_PX, GAP_PX, 50.0F);
    EXPECT_EQ(fit.segmentsShown, 0U);
    EXPECT_TRUE(fit.truncateFirst);
    EXPECT_FALSE(fit.showReadout);
}

// No room at all -- the buttons alone fill the bar -- draws no text, rather than text over them.
TEST(StatusBarTextTest, NothingIsDrawnWithoutRoom)
{
    for (const float budget : {0.0F, -40.0F, std::numeric_limits<float>::quiet_NaN()})
    {
        const auto fit = fitStatusBar(SEGMENTS, SEPARATOR_PX, READOUT_PX, GAP_PX, budget);
        EXPECT_EQ(fit.segmentsShown, 0U);
        EXPECT_FALSE(fit.truncateFirst);
        EXPECT_FALSE(fit.showReadout);
    }
}

// Whatever is shown fits: the text, its separators and the readout never exceed the budget, at any
// width from nothing to wide.
TEST(StatusBarTextTest, ShownPartsNeverOverrunTheBudget)
{
    for (int step = 0; step <= 400; ++step)
    {
        const auto budget = static_cast<float>(step);
        const auto fit = fitStatusBar(SEGMENTS, SEPARATOR_PX, READOUT_PX, GAP_PX, budget);
        float used = 0.0F;
        for (std::size_t i = 0; i < fit.segmentsShown; ++i)
        {
            used += SEGMENTS.at(i) + ((i > 0) ? SEPARATOR_PX : 0.0F);
        }
        if (fit.showReadout)
        {
            used += GAP_PX + READOUT_PX;
        }
        EXPECT_LE(used, budget) << "budget " << budget;
    }
}

TEST(StatusBarTextTest, NoSegmentsLeavesOnlyTheReadout)
{
    const auto fit = fitStatusBar(std::span<const float>{}, SEPARATOR_PX, READOUT_PX, GAP_PX, 200.0F);
    EXPECT_EQ(fit.segmentsShown, 0U);
    EXPECT_FALSE(fit.truncateFirst);
    EXPECT_TRUE(fit.showReadout);
}

} // namespace
} // namespace App::StatusBarText
