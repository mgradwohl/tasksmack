// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "Domain/SamplingConfig.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

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

// ========== clampSocketStatsCacheTtlMs Tests (Linux Netlink) ==========

TEST(SamplingConfigTest, SocketStatsCacheTtlMsDefaultInRange)
{
    EXPECT_GE(SOCKET_STATS_CACHE_TTL_MS_DEFAULT, SOCKET_STATS_CACHE_TTL_MS_MIN);
    EXPECT_LE(SOCKET_STATS_CACHE_TTL_MS_DEFAULT, SOCKET_STATS_CACHE_TTL_MS_MAX);
}

TEST(SamplingConfigTest, ClampSocketStatsCacheTtlMsInRange)
{
    EXPECT_EQ(clampSocketStatsCacheTtlMs(500), 500);
    EXPECT_EQ(clampSocketStatsCacheTtlMs(SOCKET_STATS_CACHE_TTL_MS_MIN), SOCKET_STATS_CACHE_TTL_MS_MIN);
    EXPECT_EQ(clampSocketStatsCacheTtlMs(SOCKET_STATS_CACHE_TTL_MS_MAX), SOCKET_STATS_CACHE_TTL_MS_MAX);
}

TEST(SamplingConfigTest, ClampSocketStatsCacheTtlMsZeroAllowed)
{
    // 0 is valid (disables caching)
    EXPECT_EQ(clampSocketStatsCacheTtlMs(0), 0);
}

TEST(SamplingConfigTest, ClampSocketStatsCacheTtlMsBelowMin)
{
    EXPECT_EQ(clampSocketStatsCacheTtlMs(-1), SOCKET_STATS_CACHE_TTL_MS_MIN);
    EXPECT_EQ(clampSocketStatsCacheTtlMs(-100), SOCKET_STATS_CACHE_TTL_MS_MIN);
}

TEST(SamplingConfigTest, ClampSocketStatsCacheTtlMsAboveMax)
{
    EXPECT_EQ(clampSocketStatsCacheTtlMs(SOCKET_STATS_CACHE_TTL_MS_MAX + 1), SOCKET_STATS_CACHE_TTL_MS_MAX);
    EXPECT_EQ(clampSocketStatsCacheTtlMs(100000), SOCKET_STATS_CACHE_TTL_MS_MAX);
}

TEST(SamplingConfigTest, ClampSocketStatsCacheTtlMsBoundary)
{
    EXPECT_EQ(clampSocketStatsCacheTtlMs(SOCKET_STATS_CACHE_TTL_MS_MIN), SOCKET_STATS_CACHE_TTL_MS_MIN);
    EXPECT_EQ(clampSocketStatsCacheTtlMs(SOCKET_STATS_CACHE_TTL_MS_MAX - 1), SOCKET_STATS_CACHE_TTL_MS_MAX - 1);
    EXPECT_EQ(clampSocketStatsCacheTtlMs(SOCKET_STATS_CACHE_TTL_MS_MAX), SOCKET_STATS_CACHE_TTL_MS_MAX);
    EXPECT_EQ(clampSocketStatsCacheTtlMs(SOCKET_STATS_CACHE_TTL_MS_MAX + 1), SOCKET_STATS_CACHE_TTL_MS_MAX);
}

// ========== clampMaxSaneRateBps Tests ==========

TEST(SamplingConfigTest, MaxSaneRateBpsDefaultInRange)
{
    EXPECT_GE(MAX_SANE_RATE_BPS_DEFAULT, MAX_SANE_RATE_BPS_MIN);
    EXPECT_LE(MAX_SANE_RATE_BPS_DEFAULT, MAX_SANE_RATE_BPS_MAX);
}

TEST(SamplingConfigTest, ClampMaxSaneRateBpsInRange)
{
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(MAX_SANE_RATE_BPS_DEFAULT), MAX_SANE_RATE_BPS_DEFAULT);
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(MAX_SANE_RATE_BPS_MIN), MAX_SANE_RATE_BPS_MIN);
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(MAX_SANE_RATE_BPS_MAX), MAX_SANE_RATE_BPS_MAX);
}

TEST(SamplingConfigTest, ClampMaxSaneRateBpsBelowMin)
{
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(0.0), MAX_SANE_RATE_BPS_MIN);
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(-1.0), MAX_SANE_RATE_BPS_MIN);
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(MAX_SANE_RATE_BPS_MIN - 1.0), MAX_SANE_RATE_BPS_MIN);
}

TEST(SamplingConfigTest, ClampMaxSaneRateBpsAboveMax)
{
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(MAX_SANE_RATE_BPS_MAX + 1.0), MAX_SANE_RATE_BPS_MAX);
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(1.0e20), MAX_SANE_RATE_BPS_MAX);
}

TEST(SamplingConfigTest, ClampMaxSaneRateBpsNonFinite)
{
    // NaN and -inf clamp to MIN; +inf clamps to MAX
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(std::numeric_limits<double>::quiet_NaN()), MAX_SANE_RATE_BPS_MIN);
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(std::numeric_limits<double>::infinity()), MAX_SANE_RATE_BPS_MAX);
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(-std::numeric_limits<double>::infinity()), MAX_SANE_RATE_BPS_MIN);
}

TEST(SamplingConfigTest, ClampMaxSaneRateBpsBoundary)
{
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(MAX_SANE_RATE_BPS_MIN), MAX_SANE_RATE_BPS_MIN);
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(MAX_SANE_RATE_BPS_MAX), MAX_SANE_RATE_BPS_MAX);
}

// ========== clampChartSmoothFactor Tests ==========

TEST(SamplingConfigTest, ChartSmoothFactorDefaultInRange)
{
    EXPECT_GE(CHART_SMOOTH_FACTOR_DEFAULT, CHART_SMOOTH_FACTOR_MIN);
    EXPECT_LE(CHART_SMOOTH_FACTOR_DEFAULT, CHART_SMOOTH_FACTOR_MAX);
}

TEST(SamplingConfigTest, ClampChartSmoothFactorInRange)
{
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(0.5), 0.5);
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(CHART_SMOOTH_FACTOR_MIN), CHART_SMOOTH_FACTOR_MIN);
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(CHART_SMOOTH_FACTOR_MAX), CHART_SMOOTH_FACTOR_MAX);
}

TEST(SamplingConfigTest, ClampChartSmoothFactorBelowMin)
{
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(-0.1), CHART_SMOOTH_FACTOR_MIN);
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(-1.0), CHART_SMOOTH_FACTOR_MIN);
}

TEST(SamplingConfigTest, ClampChartSmoothFactorAboveMax)
{
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(CHART_SMOOTH_FACTOR_MAX + 0.01), CHART_SMOOTH_FACTOR_MAX);
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(1.0), CHART_SMOOTH_FACTOR_MAX);
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(2.0), CHART_SMOOTH_FACTOR_MAX);
}

TEST(SamplingConfigTest, ClampChartSmoothFactorNonFinite)
{
    // NaN and -inf clamp to MIN; +inf clamps to MAX
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(std::numeric_limits<double>::quiet_NaN()), CHART_SMOOTH_FACTOR_MIN);
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(std::numeric_limits<double>::infinity()), CHART_SMOOTH_FACTOR_MAX);
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(-std::numeric_limits<double>::infinity()), CHART_SMOOTH_FACTOR_MIN);
}

TEST(SamplingConfigTest, ClampChartSmoothFactorBoundary)
{
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(0.0), 0.0);
    EXPECT_DOUBLE_EQ(clampChartSmoothFactor(CHART_SMOOTH_FACTOR_MAX), CHART_SMOOTH_FACTOR_MAX);
}

// ========== clampChartTauMsMin Tests ==========

TEST(SamplingConfigTest, ChartTauMsMinDefaultInRange)
{
    EXPECT_GE(CHART_TAU_MS_MIN_DEFAULT, CHART_TAU_MS_MIN_BOUND);
    EXPECT_LE(CHART_TAU_MS_MIN_DEFAULT, CHART_TAU_MS_MIN_MAX);
}

TEST(SamplingConfigTest, ClampChartTauMsMinInRange)
{
    EXPECT_EQ(clampChartTauMsMin(CHART_TAU_MS_MIN_DEFAULT), CHART_TAU_MS_MIN_DEFAULT);
    EXPECT_EQ(clampChartTauMsMin(CHART_TAU_MS_MIN_BOUND), CHART_TAU_MS_MIN_BOUND);
    EXPECT_EQ(clampChartTauMsMin(CHART_TAU_MS_MIN_MAX), CHART_TAU_MS_MIN_MAX);
}

TEST(SamplingConfigTest, ClampChartTauMsMinBelowMin)
{
    EXPECT_EQ(clampChartTauMsMin(0), CHART_TAU_MS_MIN_BOUND);
    EXPECT_EQ(clampChartTauMsMin(-1), CHART_TAU_MS_MIN_BOUND);
    EXPECT_EQ(clampChartTauMsMin(CHART_TAU_MS_MIN_BOUND - 1), CHART_TAU_MS_MIN_BOUND);
}

TEST(SamplingConfigTest, ClampChartTauMsMinAboveMax)
{
    EXPECT_EQ(clampChartTauMsMin(CHART_TAU_MS_MIN_MAX + 1), CHART_TAU_MS_MIN_MAX);
    EXPECT_EQ(clampChartTauMsMin(10000), CHART_TAU_MS_MIN_MAX);
}

// ========== clampChartTauMsMax Tests ==========

TEST(SamplingConfigTest, ChartTauMsMaxDefaultInRange)
{
    EXPECT_GE(CHART_TAU_MS_MAX_DEFAULT, CHART_TAU_MS_MAX_BOUND);
    EXPECT_LE(CHART_TAU_MS_MAX_DEFAULT, CHART_TAU_MS_MAX_MAX);
}

TEST(SamplingConfigTest, ClampChartTauMsMaxInRange)
{
    EXPECT_EQ(clampChartTauMsMax(CHART_TAU_MS_MAX_DEFAULT), CHART_TAU_MS_MAX_DEFAULT);
    EXPECT_EQ(clampChartTauMsMax(CHART_TAU_MS_MAX_BOUND), CHART_TAU_MS_MAX_BOUND);
    EXPECT_EQ(clampChartTauMsMax(CHART_TAU_MS_MAX_MAX), CHART_TAU_MS_MAX_MAX);
}

TEST(SamplingConfigTest, ClampChartTauMsMaxBelowMin)
{
    EXPECT_EQ(clampChartTauMsMax(0), CHART_TAU_MS_MAX_BOUND);
    EXPECT_EQ(clampChartTauMsMax(-1), CHART_TAU_MS_MAX_BOUND);
    EXPECT_EQ(clampChartTauMsMax(CHART_TAU_MS_MAX_BOUND - 1), CHART_TAU_MS_MAX_BOUND);
}

TEST(SamplingConfigTest, ClampChartTauMsMaxAboveMax)
{
    EXPECT_EQ(clampChartTauMsMax(CHART_TAU_MS_MAX_MAX + 1), CHART_TAU_MS_MAX_MAX);
    EXPECT_EQ(clampChartTauMsMax(100000), CHART_TAU_MS_MAX_MAX);
}

// ========== Default Constants Validation ==========

TEST(SamplingConfigTest, AllDefaultsAreWithinBounds)
{
    // Verify all defaults satisfy their respective min/max constraints
    EXPECT_GE(SOCKET_STATS_CACHE_TTL_MS_DEFAULT, SOCKET_STATS_CACHE_TTL_MS_MIN);
    EXPECT_LE(SOCKET_STATS_CACHE_TTL_MS_DEFAULT, SOCKET_STATS_CACHE_TTL_MS_MAX);

    EXPECT_GE(MAX_SANE_RATE_BPS_DEFAULT, MAX_SANE_RATE_BPS_MIN);
    EXPECT_LE(MAX_SANE_RATE_BPS_DEFAULT, MAX_SANE_RATE_BPS_MAX);

    EXPECT_GE(CHART_SMOOTH_FACTOR_DEFAULT, CHART_SMOOTH_FACTOR_MIN);
    EXPECT_LE(CHART_SMOOTH_FACTOR_DEFAULT, CHART_SMOOTH_FACTOR_MAX);

    EXPECT_GE(CHART_TAU_MS_MIN_DEFAULT, CHART_TAU_MS_MIN_BOUND);
    EXPECT_LE(CHART_TAU_MS_MIN_DEFAULT, CHART_TAU_MS_MIN_MAX);

    EXPECT_GE(CHART_TAU_MS_MAX_DEFAULT, CHART_TAU_MS_MAX_BOUND);
    EXPECT_LE(CHART_TAU_MS_MAX_DEFAULT, CHART_TAU_MS_MAX_MAX);
}

TEST(SamplingConfigTest, TauMsMinLessThanOrEqualToTauMsMax)
{
    // Tau min should be less than tau max for valid smoothing range
    EXPECT_LT(CHART_TAU_MS_MIN_DEFAULT, CHART_TAU_MS_MAX_DEFAULT);
    EXPECT_LE(CHART_TAU_MS_MIN_BOUND, CHART_TAU_MS_MAX_BOUND);
    EXPECT_LE(CHART_TAU_MS_MIN_MAX, CHART_TAU_MS_MAX_MAX);
}

TEST(SamplingConfigTest, MaxSaneRateBpsRepresents100Gbps)
{
    // Default 100 Gbps in bytes/sec = 100 * 10^9 / 8 bytes/sec = 12.5e9
    EXPECT_DOUBLE_EQ(MAX_SANE_RATE_BPS_DEFAULT, 12'500'000'000.0);
}

TEST(SamplingConfigTest, HistoryCapacityHoldsTheWindowAtTheFastestCadencePlusTwo)
{
    // At the fastest supported refresh the window holds ceil(seconds * samplesPerSecond) samples;
    // the ring needs one more for headroom and one for the anchor trimming keeps just before the
    // window (HistoryUtils::discardBefore, #1016). With only +1, the anchor would be overwritten
    // before a trim could keep it.
    const double samplesPerSecond = 1000.0 / static_cast<double>(REFRESH_INTERVAL_MIN_MS);
    for (const double seconds : {static_cast<double>(HISTORY_SECONDS_MIN), 300.0, static_cast<double>(HISTORY_SECONDS_MAX)})
    {
        const auto windowSamples = static_cast<std::size_t>(std::ceil(seconds * samplesPerSecond));
        EXPECT_EQ(historyCapacityForSeconds(seconds), windowSamples + 2) << seconds << " s";
    }
}

} // namespace
} // namespace Domain::Sampling

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
