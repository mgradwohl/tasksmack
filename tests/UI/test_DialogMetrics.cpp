#include "UI/DialogMetrics.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace UI::DialogMetrics
