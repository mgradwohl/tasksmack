#include "App/DialogGeometry.h"
#include "App/Panels/ProcessDetailsPanel_PriorityHelpers.h"
#include "Domain/PriorityConfig.h"
#include "UI/ColorContrast.h"
#include "UI/ThemeLoader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace App::Detail
{
namespace
{

// =============================================================================
// Constants Tests
// =============================================================================

TEST(PriorityHelpersTest, ConstantsAreValid)
{
    // Verify nice value range matches POSIX standard
    EXPECT_EQ(NICE_MIN, -20);
    EXPECT_EQ(NICE_MAX, 19);
    EXPECT_EQ(NICE_RANGE, 39);

    // The gradient segment count is a count, not a size, and stays a plain constant
    EXPECT_GT(PRIORITY_GRADIENT_SEGMENTS, 0.0F);
}

// =============================================================================
// Slider Geometry Tests (#938)
// =============================================================================

// The em multiples were derived from the pixel sizes they replaced, so at the reference em the
// slider must reproduce those sizes exactly. Asserting the header's own constants (not literals
// restated here) is what catches a call-site constant being changed and the control restyling.
TEST(PriorityHelpersTest, SliderMetricsReproduceOriginalPixelsAtReferenceEm)
{
    const PrioritySliderMetrics m = computePrioritySliderMetrics(App::REFERENCE_EM_PX, 0.0F);

    EXPECT_FLOAT_EQ(m.sliderWidth, 400.0F);
    EXPECT_FLOAT_EQ(m.sliderHeight, 12.0F);
    EXPECT_FLOAT_EQ(m.badgeHeight, 24.0F);
    EXPECT_FLOAT_EQ(m.badgeArrowSize, 8.0F);
    EXPECT_FLOAT_EQ(m.sliderCornerRadius, 2.0F);
    EXPECT_FLOAT_EQ(m.badgeCornerRadius, 4.0F);
    EXPECT_FLOAT_EQ(m.thumbOutlineThickness, 2.0F);
    EXPECT_FLOAT_EQ(m.labelPadding, 8.0F);
    EXPECT_FLOAT_EQ(m.thumbRadius, 12.0F * 0.6F);
}

TEST(PriorityHelpersTest, SliderMetricsScaleLinearlyWithEm)
{
    const PrioritySliderMetrics base = computePrioritySliderMetrics(App::REFERENCE_EM_PX, 0.0F);
    const PrioritySliderMetrics doubled = computePrioritySliderMetrics(App::REFERENCE_EM_PX * 2.0F, 0.0F);

    EXPECT_FLOAT_EQ(doubled.sliderWidth, base.sliderWidth * 2.0F);
    EXPECT_FLOAT_EQ(doubled.sliderHeight, base.sliderHeight * 2.0F);
    EXPECT_FLOAT_EQ(doubled.badgeHeight, base.badgeHeight * 2.0F);
    EXPECT_FLOAT_EQ(doubled.badgeArrowSize, base.badgeArrowSize * 2.0F);
    EXPECT_FLOAT_EQ(doubled.sliderCornerRadius, base.sliderCornerRadius * 2.0F);
    EXPECT_FLOAT_EQ(doubled.badgeCornerRadius, base.badgeCornerRadius * 2.0F);
    EXPECT_FLOAT_EQ(doubled.thumbRadius, base.thumbRadius * 2.0F);
    EXPECT_FLOAT_EQ(doubled.thumbOutlineThickness, base.thumbOutlineThickness * 2.0F);
    EXPECT_FLOAT_EQ(doubled.labelPadding, base.labelPadding * 2.0F);
}

// The defect in #938: a 24px badge around text that grows with the font. The text is one em tall,
// so the badge must clear it at every em, including ones far outside the preset range.
TEST(PriorityHelpersTest, BadgeAlwaysTallerThanItsText)
{
    for (const float em : {4.0F, 8.0F, App::REFERENCE_EM_PX, 21.4F, 48.0F, 96.0F})
    {
        const PrioritySliderMetrics m = computePrioritySliderMetrics(em, 0.0F);
        EXPECT_GT(m.badgeHeight, em) << "em=" << em;
    }
}

TEST(PriorityHelpersTest, SliderWidthIsCappedToAvailableSpace)
{
    const float em = 24.0F; // authored width would be 900px
    const PrioritySliderMetrics m = computePrioritySliderMetrics(em, 500.0F);
    EXPECT_FLOAT_EQ(m.sliderWidth, 500.0F);
}

TEST(PriorityHelpersTest, SliderWidthIsNotStretchedToAvailableSpace)
{
    const PrioritySliderMetrics m = computePrioritySliderMetrics(App::REFERENCE_EM_PX, 2000.0F);
    EXPECT_FLOAT_EQ(m.sliderWidth, 400.0F);
}

// The content area does not scroll horizontally, so any width beyond what is available is clipped
// and unreachable. No minimum may win over the available space, however small it is.
TEST(PriorityHelpersTest, SliderWidthNeverExceedsAvailableSpace)
{
    for (const float em : {8.0F, App::REFERENCE_EM_PX, 24.0F, 48.0F})
    {
        for (const float available : {1.0F, 20.0F, 120.0F, 500.0F, 5000.0F})
        {
            const PrioritySliderMetrics m = computePrioritySliderMetrics(em, available);
            EXPECT_LE(m.sliderWidth, available) << "em=" << em << " available=" << available;
            EXPECT_GT(m.sliderWidth, 0.0F) << "em=" << em << " available=" << available;
        }
    }
}

TEST(PriorityHelpersTest, BadgeCenterFollowsThumbWithinTrack)
{
    // Track from x=100 to x=500, badge 40 wide: the centre may range over [120, 480].
    EXPECT_FLOAT_EQ(computeBadgeCenterX(300.0F, 100.0F, 400.0F, 20.0F), 300.0F);
    EXPECT_FLOAT_EQ(computeBadgeCenterX(100.0F, 100.0F, 400.0F, 20.0F), 120.0F);
    EXPECT_FLOAT_EQ(computeBadgeCenterX(500.0F, 100.0F, 400.0F, 20.0F), 480.0F);
}

TEST(PriorityHelpersTest, BadgeArrowBaseIsCentredUnderTheThumbAndHeldToTheBadge)
{
    // Badge from x=280 to x=320, arrow half-base 6.
    const BadgeArrowBase centred = computeBadgeArrowBase(300.0F, 6.0F, 280.0F, 320.0F);
    EXPECT_FLOAT_EQ(centred.left, 294.0F);
    EXPECT_FLOAT_EQ(centred.right, 306.0F);

    // Thumb at the track's left end: the badge is clamped to x=100..140 but the tip stays at x=100, so
    // the base starts at the badge's left edge instead of hanging off it.
    const BadgeArrowBase atStart = computeBadgeArrowBase(100.0F, 6.0F, 100.0F, 140.0F);
    EXPECT_FLOAT_EQ(atStart.left, 100.0F);
    EXPECT_FLOAT_EQ(atStart.right, 106.0F);

    const BadgeArrowBase atEnd = computeBadgeArrowBase(500.0F, 6.0F, 460.0F, 500.0F);
    EXPECT_FLOAT_EQ(atEnd.left, 494.0F);
    EXPECT_FLOAT_EQ(atEnd.right, 500.0F);

    // A badge narrower than the arrow: the base is the whole badge.
    const BadgeArrowBase narrow = computeBadgeArrowBase(300.0F, 6.0F, 298.0F, 302.0F);
    EXPECT_FLOAT_EQ(narrow.left, 298.0F);
    EXPECT_FLOAT_EQ(narrow.right, 302.0F);
}

// A track narrower than the badge leaves no valid clamp range; the badge is centred on the track
// rather than handing std::clamp crossed bounds.
TEST(PriorityHelpersTest, BadgeCenterIsTrackCentreWhenTrackIsNarrowerThanBadge)
{
    EXPECT_FLOAT_EQ(computeBadgeCenterX(100.0F, 100.0F, 30.0F, 20.0F), 115.0F);
    EXPECT_FLOAT_EQ(computeBadgeCenterX(130.0F, 100.0F, 30.0F, 20.0F), 115.0F);
    EXPECT_FLOAT_EQ(computeBadgeCenterX(100.0F, 100.0F, 1.0F, 20.0F), 100.5F);
}

TEST(PriorityHelpersTest, BadgeCenterExactFit)
{
    // Badge exactly as wide as the track: one valid position, the centre.
    EXPECT_FLOAT_EQ(computeBadgeCenterX(100.0F, 100.0F, 40.0F, 20.0F), 120.0F);
    EXPECT_FLOAT_EQ(computeBadgeCenterX(140.0F, 100.0F, 40.0F, 20.0F), 120.0F);
}

TEST(PriorityHelpersTest, SliderWidthUnconstrainedWhenAvailableIsNotUsable)
{
    const float authored = PRIORITY_SLIDER_WIDTH_EM * 24.0F;
    EXPECT_FLOAT_EQ(computePrioritySliderMetrics(24.0F, 0.0F).sliderWidth, authored);
    EXPECT_FLOAT_EQ(computePrioritySliderMetrics(24.0F, -50.0F).sliderWidth, authored);
    EXPECT_FLOAT_EQ(computePrioritySliderMetrics(24.0F, std::numeric_limits<float>::quiet_NaN()).sliderWidth, authored);
    EXPECT_FLOAT_EQ(computePrioritySliderMetrics(24.0F, std::numeric_limits<float>::infinity()).sliderWidth, authored);
}

TEST(PriorityHelpersTest, ThumbOutlineNeverThinnerThanOnePixel)
{
    const PrioritySliderMetrics m = computePrioritySliderMetrics(2.0F, 0.0F);
    EXPECT_FLOAT_EQ(m.thumbOutlineThickness, PRIORITY_THUMB_OUTLINE_MIN_PX);
}

TEST(PriorityHelpersTest, SliderMetricsSurviveDegenerateEm)
{
    for (const float em : {0.0F, -8.0F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        const PrioritySliderMetrics m = computePrioritySliderMetrics(em, 300.0F);
        EXPECT_TRUE(std::isfinite(m.sliderWidth));
        EXPECT_GT(m.sliderWidth, 0.0F);
        EXPECT_GT(m.sliderHeight, 0.0F);
        EXPECT_GT(m.badgeHeight, 0.0F);
    }
}

// =============================================================================
// getNicePosition Tests
// =============================================================================

TEST(PriorityHelpersTest, GetNicePositionBoundaryValues)
{
    // Minimum nice (-20) should be at position 0.0
    EXPECT_FLOAT_EQ(getNicePosition(NICE_MIN), 0.0F);

    // Maximum nice (19) should be at position 1.0
    EXPECT_FLOAT_EQ(getNicePosition(NICE_MAX), 1.0F);

    // Default nice (0) should be at approximately 0.5128 (20/39)
    const float expectedZeroPos = 20.0F / 39.0F;
    EXPECT_NEAR(getNicePosition(0), expectedZeroPos, 0.001F);
}

TEST(PriorityHelpersTest, GetNicePositionClampsOutOfRange)
{
    // Values below minimum should clamp to 0.0
    EXPECT_FLOAT_EQ(getNicePosition(-100), 0.0F);
    EXPECT_FLOAT_EQ(getNicePosition(-21), 0.0F);

    // Values above maximum should clamp to 1.0
    EXPECT_FLOAT_EQ(getNicePosition(100), 1.0F);
    EXPECT_FLOAT_EQ(getNicePosition(20), 1.0F);
}

TEST(PriorityHelpersTest, GetNicePositionIsMonotonic)
{
    // Position should increase as nice value increases
    float prevPos = -1.0F;
    for (int32_t nice = NICE_MIN; nice <= NICE_MAX; ++nice)
    {
        const float pos = getNicePosition(nice);
        EXPECT_GT(pos, prevPos) << "Position should increase for nice=" << nice;
        prevPos = pos;
    }
}

// =============================================================================
// getNiceFromPosition Tests
// =============================================================================

TEST(PriorityHelpersTest, GetNiceFromPositionBoundaryValues)
{
    // Position 0.0 should give minimum nice (-20)
    EXPECT_EQ(getNiceFromPosition(0.0F), NICE_MIN);

    // Position 1.0 should give maximum nice (19)
    EXPECT_EQ(getNiceFromPosition(1.0F), NICE_MAX);

    // Position 0.5128 (20/39) should give nice 0
    const float zeroPos = 20.0F / 39.0F;
    EXPECT_EQ(getNiceFromPosition(zeroPos), 0);
}

TEST(PriorityHelpersTest, GetNiceFromPositionClampsOutOfRange)
{
    // Negative positions should clamp to minimum nice
    EXPECT_EQ(getNiceFromPosition(-0.5F), NICE_MIN);
    EXPECT_EQ(getNiceFromPosition(-1.0F), NICE_MIN);

    // Positions above 1.0 should clamp to maximum nice
    EXPECT_EQ(getNiceFromPosition(1.5F), NICE_MAX);
    EXPECT_EQ(getNiceFromPosition(2.0F), NICE_MAX);
}

TEST(PriorityHelpersTest, GetNiceFromPositionRoundTrip)
{
    // Converting nice -> position -> nice should give the same value
    for (int32_t nice = NICE_MIN; nice <= NICE_MAX; ++nice)
    {
        const float pos = getNicePosition(nice);
        const int32_t roundTrip = getNiceFromPosition(pos);
        EXPECT_EQ(roundTrip, nice) << "Round trip failed for nice=" << nice;
    }
}

// Default test colors matching the legacy hardcoded values
static const ImVec4 TEST_HIGH = {1.0F, 0.3F, 0.2F, 1.0F};   // Red/orange
static const ImVec4 TEST_NORMAL = {0.5F, 0.8F, 0.2F, 1.0F}; // Green
static const ImVec4 TEST_LOW = {0.4F, 0.4F, 0.8F, 1.0F};    // Blue

// =============================================================================
// getNiceColor Tests
// =============================================================================

TEST(PriorityHelpersTest, GetNiceColorReturnsNonZeroAlpha)
{
    // All colors should have full alpha (255)
    for (int32_t nice = NICE_MIN; nice <= NICE_MAX; ++nice)
    {
        const ImU32 color = getNiceColor(nice, TEST_HIGH, TEST_NORMAL, TEST_LOW);
        const uint8_t alpha = (color >> 24) & 0xFF;
        EXPECT_EQ(alpha, 255) << "Alpha should be 255 for nice=" << nice;
    }
}

TEST(PriorityHelpersTest, GetNiceColorHighPriorityIsReddish)
{
    const ImU32 color = getNiceColor(NICE_MIN, TEST_HIGH, TEST_NORMAL, TEST_LOW);
    const uint8_t r = (color >> 0) & 0xFF;
    const uint8_t g = (color >> 8) & 0xFF;
    const uint8_t b = (color >> 16) & 0xFF;

    // High priority (-20) should be predominantly red
    EXPECT_GT(r, g) << "Red should be greater than green at nice=-20";
    EXPECT_GT(r, b) << "Red should be greater than blue at nice=-20";
}

TEST(PriorityHelpersTest, GetNiceColorNormalPriorityIsGreenish)
{
    const ImU32 color = getNiceColor(0, TEST_HIGH, TEST_NORMAL, TEST_LOW);
    const uint8_t r = (color >> 0) & 0xFF;
    const uint8_t g = (color >> 8) & 0xFF;
    const uint8_t b = (color >> 16) & 0xFF;

    // Normal priority (0) should be predominantly green
    EXPECT_GT(g, r) << "Green should be greater than red at nice=0";
    EXPECT_GT(g, b) << "Green should be greater than blue at nice=0";
}

TEST(PriorityHelpersTest, GetNiceColorLowPriorityIsBluish)
{
    const ImU32 color = getNiceColor(NICE_MAX, TEST_HIGH, TEST_NORMAL, TEST_LOW);
    const uint8_t r = (color >> 0) & 0xFF;
    const uint8_t g = (color >> 8) & 0xFF;
    const uint8_t b = (color >> 16) & 0xFF;

    // Low priority (19) should be predominantly blue
    EXPECT_GT(b, r) << "Blue should be greater than red at nice=19";
    EXPECT_GE(b, g) << "Blue should be >= green at nice=19";
}

TEST(PriorityHelpersTest, GetNiceColorClampsOutOfRange)
{
    // Colors for out-of-range values should match boundary colors
    EXPECT_EQ(getNiceColor(-100, TEST_HIGH, TEST_NORMAL, TEST_LOW), getNiceColor(NICE_MIN, TEST_HIGH, TEST_NORMAL, TEST_LOW));
    EXPECT_EQ(getNiceColor(-21, TEST_HIGH, TEST_NORMAL, TEST_LOW), getNiceColor(NICE_MIN, TEST_HIGH, TEST_NORMAL, TEST_LOW));
    EXPECT_EQ(getNiceColor(100, TEST_HIGH, TEST_NORMAL, TEST_LOW), getNiceColor(NICE_MAX, TEST_HIGH, TEST_NORMAL, TEST_LOW));
    EXPECT_EQ(getNiceColor(20, TEST_HIGH, TEST_NORMAL, TEST_LOW), getNiceColor(NICE_MAX, TEST_HIGH, TEST_NORMAL, TEST_LOW));
}

// =============================================================================
// getPriorityLabel Tests (Domain::Priority version)
// =============================================================================

TEST(PriorityHelpersTest, GetPriorityLabelReturnsNonEmpty)
{
    using Domain::Priority::getPriorityLabel;
    for (int32_t nice = NICE_MIN; nice <= NICE_MAX; ++nice)
    {
        const auto label = getPriorityLabel(nice);
        EXPECT_FALSE(label.empty()) << "Label should not be empty for nice=" << nice;
    }
}

TEST(PriorityHelpersTest, GetPriorityLabelCategories)
{
    using Domain::Priority::getPriorityLabel;

    // High priority (nice < -10)
    EXPECT_EQ(getPriorityLabel(-20), "High");
    EXPECT_EQ(getPriorityLabel(-15), "High");
    EXPECT_EQ(getPriorityLabel(-11), "High");

    // Above normal (-10 <= nice <= -6). Note: -5 and higher are Normal.
    EXPECT_EQ(getPriorityLabel(-10), "Above Normal");
    EXPECT_EQ(getPriorityLabel(-7), "Above Normal");
    EXPECT_EQ(getPriorityLabel(-6), "Above Normal");

    // Normal (-5 <= nice <= 4). Boundary: nice < ABOVE_NORMAL_THRESHOLD (-5) is Above Normal.
    EXPECT_EQ(getPriorityLabel(-5), "Normal");
    EXPECT_EQ(getPriorityLabel(-4), "Normal");
    EXPECT_EQ(getPriorityLabel(0), "Normal");
    EXPECT_EQ(getPriorityLabel(4), "Normal");

    // Below normal (5 <= nice <= 14)
    EXPECT_EQ(getPriorityLabel(5), "Below Normal");
    EXPECT_EQ(getPriorityLabel(10), "Below Normal");
    EXPECT_EQ(getPriorityLabel(14), "Below Normal");

    // Idle (nice >= 15)
    EXPECT_EQ(getPriorityLabel(15), "Idle");
    EXPECT_EQ(getPriorityLabel(19), "Idle");
}

// =============================================================================
// Badge Text Contrast Tests (#1130)
// =============================================================================

auto bundledThemes() -> std::vector<std::filesystem::path>
{
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(TASKSMACK_SOURCE_THEMES_DIR))
    {
        if (entry.path().extension() == ".toml")
        {
            paths.push_back(entry.path());
        }
    }
    std::ranges::sort(paths);
    return paths;
}

constexpr float THUMB_MIN_CONTRAST = 3.0F; // WCAG 1.4.11, a non-text control on what surrounds it

TEST(PriorityHelpersTest, UnpackColorInvertsGetNiceColor)
{
    const ImVec4 high{1.0F, 0.0F, 0.0F, 1.0F};
    const ImVec4 normal{0.0F, 1.0F, 0.0F, 1.0F};
    const ImVec4 low{0.0F, 0.0F, 1.0F, 1.0F};
    const ImVec4 atNormal = unpackColor(getNiceColor(0, high, normal, low));
    EXPECT_FLOAT_EQ(atNormal.x, 0.0F);
    EXPECT_FLOAT_EQ(atNormal.y, 1.0F);
    EXPECT_FLOAT_EQ(atNormal.z, 0.0F);
    EXPECT_FLOAT_EQ(atNormal.w, 1.0F);
}

// The regression the issue reported: Arctic Fire's fixed white badge text on its #00E676 nice-0 badge.
TEST(PriorityHelpersTest, FixedBadgeTextWasUnreadableOnArcticFireNormal)
{
    const ImVec4 emerald{0.0F, 230.0F / 255.0F, 118.0F / 255.0F, 1.0F};
    const ImVec4 white{1.0F, 1.0F, 1.0F, 1.0F};
    const ImVec4 windowBg{20.0F / 255.0F, 26.0F / 255.0F, 36.0F / 255.0F, 1.0F};

    EXPECT_LT(UI::ColorContrast::contrastRatio(white, emerald), PRIORITY_BADGE_TEXT_MIN_CONTRAST);
    EXPECT_GE(UI::ColorContrast::contrastRatio(badgeTextFor(emerald, white, windowBg), emerald), PRIORITY_BADGE_TEXT_MIN_CONTRAST);
}

// Neither pole readable (two mid greys on a mid grey): black or white, whichever is better, wins.
// #1252 review: the theme's badge text is kept whenever it is readable, even if its window background
// would contrast more; the background is used only when the badge text fails the floor.
TEST(PriorityHelpersTest, BadgeTextKeepsAReadablePreferredColourInOrder)
{
    const ImVec4 white{1.0F, 1.0F, 1.0F, 1.0F};
    const ImVec4 black{0.0F, 0.0F, 0.0F, 1.0F};
    // On a dark #222222 fill, grey #8A8A8A is about 4.6:1 -- readable -- while white is about 15.9:1.
    // readableTextOn()'s "clearly better" margin swapped to white; the documented order keeps grey.
    const ImVec4 darkFill{34.0F / 255.0F, 34.0F / 255.0F, 34.0F / 255.0F, 1.0F};
    const ImVec4 grey{138.0F / 255.0F, 138.0F / 255.0F, 138.0F / 255.0F, 1.0F};
    ASSERT_GE(UI::ColorContrast::contrastRatio(grey, darkFill), PRIORITY_BADGE_TEXT_MIN_CONTRAST);
    ASSERT_FLOAT_EQ(UI::ColorContrast::readableTextOn(darkFill, grey, white).x, 1.0F); // The old choice
    EXPECT_FLOAT_EQ(badgeTextFor(darkFill, grey, white).x, grey.x);

    // A light fill white fails on: the alternate (black) is used.
    const ImVec4 lightGreen{0.0F, 0.9F, 0.46F, 1.0F};
    ASSERT_LT(UI::ColorContrast::contrastRatio(white, lightGreen), PRIORITY_BADGE_TEXT_MIN_CONTRAST);
    const ImVec4 switched = badgeTextFor(lightGreen, white, black);
    EXPECT_FLOAT_EQ(switched.x, 0.0F);
}

TEST(PriorityHelpersTest, BadgeTextFallsBackToBlackOrWhite)
{
    const ImVec4 grey{0.5F, 0.5F, 0.5F, 1.0F};
    const ImVec4 lighter{0.6F, 0.6F, 0.6F, 1.0F};
    const ImVec4 darker{0.4F, 0.4F, 0.4F, 1.0F};
    const ImVec4 chosen = badgeTextFor(grey, lighter, darker);
    EXPECT_FLOAT_EQ(chosen.x, 0.0F); // black: 5.3:1 on mid grey against white's 3.9:1
    EXPECT_GE(UI::ColorContrast::contrastRatio(chosen, grey), PRIORITY_BADGE_TEXT_MIN_CONTRAST);
}

// Every bundled theme, every nice value: the badge text is readable on the badge, and the thumb stands
// out from the track around it.
TEST(PriorityHelpersTest, BadgeTextAndThumbAreReadableInEveryBundledTheme)
{
    const auto themes = bundledThemes();
    ASSERT_FALSE(themes.empty());
    for (const auto& path : themes)
    {
        const auto scheme = UI::ThemeLoader::loadTheme(path);
        if (!scheme.has_value())
        {
            ADD_FAILURE() << "failed to load " << path;
            continue;
        }
        const auto name = path.stem().string();
        const auto trackAt = [&scheme](int32_t nice)
        {
            return unpackColor(getNiceColor(nice, scheme->priorityHighColor, scheme->priorityNormalColor, scheme->priorityLowColor));
        };

        for (int32_t nice = NICE_MIN; nice <= NICE_MAX; ++nice)
        {
            const ImVec4 fill = trackAt(nice);
            const ImVec4 text = badgeTextFor(fill, scheme->priorityBadgeTextColor, scheme->windowBg);
            EXPECT_GE(UI::ColorContrast::contrastRatio(text, fill), PRIORITY_BADGE_TEXT_MIN_CONTRAST)
                << name << " badge text at nice " << nice;

            // The thumb is ~1.5 nice steps wide, so it also overlaps the track either side of the value.
            for (const int32_t neighbour : {std::max(nice - 1, NICE_MIN), nice, std::min(nice + 1, NICE_MAX)})
            {
                EXPECT_GE(UI::ColorContrast::contrastRatio(text, trackAt(neighbour)), THUMB_MIN_CONTRAST)
                    << name << " thumb at nice " << nice << " on the track at " << neighbour;
            }
        }
    }
}

// =============================================================================
// Windows priority classes (#1204)
// =============================================================================

TEST(WindowsPriorityClassTest, TheControlOffersExactlyTheFiveSettableClasses)
{
    ASSERT_EQ(SETTABLE_WINDOWS_PRIORITY_CLASSES.size(), 5U);
    EXPECT_EQ(SETTABLE_WINDOWS_PRIORITY_CLASSES.front(), WindowsPriorityClass::Idle);
    EXPECT_EQ(SETTABLE_WINDOWS_PRIORITY_CLASSES.back(), WindowsPriorityClass::High);
    EXPECT_EQ(std::ranges::count(SETTABLE_WINDOWS_PRIORITY_CLASSES, WindowsPriorityClass::Realtime), 0);
}

TEST(WindowsPriorityClassTest, EveryClassRoundTripsThroughItsNiceValue)
{
    for (const auto priorityClass : {
             WindowsPriorityClass::Idle,
             WindowsPriorityClass::BelowNormal,
             WindowsPriorityClass::Normal,
             WindowsPriorityClass::AboveNormal,
             WindowsPriorityClass::High,
             WindowsPriorityClass::Realtime,
         })
    {
        EXPECT_EQ(windowsPriorityClassFromNice(windowsPriorityClassNice(priorityClass)), priorityClass)
            << windowsPriorityClassName(priorityClass);
    }
}

TEST(WindowsPriorityClassTest, RepresentativeNiceValuesAvoidTheLabelThresholds)
{
    // -5 and -10, the values the probe used to report, are where the next class down starts.
    EXPECT_EQ(Domain::Priority::getPriorityLabel(windowsPriorityClassNice(WindowsPriorityClass::AboveNormal)), "Above Normal");
    EXPECT_EQ(Domain::Priority::getPriorityLabel(windowsPriorityClassNice(WindowsPriorityClass::High)), "High");
    for (const auto priorityClass : SETTABLE_WINDOWS_PRIORITY_CLASSES)
    {
        const int32_t nice = windowsPriorityClassNice(priorityClass);
        EXPECT_NE(nice, Domain::Priority::HIGH_THRESHOLD);
        EXPECT_NE(nice, Domain::Priority::ABOVE_NORMAL_THRESHOLD);
        EXPECT_NE(nice, Domain::Priority::BELOW_NORMAL_THRESHOLD);
        EXPECT_NE(nice, Domain::Priority::IDLE_THRESHOLD);
    }
}

TEST(WindowsPriorityClassTest, NamesMatchTheProcessesTableAndRealtimeIsNamed)
{
    EXPECT_EQ(windowsPriorityClassName(WindowsPriorityClass::Idle), "Idle");
    EXPECT_EQ(windowsPriorityClassName(WindowsPriorityClass::BelowNormal), "Below Normal");
    EXPECT_EQ(windowsPriorityClassName(WindowsPriorityClass::Normal), "Normal");
    EXPECT_EQ(windowsPriorityClassName(WindowsPriorityClass::AboveNormal), "Above Normal");
    EXPECT_EQ(windowsPriorityClassName(WindowsPriorityClass::High), "High");
    EXPECT_EQ(windowsPriorityClassName(WindowsPriorityClass::Realtime), "Realtime");
}

TEST(WindowsPriorityClassTest, AnyNiceValueFallsInOneClass)
{
    EXPECT_EQ(windowsPriorityClassFromNice(-19), WindowsPriorityClass::High);
    EXPECT_EQ(windowsPriorityClassFromNice(-10), WindowsPriorityClass::AboveNormal);
    EXPECT_EQ(windowsPriorityClassFromNice(-5), WindowsPriorityClass::Normal);
    EXPECT_EQ(windowsPriorityClassFromNice(5), WindowsPriorityClass::BelowNormal);
    EXPECT_EQ(windowsPriorityClassFromNice(15), WindowsPriorityClass::Idle);
    EXPECT_EQ(windowsPriorityClassFromNice(-100), WindowsPriorityClass::Realtime);
    EXPECT_EQ(windowsPriorityClassFromNice(100), WindowsPriorityClass::Idle);
}

TEST(WindowsPriorityClassTest, OverviewTextHasNoNiceWordingWithWindowsClasses)
{
    EXPECT_EQ(priorityDisplayText(-7, true), "Above Normal");
    EXPECT_EQ(priorityDisplayText(-20, true), "Realtime");
    EXPECT_EQ(priorityDisplayText(0, true).find("nice"), std::string::npos);
    // Elsewhere the nice value stays.
    EXPECT_EQ(priorityDisplayText(0, false), "Normal (nice: 0)");
    EXPECT_EQ(priorityDisplayText(-20, false), "High (nice: -20)");
}

// =============================================================================
// Discrete-stop slider (#1538)
// =============================================================================

TEST(DiscreteSliderTest, WindowsStopsRoundTripClassToStopToNice)
{
    // High at the left, where nice -20 is on the nice slider; Idle at the right.
    ASSERT_EQ(WINDOWS_PRIORITY_STOPS.size(), SETTABLE_WINDOWS_PRIORITY_CLASSES.size());
    EXPECT_EQ(windowsPriorityClassAtStop(0), WindowsPriorityClass::High);
    EXPECT_EQ(windowsPriorityClassAtStop(4), WindowsPriorityClass::Idle);
    for (const WindowsPriorityClass priorityClass : SETTABLE_WINDOWS_PRIORITY_CLASSES)
    {
        SCOPED_TRACE(std::string(windowsPriorityClassName(priorityClass)));
        const int32_t stop = windowsPriorityStopIndex(priorityClass);
        ASSERT_GE(stop, 0);
        ASSERT_LT(stop, 5);
        EXPECT_EQ(windowsPriorityClassAtStop(stop), priorityClass);
        const PrioritySliderStop& entry = WINDOWS_PRIORITY_STOPS.at(static_cast<std::size_t>(stop));
        EXPECT_EQ(entry.name, windowsPriorityClassName(priorityClass));
        EXPECT_EQ(entry.colorNice, windowsPriorityClassNice(priorityClass));
        EXPECT_TRUE(std::string_view{entry.itemLabel}.contains(entry.name));
        EXPECT_TRUE(std::string_view{entry.itemLabel}.ends_with(WINDOWS_PRIORITY_SLIDER_ID));
        // The stop's nice value maps back to its class, and another stop's pick writes that class's value.
        EXPECT_EQ(windowsPriorityClassFromNice(entry.colorNice), priorityClass);
        EXPECT_EQ(windowsPriorityNiceForStop(stop, Domain::Priority::NORMAL_NICE),
                  priorityClass == WindowsPriorityClass::Normal ? Domain::Priority::NORMAL_NICE : windowsPriorityClassNice(priorityClass));
    }
    // Higher priority toward the left: the stops' nice values rise left to right.
    for (std::size_t i = 1; i < WINDOWS_PRIORITY_STOPS.size(); ++i)
    {
        EXPECT_LT(WINDOWS_PRIORITY_STOPS.at(i - 1).colorNice, WINDOWS_PRIORITY_STOPS.at(i).colorNice);
    }
}

TEST(DiscreteSliderTest, RealtimeIsReportOnly)
{
    EXPECT_EQ(windowsPriorityStopIndex(WindowsPriorityClass::Realtime), PRIORITY_STOP_BEYOND_START);
    EXPECT_EQ(windowsPriorityClassAtStop(PRIORITY_STOP_BEYOND_START), WindowsPriorityClass::Realtime);
    EXPECT_EQ(WINDOWS_PRIORITY_SLIDER.beyondStart, &WINDOWS_REALTIME_STOP);
    EXPECT_EQ(WINDOWS_REALTIME_STOP.name, "Realtime");
    // Left as shown, a Realtime process keeps its own value: no edit.
    const int32_t realtimeNice = windowsPriorityClassNice(WindowsPriorityClass::Realtime);
    EXPECT_EQ(windowsPriorityNiceForStop(PRIORITY_STOP_BEYOND_START, realtimeNice), realtimeNice);
    // No settable stop is Realtime.
    for (const PrioritySliderStop& stop : WINDOWS_PRIORITY_STOPS)
    {
        EXPECT_NE(windowsPriorityClassFromNice(stop.colorNice), WindowsPriorityClass::Realtime);
    }
    // The same class as shown is no edit: the reported value is kept, not the class's representative.
    EXPECT_EQ(windowsPriorityNiceForStop(windowsPriorityStopIndex(WindowsPriorityClass::BelowNormal), 12), 12);
}

TEST(DiscreteSliderTest, PositionsAreEvenAndADragSnapsToTheNearestStop)
{
    EXPECT_FLOAT_EQ(discreteStopPosition(0, 5), 0.0F);
    EXPECT_FLOAT_EQ(discreteStopPosition(2, 5), 0.5F);
    EXPECT_FLOAT_EQ(discreteStopPosition(4, 5), 1.0F);
    EXPECT_FLOAT_EQ(discreteStopPosition(9, 5), 1.0F);
    EXPECT_FLOAT_EQ(discreteStopPosition(0, 1), 0.0F);

    EXPECT_EQ(discreteStopFromPosition(0.0F, 5), 0);
    EXPECT_EQ(discreteStopFromPosition(0.12F, 5), 0);
    EXPECT_EQ(discreteStopFromPosition(0.13F, 5), 1);
    EXPECT_EQ(discreteStopFromPosition(0.49F, 5), 2);
    EXPECT_EQ(discreteStopFromPosition(0.70F, 5), 3);
    EXPECT_EQ(discreteStopFromPosition(0.90F, 5), 4);
    EXPECT_EQ(discreteStopFromPosition(-3.0F, 5), 0); // Past either end: held there
    EXPECT_EQ(discreteStopFromPosition(7.0F, 5), 4);
    EXPECT_EQ(discreteStopFromPosition(std::numeric_limits<float>::quiet_NaN(), 5), 0);
    EXPECT_EQ(discreteStopFromPosition(0.8F, 1), 0);
}

TEST(DiscreteSliderTest, KeysStepAndAreHeldToTheStops)
{
    EXPECT_EQ(stepDiscreteStop(2, -1, 5), 1);
    EXPECT_EQ(stepDiscreteStop(2, 1, 5), 3);
    EXPECT_EQ(stepDiscreteStop(0, -1, 5), 0); // Held at the high end
    EXPECT_EQ(stepDiscreteStop(4, 1, 5), 4);  // And at the low end
    EXPECT_EQ(stepDiscreteStop(2, -5, 5), 0); // Home
    EXPECT_EQ(stepDiscreteStop(2, 5, 5), 4);  // End
    // From beyond the start (Realtime): toward the high end stays; toward the low end lands on a stop.
    EXPECT_EQ(stepDiscreteStop(PRIORITY_STOP_BEYOND_START, -1, 5), PRIORITY_STOP_BEYOND_START);
    EXPECT_EQ(stepDiscreteStop(PRIORITY_STOP_BEYOND_START, -5, 5), PRIORITY_STOP_BEYOND_START);
    EXPECT_EQ(stepDiscreteStop(PRIORITY_STOP_BEYOND_START, 1, 5), 0);
    EXPECT_EQ(stepDiscreteStop(PRIORITY_STOP_BEYOND_START, 5, 5), 4);
    EXPECT_EQ(stepDiscreteStop(3, 1, 0), 3); // No stops: nothing moves
}

TEST(DiscreteSliderTest, TheTrackRunsThroughTheStopsColours)
{
    for (std::size_t i = 0; i < WINDOWS_PRIORITY_STOPS.size(); ++i)
    {
        const float position = discreteStopPosition(static_cast<int32_t>(i), 5);
        EXPECT_EQ(discreteTrackColorNice(position, WINDOWS_PRIORITY_STOPS), WINDOWS_PRIORITY_STOPS.at(i).colorNice);
    }
    // Halfway between Normal (0) and Below Normal (10).
    EXPECT_EQ(discreteTrackColorNice(0.625F, WINDOWS_PRIORITY_STOPS), 5);
    EXPECT_EQ(discreteTrackColorNice(0.5F, {}), Domain::Priority::NORMAL_NICE);
}

TEST(DiscreteSliderTest, ScaleLabelsStayInsideTheTrackAndApart)
{
    EXPECT_FLOAT_EQ(discreteStopLabelX(100.0F, 40.0F, 0, 5), 100.0F); // First: starts at its stop
    EXPECT_FLOAT_EQ(discreteStopLabelX(300.0F, 40.0F, 4, 5), 260.0F); // Last: ends at its stop
    EXPECT_FLOAT_EQ(discreteStopLabelX(200.0F, 40.0F, 2, 5), 180.0F); // Others: centred

    // Five labels 40 wide, 10 apart: the end pairs need 40 + 20 + 10 = 70 per gap, the middle 50.
    const std::array<float, 5> widths{40.0F, 40.0F, 40.0F, 40.0F, 40.0F};
    const auto widthOf = [&widths](std::size_t i)
    {
        return widths.at(i);
    };
    const float minWidth = discreteStopLabelsMinWidth(widths.size(), widthOf, 10.0F);
    EXPECT_FLOAT_EQ(minWidth, 280.0F);
    // At that width no neighbours overlap.
    for (std::size_t i = 0; i + 1 < widths.size(); ++i)
    {
        const auto index = static_cast<int32_t>(i);
        const float rightEdge = discreteStopLabelX(discreteStopPosition(index, 5) * minWidth, widths.at(i), index, 5) + widths.at(i);
        const float nextLeft = discreteStopLabelX(discreteStopPosition(index + 1, 5) * minWidth, widths.at(i + 1), index + 1, 5);
        EXPECT_LE(rightEdge + 10.0F, nextLeft + 0.001F);
    }
    EXPECT_FLOAT_EQ(discreteStopLabelsMinWidth(0, widthOf, 10.0F), 0.0F);
}

} // namespace
} // namespace App::Detail
