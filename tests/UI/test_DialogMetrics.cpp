#include "UI/DialogMetrics.h"

#include <gtest/gtest.h>

#include <limits>

namespace UI::DialogMetrics
{
namespace
{

// One em at the reference configuration: the Medium preset (8pt) on a 1.0 display scale, at 96 DPI.
constexpr float REFERENCE_EM = 32.0F / 3.0F;

TEST(DialogMetricsTest, DialogWidthIsTheEmMultiple)
{
    // Arithmetic only. That each dialog's *own* constant still reproduces the pixel size it replaced
    // is asserted against the production symbols in tests/App/test_DialogGeometry.cpp -- a literal
    // here would stay green while a call-site constant changed and the dialog restyled.
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM, 45.0F, 4000.0F), 480.0F);
}

TEST(DialogMetricsTest, DialogWidthGrowsWithTheFont)
{
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM * 2.0F, 45.0F, 4000.0F), 960.0F);
}

TEST(DialogMetricsTest, DialogWidthIsClampedToTheViewport)
{
    // Even Huger on a scaled display puts an em around 4x its reference size, which would ask for
    // ~1900px. On a 1000px window the dialog must stay on screen.
    const float wide = computeDialogWidth(REFERENCE_EM * 4.0F, 45.0F, 1000.0F);
    EXPECT_FLOAT_EQ(wide, 900.0F);
    EXPECT_LT(wide, 1000.0F);
}

TEST(DialogMetricsTest, DialogWidthIgnoresAnUnusableViewport)
{
    // A viewport is not available on the frame a popup is first opened in every code path; falling
    // back to the authored width beats collapsing the dialog to nothing.
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM, 45.0F, 0.0F), 480.0F);
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM, 45.0F, -1.0F), 480.0F);
}

TEST(DialogMetricsTest, ActionButtonFloorDecidesForAShortLabel)
{
    // "OK" is far narrower than any floor the dialogs use, so the floor is what decides. The floors
    // themselves are asserted against the production symbols in tests/App/test_DialogGeometry.cpp.
    EXPECT_FLOAT_EQ(computeActionButtonWidth(14.0F, REFERENCE_EM, 11.25F), 120.0F);
    EXPECT_FLOAT_EQ(computeActionButtonWidth(14.0F, REFERENCE_EM, 9.375F), 100.0F);
}

TEST(DialogMetricsTest, ActionButtonGrowsForALabelWiderThanTheFloor)
{
    // A translated label, or simply a long one, must not be clipped by the floor.
    const float emPx = 10.0F;
    const float wideLabel = 200.0F;
    // 200 + 2*1em = 220, against a floor of 9.375*10 = 93.75.
    EXPECT_FLOAT_EQ(computeActionButtonWidth(wideLabel, emPx, 9.375F), 220.0F);
}

TEST(DialogMetricsTest, ActionButtonScalesWithTheFontWhenTheFloorDecides)
{
    EXPECT_FLOAT_EQ(computeActionButtonWidth(14.0F, REFERENCE_EM * 2.0F, 11.25F), 240.0F);
}

TEST(DialogMetricsTest, MetricsSurviveDegenerateInputs)
{
    // A font size of zero happens before any font is pushed; returning zero or a negative extent
    // would be handed straight to ImGui.
    EXPECT_GT(computeDialogWidth(0.0F, 45.0F, 1000.0F), 0.0F);
    EXPECT_GE(computeActionButtonWidth(-1.0F, 0.0F, -1.0F), 0.0F);
    EXPECT_GT(computeActionButtonWidth(0.0F, REFERENCE_EM, 11.25F), 0.0F);
}

TEST(DialogMetricsTest, ValueColumnStartsAfterTheWidestLabel)
{
    EXPECT_FLOAT_EQ(computeValueColumnStart(120.0F, 16.0F), 136.0F);
}

TEST(DialogMetricsTest, ValueColumnToleratesAnUnmeasuredLabel)
{
    // CalcTextSize returns zero before a font is pushed; a negative column start would be handed to
    // ImGui::SameLine().
    EXPECT_GE(computeValueColumnStart(0.0F, 16.0F), 0.0F);
    EXPECT_GE(computeValueColumnStart(-5.0F, -5.0F), 0.0F);
}

TEST(DialogMetricsTest, NarrowControlSharesTheRightEdgeOfTheWiderOne)
{
    // Settings' performance combos are narrower than its appearance combos and must end flush with
    // them: column 150, wide 250, narrow 150 -> start at 250, so both finish at 400.
    EXPECT_FLOAT_EQ(computeRightAlignedStart(150.0F, 250.0F, 150.0F), 250.0F);
}

TEST(DialogMetricsTest, RightAlignedStartNeverRunsBackOverTheLabels)
{
    // If the "narrow" control ever became the wider of the two -- a longer translation, or an option
    // list that grew -- the naive offset goes left of the value column and draws over the labels.
    EXPECT_FLOAT_EQ(computeRightAlignedStart(150.0F, 150.0F, 250.0F), 150.0F);
    EXPECT_GE(computeRightAlignedStart(150.0F, 100.0F, 400.0F), 150.0F);
}

TEST(DialogMetricsTest, MeasuredWidthPassesThroughWhenItFits)
{
    // The normal case: nothing is capped, so measuring content still decides.
    EXPECT_FLOAT_EQ(computeCappedControlWidth(200.0F, 100.0F, 16.0F, 2000.0F, 60.0F), 200.0F);
}

TEST(DialogMetricsTest, MeasuredWidthIsCappedToTheViewportBudget)
{
    // A user's theme name is unbounded, so the measured width can be absurd. Budget here is
    // 1000*0.9 - 100 - 16 = 784.
    EXPECT_FLOAT_EQ(computeCappedControlWidth(5000.0F, 100.0F, 16.0F, 1000.0F, 60.0F), 784.0F);
}

TEST(DialogMetricsTest, CappedWidthNeverCollapsesBelowAUsableMinimum)
{
    // If the row cannot fit at all, a control clipped at a usable minimum beats one shrunk to
    // nothing: ImGui clips a combo's preview text, so the control stays operable.
    EXPECT_FLOAT_EQ(computeCappedControlWidth(5000.0F, 900.0F, 16.0F, 1000.0F, 60.0F), 60.0F);
    EXPECT_GE(computeCappedControlWidth(5000.0F, 100000.0F, 0.0F, 1000.0F, 60.0F), 60.0F);
}

TEST(DialogMetricsTest, CappedWidthFallsBackWithoutAViewport)
{
    EXPECT_FLOAT_EQ(computeCappedControlWidth(200.0F, 100.0F, 16.0F, 0.0F, 60.0F), 200.0F);
    EXPECT_FLOAT_EQ(computeCappedControlWidth(20.0F, 100.0F, 16.0F, -1.0F, 60.0F), 60.0F);
}

// ========== computeFilledControlWidth (#972) ==========

// The reported case, with the numbers measured at Extra Large on a 175% display: the combos started
// 259px in and needed 272px of their own, ending at 531, while the ADVANCED row made the dialog's
// content run from 21 to 568. The combo must be widened to end where the content does.
TEST(DialogMetricsTest, FilledWidthReachesTheDialogsContentEdge)
{
    const float width = computeFilledControlWidth(/*contentWidthPx=*/272.0F,
                                                  /*controlStartPx=*/259.0F,
                                                  /*contentLeftPx=*/21.0F,
                                                  /*widestOtherRowPx=*/547.0F);
    EXPECT_FLOAT_EQ(width, 309.0F);
    EXPECT_FLOAT_EQ(259.0F + width, 21.0F + 547.0F);
}

// When the control's own row is the widest in the dialog, it keeps its content width: the dialog
// will be exactly that wide and there is nothing to reach for.
TEST(DialogMetricsTest, FilledWidthIsNeverNarrowerThanTheContent)
{
    EXPECT_FLOAT_EQ(computeFilledControlWidth(600.0F, 259.0F, 21.0F, 547.0F), 600.0F);
    EXPECT_FLOAT_EQ(computeFilledControlWidth(309.0F, 259.0F, 21.0F, 547.0F), 309.0F);
}

TEST(DialogMetricsTest, FilledWidthSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(computeFilledControlWidth(272.0F, 259.0F, 21.0F, 0.0F), 272.0F);
    EXPECT_FLOAT_EQ(computeFilledControlWidth(272.0F, 259.0F, 21.0F, nan), 272.0F);
    EXPECT_FLOAT_EQ(computeFilledControlWidth(nan, nan, nan, nan), 0.0F);
    EXPECT_FLOAT_EQ(computeFilledControlWidth(-5.0F, 259.0F, 21.0F, 100.0F), 0.0F);
}

// ---- Keeping dialogs within the viewport (#1129) ----

TEST(DialogMetricsTest, DialogMaxExtentIsTheViewportFraction)
{
    EXPECT_FLOAT_EQ(computeDialogMaxExtent(400.0F), 400.0F * MAX_VIEWPORT_FRACTION);
    EXPECT_LT(computeDialogMaxExtent(400.0F), 400.0F);
}

TEST(DialogMetricsTest, DialogMaxExtentIsUnboundedForAnUnusableViewport)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(computeDialogMaxExtent(0.0F), std::numeric_limits<float>::max());
    EXPECT_EQ(computeDialogMaxExtent(-1.0F), std::numeric_limits<float>::max());
    EXPECT_EQ(computeDialogMaxExtent(nan), std::numeric_limits<float>::max());
}

// ---- A header row that stacks when it doesn't fit (#1490 review) ----

TEST(DialogMetricsTest, SideBySideFitsWhenTheRowHasRoom)
{
    EXPECT_TRUE(fitsSideBySide(300.0F, 64.0F, 16.0F, 128.0F));
    EXPECT_TRUE(fitsSideBySide(208.0F, 64.0F, 16.0F, 128.0F)); // Exactly
}

TEST(DialogMetricsTest, SideBySideStacksWhenTheRowIsTooNarrow)
{
    EXPECT_FALSE(fitsSideBySide(207.0F, 64.0F, 16.0F, 128.0F));
    EXPECT_FALSE(fitsSideBySide(0.0F, 64.0F, 16.0F, 128.0F));
}

TEST(DialogMetricsTest, SideBySideIgnoresUnusableInputs)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(fitsSideBySide(nan, 64.0F, 16.0F, 128.0F));
    EXPECT_TRUE(fitsSideBySide(144.0F, 64.0F, -16.0F, nan));
}

// ---- A compact dialog's own, smaller cap (#1490) ----

TEST(DialogMetricsTest, CompactDialogMaxExtentIsItsOwnFraction)
{
    EXPECT_FLOAT_EQ(computeCompactDialogMaxExtent(900.0F, 0.7F), 630.0F);
    EXPECT_LT(computeCompactDialogMaxExtent(900.0F, 0.7F), computeDialogMaxExtent(900.0F));
}

TEST(DialogMetricsTest, CompactDialogMaxExtentNeverExceedsTheSharedCap)
{
    EXPECT_FLOAT_EQ(computeCompactDialogMaxExtent(900.0F, 1.5F), computeDialogMaxExtent(900.0F));
}

TEST(DialogMetricsTest, CompactDialogMaxExtentFallsBackToTheSharedCapForAnUnusableFraction)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(computeCompactDialogMaxExtent(900.0F, 0.0F), computeDialogMaxExtent(900.0F));
    EXPECT_FLOAT_EQ(computeCompactDialogMaxExtent(900.0F, -0.5F), computeDialogMaxExtent(900.0F));
    EXPECT_FLOAT_EQ(computeCompactDialogMaxExtent(900.0F, nan), computeDialogMaxExtent(900.0F));
}

TEST(DialogMetricsTest, CompactDialogMaxExtentIsUnboundedForAnUnusableViewport)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(computeCompactDialogMaxExtent(0.0F, 0.7F), std::numeric_limits<float>::max());
    EXPECT_EQ(computeCompactDialogMaxExtent(nan, 0.7F), std::numeric_limits<float>::max());
}

// The issue's scenario: a 400px-tall window. The dialog may take 360px; with 120px of title bar,
// padding and button row reserved, the body scrolls past 240px and the buttons stay inside.
TEST(DialogMetricsTest, ScrollableBodyLeavesRoomForThePinnedRows)
{
    const float dialogMax = computeDialogMaxExtent(400.0F);
    const float body = computeScrollableBodyMaxHeight(dialogMax, 120.0F, 40.0F);
    EXPECT_FLOAT_EQ(body, 240.0F);
    EXPECT_LE(body + 120.0F, 400.0F);
}

TEST(DialogMetricsTest, ScrollableBodyKeepsItsFloorWhenNothingFits)
{
    EXPECT_FLOAT_EQ(computeScrollableBodyMaxHeight(100.0F, 120.0F, 40.0F), 40.0F);
}

TEST(DialogMetricsTest, ScrollableBodyIsUnboundedWithoutADialogCap)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(computeScrollableBodyMaxHeight(std::numeric_limits<float>::max(), 120.0F, 40.0F), std::numeric_limits<float>::max());
    EXPECT_EQ(computeScrollableBodyMaxHeight(nan, 120.0F, 40.0F), std::numeric_limits<float>::max());
    EXPECT_EQ(computeScrollableBodyMaxHeight(0.0F, 120.0F, 40.0F), std::numeric_limits<float>::max());
}

TEST(DialogMetricsTest, ScrollableBodySurvivesDegenerateReservations)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(computeScrollableBodyMaxHeight(360.0F, nan, 40.0F), 360.0F);
    EXPECT_FLOAT_EQ(computeScrollableBodyMaxHeight(360.0F, -50.0F, 40.0F), 360.0F);
    EXPECT_FLOAT_EQ(computeScrollableBodyMaxHeight(360.0F, 400.0F, nan), 0.0F);
}
TEST(DialogMetricsTest, ActionButtonsKeepTheirWidthWhenThePairFits)
{
    EXPECT_FLOAT_EQ(fitActionButtonPairWidth(150.0F, 8.0F, 400.0F), 150.0F);
}

TEST(DialogMetricsTest, ActionButtonsShrinkSoThePairFitsANarrowRow)
{
    // #1253 review: at a large font preset the capped dialog is narrower than the Cancel/Apply pair;
    // both shrink equally so the pair (plus the gap) exactly fills the row.
    const float width = fitActionButtonPairWidth(300.0F, 8.0F, 400.0F);
    EXPECT_FLOAT_EQ(width, 196.0F);
    EXPECT_FLOAT_EQ((width * 2.0F) + 8.0F, 400.0F);
}

TEST(DialogMetricsTest, ActionButtonsSurviveDegenerateInputs)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(fitActionButtonPairWidth(150.0F, 8.0F, nan), 150.0F);
    EXPECT_FLOAT_EQ(fitActionButtonPairWidth(150.0F, 8.0F, 0.0F), 150.0F);
    EXPECT_FLOAT_EQ(fitActionButtonPairWidth(150.0F, 8.0F, 4.0F), 0.0F);
    EXPECT_FLOAT_EQ(fitActionButtonPairWidth(nan, 8.0F, 400.0F), 0.0F);
}
} // namespace
} // namespace UI::DialogMetrics
