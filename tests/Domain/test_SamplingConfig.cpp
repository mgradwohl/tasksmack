// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "Domain/SamplingConfig.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace Domain::Sampling
{
namespace
{

// ========== Constants Tests ==========

TEST(SamplingConfigTest, DefaultsAreValid)
{
    // Verify defaults are within valid ranges
    EXPECT_GE(REFRESH_INTERVAL_DEFAULT_MS, REFRESH_INTERVAL_MIN_MS);
    EXPECT_LE(REFRESH_INTERVAL_DEFAULT_MS, REFRESH_INTERVAL_MAX_MS);

    EXPECT_GE(HISTORY_SECONDS_DEFAULT, HISTORY_SECONDS_MIN);
    EXPECT_LE(HISTORY_SECONDS_DEFAULT, HISTORY_SECONDS_MAX);
}

TEST(SamplingConfigTest, RefreshIntervalsArePositive)
{
    EXPECT_GT(REFRESH_INTERVAL_MIN_MS, 0);
    EXPECT_GT(REFRESH_INTERVAL_MAX_MS, 0);
    EXPECT_GT(REFRESH_INTERVAL_DEFAULT_MS, 0);
}

TEST(SamplingConfigTest, HistorySecondsArePositive)
{
    EXPECT_GT(HISTORY_SECONDS_MIN, 0);
    EXPECT_GT(HISTORY_SECONDS_MAX, 0);
    EXPECT_GT(HISTORY_SECONDS_DEFAULT, 0);
}

TEST(SamplingConfigTest, CommonRefreshIntervalsAreInRange)
{
    for (int interval : COMMON_REFRESH_INTERVALS_MS)
    {
        EXPECT_GE(interval, REFRESH_INTERVAL_MIN_MS);
        EXPECT_LE(interval, REFRESH_INTERVAL_MAX_MS);
    }
}

TEST(SamplingConfigTest, LinkSpeedCacheTtlIsPositive)
{
    EXPECT_GT(LINK_SPEED_CACHE_TTL_SECONDS, 0);
}

// ========== clampRefreshInterval Tests ==========

TEST(SamplingConfigTest, ClampRefreshIntervalInRange)
{
    // Values within range should be unchanged
    EXPECT_EQ(clampRefreshInterval(500), 500);
    EXPECT_EQ(clampRefreshInterval(1000), 1000);
    EXPECT_EQ(clampRefreshInterval(REFRESH_INTERVAL_MIN_MS), REFRESH_INTERVAL_MIN_MS);
    EXPECT_EQ(clampRefreshInterval(REFRESH_INTERVAL_MAX_MS), REFRESH_INTERVAL_MAX_MS);
}

TEST(SamplingConfigTest, ClampRefreshIntervalBelowMin)
{
    // Values below minimum should clamp to minimum
    EXPECT_EQ(clampRefreshInterval(0), REFRESH_INTERVAL_MIN_MS);
    EXPECT_EQ(clampRefreshInterval(50), REFRESH_INTERVAL_MIN_MS);
    EXPECT_EQ(clampRefreshInterval(-100), REFRESH_INTERVAL_MIN_MS);
}

TEST(SamplingConfigTest, ClampRefreshIntervalAboveMax)
{
    // Values above maximum should clamp to maximum
    EXPECT_EQ(clampRefreshInterval(6000), REFRESH_INTERVAL_MAX_MS);
    EXPECT_EQ(clampRefreshInterval(10000), REFRESH_INTERVAL_MAX_MS);
    EXPECT_EQ(clampRefreshInterval(100000), REFRESH_INTERVAL_MAX_MS);
}

TEST(SamplingConfigTest, ClampRefreshIntervalWithDifferentTypes)
{
    // Test with different integer types
    EXPECT_EQ(clampRefreshInterval(500L), 500L);
    EXPECT_EQ(clampRefreshInterval(static_cast<int64_t>(500)), static_cast<int64_t>(500));
    EXPECT_EQ(clampRefreshInterval(static_cast<int16_t>(500)), static_cast<int16_t>(500));

    // Verify clamping works with different types
    EXPECT_EQ(clampRefreshInterval(0L), static_cast<long>(REFRESH_INTERVAL_MIN_MS));
    EXPECT_EQ(clampRefreshInterval(static_cast<int64_t>(10000)), static_cast<int64_t>(REFRESH_INTERVAL_MAX_MS));
}

// ========== clampHistorySeconds Tests ==========

TEST(SamplingConfigTest, ClampHistorySecondsInRange)
{
    // Values within range should be unchanged
    EXPECT_EQ(clampHistorySeconds(60), 60);
    EXPECT_EQ(clampHistorySeconds(300), 300);
    EXPECT_EQ(clampHistorySeconds(HISTORY_SECONDS_MIN), HISTORY_SECONDS_MIN);
    EXPECT_EQ(clampHistorySeconds(HISTORY_SECONDS_MAX), HISTORY_SECONDS_MAX);
}

TEST(SamplingConfigTest, ClampHistorySecondsBelowMin)
{
    // Values below minimum should clamp to minimum
    EXPECT_EQ(clampHistorySeconds(0), HISTORY_SECONDS_MIN);
    EXPECT_EQ(clampHistorySeconds(5), HISTORY_SECONDS_MIN);
    EXPECT_EQ(clampHistorySeconds(-100), HISTORY_SECONDS_MIN);
}

TEST(SamplingConfigTest, ClampHistorySecondsAboveMax)
{
    // Values above maximum should clamp to maximum
    EXPECT_EQ(clampHistorySeconds(2000), HISTORY_SECONDS_MAX);
    EXPECT_EQ(clampHistorySeconds(3600), HISTORY_SECONDS_MAX);
    EXPECT_EQ(clampHistorySeconds(10000), HISTORY_SECONDS_MAX);
}

TEST(SamplingConfigTest, ClampHistorySecondsWithDifferentTypes)
{
    // Test with different integer types
    EXPECT_EQ(clampHistorySeconds(120L), 120L);
    EXPECT_EQ(clampHistorySeconds(static_cast<int64_t>(120)), static_cast<int64_t>(120));
    EXPECT_EQ(clampHistorySeconds(static_cast<int16_t>(120)), static_cast<int16_t>(120));

    // Verify clamping works with different types
    EXPECT_EQ(clampHistorySeconds(0L), static_cast<long>(HISTORY_SECONDS_MIN));
    EXPECT_EQ(clampHistorySeconds(static_cast<int64_t>(10000)), static_cast<int64_t>(HISTORY_SECONDS_MAX));
}

// ========== Edge Cases ==========

TEST(SamplingConfigTest, ClampRefreshIntervalBoundaryValues)
{
    // Test values just at the boundaries
    EXPECT_EQ(clampRefreshInterval(REFRESH_INTERVAL_MIN_MS - 1), REFRESH_INTERVAL_MIN_MS);
    EXPECT_EQ(clampRefreshInterval(REFRESH_INTERVAL_MIN_MS), REFRESH_INTERVAL_MIN_MS);
    EXPECT_EQ(clampRefreshInterval(REFRESH_INTERVAL_MIN_MS + 1), REFRESH_INTERVAL_MIN_MS + 1);

    EXPECT_EQ(clampRefreshInterval(REFRESH_INTERVAL_MAX_MS - 1), REFRESH_INTERVAL_MAX_MS - 1);
    EXPECT_EQ(clampRefreshInterval(REFRESH_INTERVAL_MAX_MS), REFRESH_INTERVAL_MAX_MS);
    EXPECT_EQ(clampRefreshInterval(REFRESH_INTERVAL_MAX_MS + 1), REFRESH_INTERVAL_MAX_MS);
}

TEST(SamplingConfigTest, ClampHistorySecondsBoundaryValues)
{
    // Test values just at the boundaries
    EXPECT_EQ(clampHistorySeconds(HISTORY_SECONDS_MIN - 1), HISTORY_SECONDS_MIN);
    EXPECT_EQ(clampHistorySeconds(HISTORY_SECONDS_MIN), HISTORY_SECONDS_MIN);
    EXPECT_EQ(clampHistorySeconds(HISTORY_SECONDS_MIN + 1), HISTORY_SECONDS_MIN + 1);

    EXPECT_EQ(clampHistorySeconds(HISTORY_SECONDS_MAX - 1), HISTORY_SECONDS_MAX - 1);
    EXPECT_EQ(clampHistorySeconds(HISTORY_SECONDS_MAX), HISTORY_SECONDS_MAX);
    EXPECT_EQ(clampHistorySeconds(HISTORY_SECONDS_MAX + 1), HISTORY_SECONDS_MAX);
}

} // namespace
} // namespace Domain::Sampling

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
