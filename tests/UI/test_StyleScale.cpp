#include "UI/StyleScale.h"

#include <gtest/gtest.h>

#include <limits>

namespace UI
{
namespace
{

TEST(StyleScaleTest, MediumPresetOnAnUnscaledDisplayLeavesTheStyleUntouched)
{
    // The ImGuiStyle literals are authored against exactly this configuration, so the factor must
    // be 1.0 or the default look would shift the moment scaling was introduced.
    EXPECT_FLOAT_EQ(computeStyleScale(STYLE_REFERENCE_PT, 1.0F), 1.0F);
}

TEST(StyleScaleTest, ScaleTracksTheFontPreset)
{
    EXPECT_FLOAT_EQ(computeStyleScale(7.0F, 1.0F), 0.875F); // Small
    EXPECT_FLOAT_EQ(computeStyleScale(10.0F, 1.0F), 1.25F); // Large
    EXPECT_FLOAT_EQ(computeStyleScale(16.0F, 1.0F), 2.0F);  // Even Huger
}

TEST(StyleScaleTest, ScaleTracksDisplayDensity)
{
    EXPECT_FLOAT_EQ(computeStyleScale(STYLE_REFERENCE_PT, 2.0F), 2.0F);
    EXPECT_FLOAT_EQ(computeStyleScale(STYLE_REFERENCE_PT, 1.5F), 1.5F);
}

TEST(StyleScaleTest, FontPresetAndDisplayDensityCompose)
{
    // A large preset on a HiDPI display must scale by both, not by whichever is larger.
    EXPECT_FLOAT_EQ(computeStyleScale(16.0F, 2.0F), 4.0F);
}

TEST(StyleScaleTest, SmallPresetIsAllowedToShrinkTheChrome)
{
    // Regression guard. An earlier version applied the factor through ScaleAllSizes(), whose
    // ImTrunc truncated the 1px border sizes to zero below 1.0, which forced a clamp at 1.0 and
    // left the Small preset with Medium's chrome. Scaling the literals directly removes the need.
    EXPECT_LT(computeStyleScale(7.0F, 1.0F), 1.0F);
}

TEST(StyleScaleTest, DegenerateInputsFallBackInsteadOfCollapsingTheStyle)
{
    // A window that is not mapped yet reports a zero display scale; a zero-size style would make
    // the whole UI unusable, so fall back rather than propagate it.
    EXPECT_FLOAT_EQ(computeStyleScale(STYLE_REFERENCE_PT, 0.0F), 1.0F);
    EXPECT_FLOAT_EQ(computeStyleScale(STYLE_REFERENCE_PT, -1.0F), 1.0F);
    EXPECT_FLOAT_EQ(computeStyleScale(0.0F, 1.0F), 1.0F);
    EXPECT_FLOAT_EQ(computeStyleScale(-6.0F, 1.0F), 1.0F);
}

TEST(StyleScaleTest, ScaleIsNeverBelowTheFloor)
{
    EXPECT_GE(computeStyleScale(0.001F, 0.001F), STYLE_SCALE_MIN);
}

TEST(StyleScaleTest, LineWeightIsUnchangedAtTheReferenceConfiguration)
{
    EXPECT_FLOAT_EQ(scaledLineWeight(2.0F, 1.0F), 2.0F);
    EXPECT_FLOAT_EQ(scaledLineWeight(1.5F, 1.0F), 1.5F);
}

TEST(StyleScaleTest, LineWeightTracksTheStyleScale)
{
    // A 2px series line beside text 2.8 times the reference size (Extra Large on 175%) (#971).
    EXPECT_FLOAT_EQ(scaledLineWeight(2.0F, computeStyleScale(12.0F, 1.75F)), 5.25F);
    EXPECT_FLOAT_EQ(scaledLineWeight(2.0F, 0.875F), 1.75F); // Small
}

TEST(StyleScaleTest, LineWeightIsNeverThinnerThanOnePixel)
{
    EXPECT_FLOAT_EQ(scaledLineWeight(1.0F, 0.75F), LINE_WEIGHT_MIN_PX);
    EXPECT_FLOAT_EQ(scaledLineWeight(2.0F, 0.0F), 2.0F); // a degenerate scale falls back to 1.0
}

TEST(StyleScaleTest, StrokesScaleInWholePixelsAndNeverVanish)
{
    // #1169: the tab overline and underline stayed 1px beside tabs twice the size.
    EXPECT_FLOAT_EQ(scaledStrokePx(1.0F, 1.0F), 1.0F);
    EXPECT_FLOAT_EQ(scaledStrokePx(1.0F, 1.75F), 2.0F);
    EXPECT_FLOAT_EQ(scaledStrokePx(1.0F, 2.8F), 3.0F);
    EXPECT_FLOAT_EQ(scaledStrokePx(3.0F, 1.5F), 5.0F); // 4.5 rounds away from zero
    EXPECT_FLOAT_EQ(scaledStrokePx(1.0F, 0.875F), 1.0F);
    EXPECT_FLOAT_EQ(scaledStrokePx(1.0F, 0.25F), 1.0F);
    EXPECT_FLOAT_EQ(scaledStrokePx(2.0F, 0.0F), 2.0F);
    EXPECT_FLOAT_EQ(scaledStrokePx(2.0F, std::numeric_limits<float>::quiet_NaN()), 2.0F);
}
} // namespace
} // namespace UI
