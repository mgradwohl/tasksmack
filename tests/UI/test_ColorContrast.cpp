/// @file test_ColorContrast.cpp
/// @brief Tests for the pure colour arithmetic in UI/ColorContrast.h (#969).

#include "UI/ColorContrast.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cmath>
#include <limits>

namespace UI::ColorContrast
{
namespace
{

constexpr ImVec4 BLACK{0.0F, 0.0F, 0.0F, 1.0F};
constexpr ImVec4 WHITE{1.0F, 1.0F, 1.0F, 1.0F};

[[nodiscard]] ImVec4 rgb(int r, int g, int b)
{
    return {static_cast<float>(r) / 255.0F, static_cast<float>(g) / 255.0F, static_cast<float>(b) / 255.0F, 1.0F};
}

// The anchors of the WCAG definitions.
TEST(ColorContrastTest, LuminanceOfBlackAndWhite)
{
    EXPECT_NEAR(relativeLuminance(BLACK), 0.0F, 1e-6F);
    EXPECT_NEAR(relativeLuminance(WHITE), 1.0F, 1e-5F);
}

// Green carries most of the luminance, blue the least: the weighting that makes a "dark" green
// fill brighter than it looks from its hex value.
TEST(ColorContrastTest, LuminanceWeightsGreenOverRedOverBlue)
{
    const float red = relativeLuminance(rgb(255, 0, 0));
    const float green = relativeLuminance(rgb(0, 255, 0));
    const float blue = relativeLuminance(rgb(0, 0, 255));
    EXPECT_GT(green, red);
    EXPECT_GT(red, blue);
    EXPECT_NEAR(red + green + blue, 1.0F, 1e-4F);
}

TEST(ColorContrastTest, ContrastRatioSpansOneToTwentyOne)
{
    EXPECT_NEAR(contrastRatio(BLACK, WHITE), 21.0F, 1e-3F);
    EXPECT_NEAR(contrastRatio(WHITE, WHITE), 1.0F, 1e-5F);
    EXPECT_NEAR(contrastRatio(rgb(74, 102, 0), rgb(74, 102, 0)), 1.0F, 1e-5F);
}

TEST(ColorContrastTest, ContrastRatioIsSymmetric)
{
    const ImVec4 a = rgb(74, 102, 0);
    const ImVec4 b = rgb(253, 246, 227);
    EXPECT_FLOAT_EQ(contrastRatio(a, b), contrastRatio(b, a));
}

// Alpha plays no part: the same colour at any alpha has the same luminance.
TEST(ColorContrastTest, AlphaIsIgnored)
{
    ImVec4 translucent = rgb(74, 102, 0);
    translucent.w = 0.2F;
    EXPECT_FLOAT_EQ(relativeLuminance(translucent), relativeLuminance(rgb(74, 102, 0)));
}

// The reported case, with Solarized Light's own colours: a dark green Apply button (#4A6600)
// labelled in the theme's dark text (#586E75) had a contrast ratio under 1.5. The window background
// (#FDF6E3) is the readable one, and must be chosen.
TEST(ColorContrastTest, PicksTheWindowBackgroundOnSolarizedLightsApplyButton)
{
    const ImVec4 fill = rgb(0x4A, 0x66, 0x00);
    const ImVec4 text = rgb(0x58, 0x6E, 0x75);
    const ImVec4 windowBg = rgb(0xFD, 0xF6, 0xE3);

    ASSERT_LT(contrastRatio(fill, text), 1.6F);
    const ImVec4 chosen = readableTextOn(fill, text, windowBg);
    EXPECT_FLOAT_EQ(chosen.x, windowBg.x);
    EXPECT_FLOAT_EQ(chosen.y, windowBg.y);
    EXPECT_FLOAT_EQ(chosen.z, windowBg.z);
    EXPECT_GT(contrastRatio(fill, chosen), 4.5F);
}

// Where the ordinary text colour is already the better of the two, it is kept.
TEST(ColorContrastTest, KeepsThePreferredColourWhenItReadsBetter)
{
    const ImVec4 fill = rgb(0x20, 0x60, 0x20);
    const ImVec4 chosen = readableTextOn(fill, WHITE, BLACK);
    EXPECT_FLOAT_EQ(chosen.x, 1.0F);
}

TEST(ColorContrastTest, PrefersTheFirstCandidateOnATie)
{
    const ImVec4 fill = rgb(120, 120, 120);
    const ImVec4 preferred{0.9F, 0.9F, 0.9F, 1.0F};
    const ImVec4 same{0.9F, 0.9F, 0.9F, 0.5F}; // same luminance, distinguishable by alpha
    EXPECT_FLOAT_EQ(readableTextOn(fill, preferred, same).w, 1.0F);
}

// A marginal gain does not flip the label. On a mid-grey fill white reads about 4.5 and black about
// 4.7: black is better, but not by the margin, so the preferred white stays. Tokyo Night's Apply
// button is the real instance (3.25 with the theme's light text, 3.26 with its dark background).
TEST(ColorContrastTest, KeepsThePreferredColourWhenTheAlternateIsOnlyMarginallyBetter)
{
    const ImVec4 fill = rgb(119, 119, 119);
    const ImVec4 preferred = WHITE;
    const ImVec4 alternate = BLACK;
    const float preferredRatio = contrastRatio(fill, preferred);
    const float alternateRatio = contrastRatio(fill, alternate);
    ASSERT_GT(alternateRatio, preferredRatio);                    // the alternate is better...
    ASSERT_LT(alternateRatio, preferredRatio * ALTERNATE_MARGIN); // ...but not by the margin
    EXPECT_FLOAT_EQ(readableTextOn(fill, preferred, alternate).x, preferred.x);
}

// The reason the label colour is chosen per state rather than once (see UI::Widgets::filledButton):
// Tokyo Night's Apply button, with the theme's own colours. Its light text is fine on the resting
// fill and nearly invisible on the hovered one, where the dark window background is the readable
// choice. One colour cannot serve both.
TEST(ColorContrastTest, TheRightLabelColourDiffersBetweenAButtonsStates)
{
    const ImVec4 text = rgb(0xC0, 0xCA, 0xF5);
    const ImVec4 windowBg = rgb(0x1A, 0x1B, 0x26);
    const ImVec4 resting = rgb(0x4A, 0x78, 0x25);
    const ImVec4 hovered = rgb(0x9E, 0xCE, 0x6A);

    EXPECT_FLOAT_EQ(readableTextOn(resting, text, windowBg).x, text.x);
    EXPECT_FLOAT_EQ(readableTextOn(hovered, text, windowBg).x, windowBg.x);
    EXPECT_LT(contrastRatio(hovered, text), 1.5F);
    EXPECT_GT(contrastRatio(hovered, windowBg), 7.0F);
}
// ========== flattenOver ==========

TEST(ColorContrastTest, FlatteningAnOpaqueColourReturnsIt)
{
    const ImVec4 flat = flattenOver(rgb(238, 232, 213), BLACK);
    EXPECT_FLOAT_EQ(flat.x, 238.0F / 255.0F);
    EXPECT_FLOAT_EQ(flat.y, 232.0F / 255.0F);
    EXPECT_FLOAT_EQ(flat.z, 213.0F / 255.0F);
    EXPECT_FLOAT_EQ(flat.w, 1.0F);
}

TEST(ColorContrastTest, FlatteningIsAlwaysOpaque)
{
    ImVec4 translucent = WHITE;
    translucent.w = 0.25F;
    ImVec4 translucentBottom = BLACK;
    translucentBottom.w = 0.1F; // the bottom's alpha plays no part
    const ImVec4 flat = flattenOver(translucent, translucentBottom);
    EXPECT_FLOAT_EQ(flat.w, 1.0F);
    EXPECT_FLOAT_EQ(flat.x, 0.25F);
}

// The reason this is not simply "set alpha to 1": Solarized Light's popup (#EEE8D5 at 94%) is the
// same colour as its buttons and combos (#EEE8D5). Made opaque as-is, those controls vanish into
// the dialog. Flattened over the dimmed window it is normally seen against, the popup comes out a
// little darker than the controls, as it always looked.
TEST(ColorContrastTest, FlattenedPopupStaysDistinctFromControlsOfTheSameNominalColour)
{
    ImVec4 popup = rgb(0xEE, 0xE8, 0xD5);
    popup.w = 240.0F / 255.0F; // F0
    ImVec4 dim = BLACK;
    dim.w = 153.0F / 255.0F; // 99
    const ImVec4 windowBg = rgb(0xFD, 0xF6, 0xE3);
    const ImVec4 control = rgb(0xEE, 0xE8, 0xD5);

    const ImVec4 flat = flattenOver(popup, flattenOver(dim, windowBg));
    EXPECT_FLOAT_EQ(flat.w, 1.0F);
    EXPECT_LT(relativeLuminance(flat), relativeLuminance(control));
    EXPECT_LT(flat.x, control.x - 0.02F); // visibly darker, not a rounding difference
}

TEST(ColorContrastTest, FlatteningSurvivesDegenerateAlpha)
{
    ImVec4 odd = WHITE;
    odd.w = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(flattenOver(odd, BLACK).x, 1.0F);
    odd.w = 5.0F;
    EXPECT_FLOAT_EQ(flattenOver(odd, BLACK).x, 1.0F);
    odd.w = -1.0F;
    EXPECT_FLOAT_EQ(flattenOver(odd, BLACK).x, 0.0F);
}

// Out-of-range and non-finite channels are treated as clamped, never propagated as NaN.
TEST(ColorContrastTest, SurvivesDegenerateChannels)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ImVec4 garbage{nan, 7.0F, -3.0F, 1.0F};
    const float luminance = relativeLuminance(garbage);
    EXPECT_TRUE(std::isfinite(luminance));
    EXPECT_GE(luminance, 0.0F);
    EXPECT_LE(luminance, 1.0F);
    EXPECT_TRUE(std::isfinite(contrastRatio(garbage, WHITE)));
}

} // namespace
} // namespace UI::ColorContrast
