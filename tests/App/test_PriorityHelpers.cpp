#include "App/Panels/ProcessDetailsPanel_PriorityHelpers.h"
#include "Domain/PriorityConfig.h"

#include <gtest/gtest.h>

#include <cmath>

namespace App::detail
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

    // Verify slider dimensions are positive
    EXPECT_GT(PRIORITY_SLIDER_WIDTH, 0.0F);
    EXPECT_GT(PRIORITY_SLIDER_HEIGHT, 0.0F);
    EXPECT_GT(PRIORITY_BADGE_HEIGHT, 0.0F);
    EXPECT_GT(PRIORITY_BADGE_ARROW_SIZE, 0.0F);
    EXPECT_GT(PRIORITY_GRADIENT_SEGMENTS, 0.0F);
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

// =============================================================================
// getNiceColor Tests
// =============================================================================

TEST(PriorityHelpersTest, GetNiceColorReturnsNonZeroAlpha)
{
    // All colors should have full alpha (255)
    for (int32_t nice = NICE_MIN; nice <= NICE_MAX; ++nice)
    {
        const ImU32 color = getNiceColor(nice);
        const uint8_t alpha = (color >> 24) & 0xFF;
        EXPECT_EQ(alpha, 255) << "Alpha should be 255 for nice=" << nice;
    }
}

TEST(PriorityHelpersTest, GetNiceColorHighPriorityIsReddish)
{
    const ImU32 color = getNiceColor(NICE_MIN);
    const uint8_t r = (color >> 0) & 0xFF;
    const uint8_t g = (color >> 8) & 0xFF;
    const uint8_t b = (color >> 16) & 0xFF;

    // High priority (-20) should be predominantly red
    EXPECT_GT(r, g) << "Red should be greater than green at nice=-20";
    EXPECT_GT(r, b) << "Red should be greater than blue at nice=-20";
}

TEST(PriorityHelpersTest, GetNiceColorNormalPriorityIsGreenish)
{
    const ImU32 color = getNiceColor(0);
    const uint8_t r = (color >> 0) & 0xFF;
    const uint8_t g = (color >> 8) & 0xFF;
    const uint8_t b = (color >> 16) & 0xFF;

    // Normal priority (0) should be predominantly green
    EXPECT_GT(g, r) << "Green should be greater than red at nice=0";
    EXPECT_GT(g, b) << "Green should be greater than blue at nice=0";
}

TEST(PriorityHelpersTest, GetNiceColorLowPriorityIsBluish)
{
    const ImU32 color = getNiceColor(NICE_MAX);
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
    EXPECT_EQ(getNiceColor(-100), getNiceColor(NICE_MIN));
    EXPECT_EQ(getNiceColor(-21), getNiceColor(NICE_MIN));
    EXPECT_EQ(getNiceColor(100), getNiceColor(NICE_MAX));
    EXPECT_EQ(getNiceColor(20), getNiceColor(NICE_MAX));
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

    // Above normal (-10 <= nice < -5)
    EXPECT_EQ(getPriorityLabel(-10), "Above Normal");
    EXPECT_EQ(getPriorityLabel(-7), "Above Normal");
    EXPECT_EQ(getPriorityLabel(-5), "Normal"); // -5 is now Normal boundary

    // Normal (-5 <= nice < 5)
    EXPECT_EQ(getPriorityLabel(-4), "Normal");
    EXPECT_EQ(getPriorityLabel(0), "Normal");
    EXPECT_EQ(getPriorityLabel(4), "Normal");

    // Below normal (5 <= nice < 15)
    EXPECT_EQ(getPriorityLabel(5), "Below Normal");
    EXPECT_EQ(getPriorityLabel(10), "Below Normal");
    EXPECT_EQ(getPriorityLabel(14), "Below Normal");

    // Idle (nice >= 15)
    EXPECT_EQ(getPriorityLabel(15), "Idle");
    EXPECT_EQ(getPriorityLabel(19), "Idle");
}

} // namespace
} // namespace App::detail
