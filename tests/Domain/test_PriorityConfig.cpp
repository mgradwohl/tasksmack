// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "Domain/PriorityConfig.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace Domain::Priority
{
namespace
{

// ========== clampNice Tests ==========

TEST(PriorityConfigTest, ClampNiceInRangeValues)
{
    // Values within the valid range should be unchanged
    EXPECT_EQ(clampNice(0), 0);
    EXPECT_EQ(clampNice(-10), -10);
    EXPECT_EQ(clampNice(10), 10);
    EXPECT_EQ(clampNice(-5), -5);
    EXPECT_EQ(clampNice(5), 5);
}

TEST(PriorityConfigTest, ClampNiceBoundaryValues)
{
    // Boundary values should remain unchanged
    EXPECT_EQ(clampNice(MIN_NICE), MIN_NICE);     // -20
    EXPECT_EQ(clampNice(MAX_NICE), MAX_NICE);     // 19
}

TEST(PriorityConfigTest, ClampNiceBelowMinimum)
{
    // Values below MIN_NICE should clamp to MIN_NICE
    EXPECT_EQ(clampNice(-21), MIN_NICE);
    EXPECT_EQ(clampNice(-100), MIN_NICE);
    EXPECT_EQ(clampNice(-1000), MIN_NICE);
}

TEST(PriorityConfigTest, ClampNiceAboveMaximum)
{
    // Values above MAX_NICE should clamp to MAX_NICE
    EXPECT_EQ(clampNice(20), MAX_NICE);
    EXPECT_EQ(clampNice(100), MAX_NICE);
    EXPECT_EQ(clampNice(1000), MAX_NICE);
}

TEST(PriorityConfigTest, ClampNiceWithInt32)
{
    // Test with explicit int32_t type
    EXPECT_EQ(clampNice(std::int32_t{0}), std::int32_t{0});
    EXPECT_EQ(clampNice(std::int32_t{-20}), std::int32_t{-20});
    EXPECT_EQ(clampNice(std::int32_t{19}), std::int32_t{19});
    EXPECT_EQ(clampNice(std::int32_t{-25}), std::int32_t{-20});
    EXPECT_EQ(clampNice(std::int32_t{25}), std::int32_t{19});
}

TEST(PriorityConfigTest, ClampNiceWithInt64)
{
    // Test with larger integer type
    EXPECT_EQ(clampNice(std::int64_t{0}), std::int64_t{0});
    EXPECT_EQ(clampNice(std::int64_t{-20}), std::int64_t{-20});
    EXPECT_EQ(clampNice(std::int64_t{19}), std::int64_t{19});
    EXPECT_EQ(clampNice(std::int64_t{-1000}), std::int64_t{-20});
    EXPECT_EQ(clampNice(std::int64_t{1000}), std::int64_t{19});
}

TEST(PriorityConfigTest, ClampNiceWithInt16)
{
    // Test with smaller integer type
    EXPECT_EQ(clampNice(std::int16_t{0}), std::int16_t{0});
    EXPECT_EQ(clampNice(std::int16_t{-20}), std::int16_t{-20});
    EXPECT_EQ(clampNice(std::int16_t{19}), std::int16_t{19});
    EXPECT_EQ(clampNice(std::int16_t{-30}), std::int16_t{-20});
    EXPECT_EQ(clampNice(std::int16_t{30}), std::int16_t{19});
}

// ========== getPriorityLabel Tests ==========

TEST(PriorityConfigTest, GetPriorityLabelHigh)
{
    // Values below HIGH_THRESHOLD (-10) should return "High"
    EXPECT_EQ(getPriorityLabel(-20), "High");
    EXPECT_EQ(getPriorityLabel(-15), "High");
    EXPECT_EQ(getPriorityLabel(-11), "High");
}

TEST(PriorityConfigTest, GetPriorityLabelAboveNormal)
{
    // Values from HIGH_THRESHOLD (-10) to ABOVE_NORMAL_THRESHOLD (-5) should return "Above Normal"
    EXPECT_EQ(getPriorityLabel(-10), "Above Normal");
    EXPECT_EQ(getPriorityLabel(-9), "Above Normal");
    EXPECT_EQ(getPriorityLabel(-6), "Above Normal");
}

TEST(PriorityConfigTest, GetPriorityLabelNormal)
{
    // Values from ABOVE_NORMAL_THRESHOLD (-5) to BELOW_NORMAL_THRESHOLD (5) should return "Normal"
    EXPECT_EQ(getPriorityLabel(-5), "Normal");
    EXPECT_EQ(getPriorityLabel(-1), "Normal");
    EXPECT_EQ(getPriorityLabel(0), "Normal");
    EXPECT_EQ(getPriorityLabel(1), "Normal");
    EXPECT_EQ(getPriorityLabel(4), "Normal");
}

TEST(PriorityConfigTest, GetPriorityLabelBelowNormal)
{
    // Values from BELOW_NORMAL_THRESHOLD (5) to IDLE_THRESHOLD (15) should return "Below Normal"
    EXPECT_EQ(getPriorityLabel(5), "Below Normal");
    EXPECT_EQ(getPriorityLabel(10), "Below Normal");
    EXPECT_EQ(getPriorityLabel(14), "Below Normal");
}

TEST(PriorityConfigTest, GetPriorityLabelIdle)
{
    // Values at or above IDLE_THRESHOLD (15) should return "Idle"
    EXPECT_EQ(getPriorityLabel(15), "Idle");
    EXPECT_EQ(getPriorityLabel(19), "Idle");
    EXPECT_EQ(getPriorityLabel(100), "Idle");
}

TEST(PriorityConfigTest, GetPriorityLabelBoundaryValues)
{
    // Test exact threshold values to ensure correct classification
    EXPECT_EQ(getPriorityLabel(HIGH_THRESHOLD), "Above Normal");     // -10
    EXPECT_EQ(getPriorityLabel(ABOVE_NORMAL_THRESHOLD), "Normal");   // -5
    EXPECT_EQ(getPriorityLabel(BELOW_NORMAL_THRESHOLD), "Below Normal"); // 5
    EXPECT_EQ(getPriorityLabel(IDLE_THRESHOLD), "Idle");             // 15
}

TEST(PriorityConfigTest, GetPriorityLabelBoundaryMinusOne)
{
    // Test one less than each threshold
    EXPECT_EQ(getPriorityLabel(HIGH_THRESHOLD - 1), "High");         // -11
    EXPECT_EQ(getPriorityLabel(ABOVE_NORMAL_THRESHOLD - 1), "Above Normal"); // -6
    EXPECT_EQ(getPriorityLabel(BELOW_NORMAL_THRESHOLD - 1), "Normal");       // 4
    EXPECT_EQ(getPriorityLabel(IDLE_THRESHOLD - 1), "Below Normal"); // 14
}

TEST(PriorityConfigTest, GetPriorityLabelExtremeValues)
{
    // Test extreme values outside normal range
    EXPECT_EQ(getPriorityLabel(-1000), "High");
    EXPECT_EQ(getPriorityLabel(1000), "Idle");
}

// ========== Combined Tests ==========

TEST(PriorityConfigTest, ClampAndLabelConsistency)
{
    // Verify that clamped values produce expected labels
    EXPECT_EQ(getPriorityLabel(clampNice(-100)), "High");    // Clamped to -20
    EXPECT_EQ(getPriorityLabel(clampNice(100)), "Idle");     // Clamped to 19
    EXPECT_EQ(getPriorityLabel(clampNice(0)), "Normal");     // Unchanged at 0
}

TEST(PriorityConfigTest, ConstantsRelationship)
{
    // Verify that constants are in the expected order
    EXPECT_LT(MIN_NICE, HIGH_THRESHOLD);
    EXPECT_LT(HIGH_THRESHOLD, ABOVE_NORMAL_THRESHOLD);
    EXPECT_LT(ABOVE_NORMAL_THRESHOLD, NORMAL_NICE);
    EXPECT_LT(NORMAL_NICE, BELOW_NORMAL_THRESHOLD);
    EXPECT_LT(BELOW_NORMAL_THRESHOLD, IDLE_THRESHOLD);
    EXPECT_LT(IDLE_THRESHOLD, MAX_NICE);
}

} // namespace
} // namespace Domain::Priority
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
