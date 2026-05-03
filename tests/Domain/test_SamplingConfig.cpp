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

// ========== clampPdhInstanceRefreshSeconds Tests (Windows GPU PDH) ==========

TEST(SamplingConfigTest, PdhInstanceRefreshSecondsDefaultInRange)
{
    EXPECT_GE(PDH_INSTANCE_REFRESH_SECONDS_DEFAULT, PDH_INSTANCE_REFRESH_SECONDS_MIN);
    EXPECT_LE(PDH_INSTANCE_REFRESH_SECONDS_DEFAULT, PDH_INSTANCE_REFRESH_SECONDS_MAX);
}

TEST(SamplingConfigTest, ClampPdhInstanceRefreshSecondsInRange)
{
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(5), 5);
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(PDH_INSTANCE_REFRESH_SECONDS_MIN), PDH_INSTANCE_REFRESH_SECONDS_MIN);
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(PDH_INSTANCE_REFRESH_SECONDS_MAX), PDH_INSTANCE_REFRESH_SECONDS_MAX);
}

TEST(SamplingConfigTest, ClampPdhInstanceRefreshSecondsBelowMin)
{
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(0), PDH_INSTANCE_REFRESH_SECONDS_MIN);
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(-10), PDH_INSTANCE_REFRESH_SECONDS_MIN);
}

TEST(SamplingConfigTest, ClampPdhInstanceRefreshSecondsAboveMax)
{
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(PDH_INSTANCE_REFRESH_SECONDS_MAX + 1), PDH_INSTANCE_REFRESH_SECONDS_MAX);
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(1000), PDH_INSTANCE_REFRESH_SECONDS_MAX);
}

TEST(SamplingConfigTest, ClampPdhInstanceRefreshSecondsBoundary)
{
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(PDH_INSTANCE_REFRESH_SECONDS_MIN - 1), PDH_INSTANCE_REFRESH_SECONDS_MIN);
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(PDH_INSTANCE_REFRESH_SECONDS_MIN), PDH_INSTANCE_REFRESH_SECONDS_MIN);
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(PDH_INSTANCE_REFRESH_SECONDS_MIN + 1), PDH_INSTANCE_REFRESH_SECONDS_MIN + 1);
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(PDH_INSTANCE_REFRESH_SECONDS_MAX - 1), PDH_INSTANCE_REFRESH_SECONDS_MAX - 1);
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(PDH_INSTANCE_REFRESH_SECONDS_MAX), PDH_INSTANCE_REFRESH_SECONDS_MAX);
    EXPECT_EQ(clampPdhInstanceRefreshSeconds(PDH_INSTANCE_REFRESH_SECONDS_MAX + 1), PDH_INSTANCE_REFRESH_SECONDS_MAX);
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

// ========== clampMinTimeForRateSeconds Tests ==========

TEST(SamplingConfigTest, MinTimeForRateSecondsDefaultInRange)
{
    EXPECT_GE(MIN_TIME_FOR_RATE_SECONDS_DEFAULT, MIN_TIME_FOR_RATE_SECONDS_MIN);
    EXPECT_LE(MIN_TIME_FOR_RATE_SECONDS_DEFAULT, MIN_TIME_FOR_RATE_SECONDS_MAX);
}

TEST(SamplingConfigTest, ClampMinTimeForRateSecondsInRange)
{
    EXPECT_DOUBLE_EQ(clampMinTimeForRateSeconds(0.5), 0.5);
    EXPECT_DOUBLE_EQ(clampMinTimeForRateSeconds(MIN_TIME_FOR_RATE_SECONDS_MIN), MIN_TIME_FOR_RATE_SECONDS_MIN);
    EXPECT_DOUBLE_EQ(clampMinTimeForRateSeconds(MIN_TIME_FOR_RATE_SECONDS_MAX), MIN_TIME_FOR_RATE_SECONDS_MAX);
}

TEST(SamplingConfigTest, ClampMinTimeForRateSecondsZeroAllowed)
{
    // 0.0 is valid (no minimum time required)
    EXPECT_DOUBLE_EQ(clampMinTimeForRateSeconds(0.0), 0.0);
}

TEST(SamplingConfigTest, ClampMinTimeForRateSecondsBelowMin)
{
    EXPECT_DOUBLE_EQ(clampMinTimeForRateSeconds(-1.0), MIN_TIME_FOR_RATE_SECONDS_MIN);
    EXPECT_DOUBLE_EQ(clampMinTimeForRateSeconds(-0.001), MIN_TIME_FOR_RATE_SECONDS_MIN);
}

TEST(SamplingConfigTest, ClampMinTimeForRateSecondsAboveMax)
{
    EXPECT_DOUBLE_EQ(clampMinTimeForRateSeconds(MIN_TIME_FOR_RATE_SECONDS_MAX + 1.0), MIN_TIME_FOR_RATE_SECONDS_MAX);
    EXPECT_DOUBLE_EQ(clampMinTimeForRateSeconds(100.0), MIN_TIME_FOR_RATE_SECONDS_MAX);
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

TEST(SamplingConfigTest, ClampMaxSaneRateBpsBoundary)
{
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(MAX_SANE_RATE_BPS_MIN), MAX_SANE_RATE_BPS_MIN);
    EXPECT_DOUBLE_EQ(clampMaxSaneRateBps(MAX_SANE_RATE_BPS_MAX), MAX_SANE_RATE_BPS_MAX);
}

// ========== clampIntegratedGpuVramThresholdBytes Tests ==========

TEST(SamplingConfigTest, IntegratedGpuVramThresholdDefaultInRange)
{
    EXPECT_GE(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_DEFAULT, INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN);
    EXPECT_LE(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_DEFAULT, INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MAX);
}

TEST(SamplingConfigTest, ClampIntegratedGpuVramThresholdBytesInRange)
{
    EXPECT_EQ(clampIntegratedGpuVramThresholdBytes(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_DEFAULT),
              INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_DEFAULT);
    EXPECT_EQ(clampIntegratedGpuVramThresholdBytes(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN),
              INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN);
    EXPECT_EQ(clampIntegratedGpuVramThresholdBytes(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MAX),
              INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MAX);
}

TEST(SamplingConfigTest, ClampIntegratedGpuVramThresholdBytesBelowMin)
{
    EXPECT_EQ(clampIntegratedGpuVramThresholdBytes(int64_t{0}), INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN);
    EXPECT_EQ(clampIntegratedGpuVramThresholdBytes(int64_t{-1}), INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN);
    EXPECT_EQ(clampIntegratedGpuVramThresholdBytes(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN - 1),
              INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN);
}

TEST(SamplingConfigTest, ClampIntegratedGpuVramThresholdBytesAboveMax)
{
    EXPECT_EQ(clampIntegratedGpuVramThresholdBytes(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MAX + 1),
              INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MAX);
    EXPECT_EQ(clampIntegratedGpuVramThresholdBytes(int64_t{1024} * 1024 * 1024 * 16), INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MAX);
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

// ========== clampProgressColorLowThreshold Tests ==========

TEST(SamplingConfigTest, ProgressColorLowThresholdDefaultInRange)
{
    EXPECT_GE(PROGRESS_COLOR_LOW_THRESHOLD_DEFAULT, PROGRESS_COLOR_LOW_THRESHOLD_MIN);
    EXPECT_LE(PROGRESS_COLOR_LOW_THRESHOLD_DEFAULT, PROGRESS_COLOR_LOW_THRESHOLD_MAX);
}

TEST(SamplingConfigTest, ClampProgressColorLowThresholdInRange)
{
    EXPECT_DOUBLE_EQ(clampProgressColorLowThreshold(50.0), 50.0);
    EXPECT_DOUBLE_EQ(clampProgressColorLowThreshold(PROGRESS_COLOR_LOW_THRESHOLD_MIN), PROGRESS_COLOR_LOW_THRESHOLD_MIN);
    EXPECT_DOUBLE_EQ(clampProgressColorLowThreshold(PROGRESS_COLOR_LOW_THRESHOLD_MAX), PROGRESS_COLOR_LOW_THRESHOLD_MAX);
}

TEST(SamplingConfigTest, ClampProgressColorLowThresholdBelowMin)
{
    EXPECT_DOUBLE_EQ(clampProgressColorLowThreshold(-1.0), PROGRESS_COLOR_LOW_THRESHOLD_MIN);
    EXPECT_DOUBLE_EQ(clampProgressColorLowThreshold(-100.0), PROGRESS_COLOR_LOW_THRESHOLD_MIN);
}

TEST(SamplingConfigTest, ClampProgressColorLowThresholdAboveMax)
{
    EXPECT_DOUBLE_EQ(clampProgressColorLowThreshold(101.0), PROGRESS_COLOR_LOW_THRESHOLD_MAX);
    EXPECT_DOUBLE_EQ(clampProgressColorLowThreshold(1000.0), PROGRESS_COLOR_LOW_THRESHOLD_MAX);
}

TEST(SamplingConfigTest, ClampProgressColorLowThresholdBoundary)
{
    EXPECT_DOUBLE_EQ(clampProgressColorLowThreshold(0.0), 0.0);
    EXPECT_DOUBLE_EQ(clampProgressColorLowThreshold(100.0), 100.0);
}

// ========== clampProgressColorHighThreshold Tests ==========

TEST(SamplingConfigTest, ProgressColorHighThresholdDefaultInRange)
{
    EXPECT_GE(PROGRESS_COLOR_HIGH_THRESHOLD_DEFAULT, PROGRESS_COLOR_HIGH_THRESHOLD_MIN);
    EXPECT_LE(PROGRESS_COLOR_HIGH_THRESHOLD_DEFAULT, PROGRESS_COLOR_HIGH_THRESHOLD_MAX);
}

TEST(SamplingConfigTest, ClampProgressColorHighThresholdInRange)
{
    EXPECT_DOUBLE_EQ(clampProgressColorHighThreshold(80.0), 80.0);
    EXPECT_DOUBLE_EQ(clampProgressColorHighThreshold(PROGRESS_COLOR_HIGH_THRESHOLD_MIN), PROGRESS_COLOR_HIGH_THRESHOLD_MIN);
    EXPECT_DOUBLE_EQ(clampProgressColorHighThreshold(PROGRESS_COLOR_HIGH_THRESHOLD_MAX), PROGRESS_COLOR_HIGH_THRESHOLD_MAX);
}

TEST(SamplingConfigTest, ClampProgressColorHighThresholdBelowMin)
{
    EXPECT_DOUBLE_EQ(clampProgressColorHighThreshold(-1.0), PROGRESS_COLOR_HIGH_THRESHOLD_MIN);
    EXPECT_DOUBLE_EQ(clampProgressColorHighThreshold(-100.0), PROGRESS_COLOR_HIGH_THRESHOLD_MIN);
}

TEST(SamplingConfigTest, ClampProgressColorHighThresholdAboveMax)
{
    EXPECT_DOUBLE_EQ(clampProgressColorHighThreshold(101.0), PROGRESS_COLOR_HIGH_THRESHOLD_MAX);
    EXPECT_DOUBLE_EQ(clampProgressColorHighThreshold(1000.0), PROGRESS_COLOR_HIGH_THRESHOLD_MAX);
}

TEST(SamplingConfigTest, ClampProgressColorHighThresholdBoundary)
{
    EXPECT_DOUBLE_EQ(clampProgressColorHighThreshold(0.0), 0.0);
    EXPECT_DOUBLE_EQ(clampProgressColorHighThreshold(100.0), 100.0);
}

// ========== Default Constants Validation ==========

TEST(SamplingConfigTest, AllDefaultsAreWithinBounds)
{
    // Verify all defaults satisfy their respective min/max constraints
    EXPECT_GE(PDH_INSTANCE_REFRESH_SECONDS_DEFAULT, PDH_INSTANCE_REFRESH_SECONDS_MIN);
    EXPECT_LE(PDH_INSTANCE_REFRESH_SECONDS_DEFAULT, PDH_INSTANCE_REFRESH_SECONDS_MAX);

    EXPECT_GE(SOCKET_STATS_CACHE_TTL_MS_DEFAULT, SOCKET_STATS_CACHE_TTL_MS_MIN);
    EXPECT_LE(SOCKET_STATS_CACHE_TTL_MS_DEFAULT, SOCKET_STATS_CACHE_TTL_MS_MAX);

    EXPECT_GE(MIN_TIME_FOR_RATE_SECONDS_DEFAULT, MIN_TIME_FOR_RATE_SECONDS_MIN);
    EXPECT_LE(MIN_TIME_FOR_RATE_SECONDS_DEFAULT, MIN_TIME_FOR_RATE_SECONDS_MAX);

    EXPECT_GE(MAX_SANE_RATE_BPS_DEFAULT, MAX_SANE_RATE_BPS_MIN);
    EXPECT_LE(MAX_SANE_RATE_BPS_DEFAULT, MAX_SANE_RATE_BPS_MAX);

    EXPECT_GE(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_DEFAULT, INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN);
    EXPECT_LE(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_DEFAULT, INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MAX);

    EXPECT_GE(CHART_SMOOTH_FACTOR_DEFAULT, CHART_SMOOTH_FACTOR_MIN);
    EXPECT_LE(CHART_SMOOTH_FACTOR_DEFAULT, CHART_SMOOTH_FACTOR_MAX);

    EXPECT_GE(CHART_TAU_MS_MIN_DEFAULT, CHART_TAU_MS_MIN_BOUND);
    EXPECT_LE(CHART_TAU_MS_MIN_DEFAULT, CHART_TAU_MS_MIN_MAX);

    EXPECT_GE(CHART_TAU_MS_MAX_DEFAULT, CHART_TAU_MS_MAX_BOUND);
    EXPECT_LE(CHART_TAU_MS_MAX_DEFAULT, CHART_TAU_MS_MAX_MAX);

    EXPECT_GE(PROGRESS_COLOR_LOW_THRESHOLD_DEFAULT, PROGRESS_COLOR_LOW_THRESHOLD_MIN);
    EXPECT_LE(PROGRESS_COLOR_LOW_THRESHOLD_DEFAULT, PROGRESS_COLOR_LOW_THRESHOLD_MAX);

    EXPECT_GE(PROGRESS_COLOR_HIGH_THRESHOLD_DEFAULT, PROGRESS_COLOR_HIGH_THRESHOLD_MIN);
    EXPECT_LE(PROGRESS_COLOR_HIGH_THRESHOLD_DEFAULT, PROGRESS_COLOR_HIGH_THRESHOLD_MAX);
}

TEST(SamplingConfigTest, ProgressColorThresholdsOrdering)
{
    // The static_assert in the header enforces low <= high for defaults
    EXPECT_LE(PROGRESS_COLOR_LOW_THRESHOLD_DEFAULT, PROGRESS_COLOR_HIGH_THRESHOLD_DEFAULT);
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

} // namespace
} // namespace Domain::Sampling

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
