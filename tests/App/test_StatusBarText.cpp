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
#include <string_view>

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
    // The Settings presets.
    EXPECT_EQ(updateIntervalText(100), "Updates every 100 ms");
    EXPECT_EQ(updateIntervalText(250), "Updates every 250 ms");
    EXPECT_EQ(updateIntervalText(500), "Updates every 500 ms");
    EXPECT_EQ(updateIntervalText(1000), "Updates every 1 s");
    EXPECT_EQ(updateIntervalText(2000), "Updates every 2 s");
    EXPECT_EQ(updateIntervalText(5000), "Updates every 5 s");
    // Anything else (a hand-edited config, a synthetic scenario) exactly, not rounded to "1.0 s" or
    // "1.2 s" (#1200 review).
    EXPECT_EQ(updateIntervalText(1001), "Updates every 1001 ms");
    EXPECT_EQ(updateIntervalText(1250), "Updates every 1250 ms");
    EXPECT_EQ(updateIntervalText(1500), "Updates every 1500 ms");
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

// Width of a string as 8 px per character (UTF-8 code point), standing in for ImGui::CalcTextSize.
float measureText(std::string_view text)
{
    float width = 0.0F;
    for (const char c : text)
    {
        width += ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ? 8.0F : 0.0F;
    }
    return width;
}

// Whatever is shown fits: the text as drawn -- a cut-short first segment at its real width, ellipsis
// included -- its separators and the readout never exceed the budget, at any width from nothing to
// wide (#1200 review: a bare "…" was drawn even where it was wider than the space left).
TEST(StatusBarTextTest, ShownPartsNeverOverrunTheBudget)
{
    const std::array<std::string_view, 2> texts{"312 processes", "Updates every 1 s"};
    const std::array<float, 2> widths{measureText(texts[0]), measureText(texts[1])};
    const float separator = measureText(SEPARATOR);
    int truncatedSteps = 0;
    for (int step = 0; step <= 400; ++step)
    {
        const auto budget = static_cast<float>(step);
        const auto fit = fitStatusBar(widths, separator, READOUT_PX, GAP_PX, budget);
        float used = 0.0F;
        if (fit.truncateFirst)
        {
            const std::string cut = ellipsize(texts[0], budget, measureText);
            used += measureText(cut);
            truncatedSteps += cut.empty() ? 0 : 1;
        }
        for (std::size_t i = 0; i < fit.segmentsShown; ++i)
        {
            used += widths.at(i) + ((i > 0) ? separator : 0.0F);
        }
        if (fit.showReadout)
        {
            used += GAP_PX + READOUT_PX;
        }
        EXPECT_LE(used, budget) << "budget " << budget;
    }
    EXPECT_GT(truncatedSteps, 0); // the sweep does reach the cut-short case
}

TEST(StatusBarTextTest, EllipsizeKeepsTheLongestPrefixThatFits)
{
    EXPECT_EQ(ellipsize("312 processes", 200.0F, measureText), "312 processes");   // fits whole
    EXPECT_EQ(ellipsize("312 processes", 40.0F, measureText), "312 \xE2\x80\xA6"); // 4 + the ellipsis
    EXPECT_EQ(ellipsize("312 processes", 8.0F, measureText), "\xE2\x80\xA6");      // just the ellipsis
    // Not even the ellipsis fits: nothing, rather than an ellipsis drawn past the budget.
    EXPECT_EQ(ellipsize("312 processes", 7.0F, measureText), "");
    EXPECT_EQ(ellipsize("312 processes", 0.0F, measureText), "");
    // Cut on a character boundary, never inside one.
    EXPECT_EQ(ellipsize("\xC3\xA9\xC3\xA9\xC3\xA9", 16.0F, measureText), "\xC3\xA9\xE2\x80\xA6");
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
