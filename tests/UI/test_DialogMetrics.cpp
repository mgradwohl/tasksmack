#include "UI/DialogMetrics.h"

#include <gtest/gtest.h>

namespace UI::DialogMetrics
{
namespace
{

// One em at the reference configuration: the Medium preset (8pt) on a 1.0 display scale, at 96 DPI.
constexpr float REFERENCE_EM = 32.0F / 3.0F;

TEST(DialogMetricsTest, DialogWidthReproducesTheReplacedPixelSizeAtTheReferenceConfiguration)
{
    // The elevation notice was a fixed 480px; 45 em must still be 480px at the reference, or this
    // change would silently restyle a dialog it is only meant to make scale.
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

TEST(DialogMetricsTest, ActionButtonFloorReproducesTheReplacedPixelSizes)
{
    // About was 120px (11.25 em) and the elevation notice 100px (9.375 em). "OK" is far narrower
    // than either, so the floor is what decides both at the reference configuration.
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

} // namespace
} // namespace UI::DialogMetrics
