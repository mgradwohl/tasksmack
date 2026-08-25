#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace Domain::Sampling
{

// =============================================================================
// SAMPLING / CACHING CONSTANTS (SUMMARY)
// =============================================================================
//
// This header centralizes sampling guardrails used across Domain and UI:
//   1) User-configurable refresh interval (ms)
//   2) Platform-level optimization cache TTLs
//   3) Instance-enumeration intervals/TTLs (e.g., GPU, socket stats)
//
// Detailed rationale and architecture notes live in the docs
// (see tasksmack.md and related sampling documentation).
// The comments below are intentionally brief to keep this header lightweight.
//
// =============================================================================
//
// IMPORTANT: Panels drive data polling via their onUpdate() methods using the
// user's refresh interval. Some panels (e.g., ProcessesPanel, SystemMetricsPanel)
// use the BackgroundSampler class to offload sampling to a background thread,
// while others poll directly on the UI thread depending on their needs.
//
// =============================================================================

// -----------------------------------------------------------------------------
// User-Configurable Refresh Interval
// -----------------------------------------------------------------------------
// Controls how often panels refresh their data. This is the primary user-facing
// setting that affects data freshness vs. CPU usage tradeoff.

inline constexpr int REFRESH_INTERVAL_DEFAULT_MS = 1000;
inline constexpr int REFRESH_INTERVAL_MIN_MS = 100;
inline constexpr int REFRESH_INTERVAL_MAX_MS = 5000;

// Common refresh rate presets (milliseconds) used for UI snapping and tick marks
inline constexpr std::array<int, 4> COMMON_REFRESH_INTERVALS_MS = {100, 250, 500, 1000};

// -----------------------------------------------------------------------------
// History Window Configuration
// -----------------------------------------------------------------------------
// Controls how much timeline data is retained and shown in plots.

inline constexpr int HISTORY_SECONDS_DEFAULT = 300; // 5 minutes
inline constexpr int HISTORY_SECONDS_MIN = 10;
inline constexpr int HISTORY_SECONDS_MAX = 1800; // 30 minutes

// -----------------------------------------------------------------------------
// Platform-Level Optimization Caches (Fixed TTL)
// -----------------------------------------------------------------------------
// These are internal optimization caches, not user-configurable.
// They prevent redundant expensive operations within/between refresh cycles.

// Cache TTL for network interface link speed (seconds)
// Link speed rarely changes (only on cable replug or driver reload), so we
// cache it for 60 seconds to avoid repeated sysfs reads.
inline constexpr int64_t LINK_SPEED_CACHE_TTL_SECONDS = 60;

// Cache TTL for inode-to-PID mapping (milliseconds) - Linux only
// buildInodeToPidMap() scans /proc/[pid]/fd/* for all processes to resolve socket
// inodes to owning PIDs. At 1Hz sampling this scan adds measurable latency on busy
// systems. A 3-second TTL cuts rebuilds to ~once every 3 calls; short-lived staleness
// is acceptable for network attribution since the mapping drifts slowly.
inline constexpr int INODE_PID_CACHE_TTL_MS = 3000;

// -----------------------------------------------------------------------------
// Instance Enumeration Caches (User-Configurable via TOML)
// -----------------------------------------------------------------------------
// These control how often we discover NEW entities. They are separate from the
// main refresh interval because discovery operations are often more expensive
// than updating known entities.

// Socket stats cache TTL (milliseconds) - Linux only
// Controls how long per-process network stats (via Netlink INET_DIAG) are cached.
// This is an optimization cache: if multiple calls happen within the TTL, the
// cached result is returned instead of querying the kernel again.
// Shorter TTL = fresher data but more kernel queries (higher CPU cost).
// A TTL of ~50% of refresh interval balances freshness vs. cost.
// Configurable via [sampling] socket_stats_cache_ttl_ms in config.toml.
inline constexpr int SOCKET_STATS_CACHE_TTL_MS_DEFAULT = 500;
inline constexpr int SOCKET_STATS_CACHE_TTL_MS_MIN = 0;    // 0 = disable caching
inline constexpr int SOCKET_STATS_CACHE_TTL_MS_MAX = 5000; // Cap at max refresh interval

// -----------------------------------------------------------------------------
// Metrics Calculation Parameters (User-Configurable via TOML)
// -----------------------------------------------------------------------------
// These control how metrics are computed from raw counter data.
// They affect data freshness, responsiveness, and sanity checking.

// Minimum time elapsed before computing network rates (seconds)
// On first sample or shortly after a process starts, we don't have enough deltas
// to compute meaningful rates. This threshold prevents huge rate spikes early on.
// Shorter = earlier rate display but more noise. Longer = fewer spikes but delayed data.
// Configurable via [metrics] min_time_for_rate_seconds in config.toml.
inline constexpr double MIN_TIME_FOR_RATE_SECONDS_DEFAULT = 0.5;
inline constexpr double MIN_TIME_FOR_RATE_SECONDS_MIN = 0.0;
inline constexpr double MIN_TIME_FOR_RATE_SECONDS_MAX = 5.0;

// Maximum sanity check for network/IO rate calculation (bytes per second)
// Rates above this are treated as errors (counter overflow, bad data, etc.)
// and are clamped to 0. Default is 100 Gbps (12.5 billion bytes/sec).
// This is a safety net to catch data corruption or counter resets.
// Configurable via [metrics] max_sane_rate_bps in config.toml.
inline constexpr double MAX_SANE_RATE_BPS_DEFAULT = 12'500'000'000.0; // 100 Gbps in bytes/sec
inline constexpr double MAX_SANE_RATE_BPS_MIN = 1'000'000'000.0;      // 8 Gbps (minimum reasonable)
inline constexpr double MAX_SANE_RATE_BPS_MAX = 100'000'000'000.0;    // 800 Gbps (upper bound)

// GPU integrated VRAM threshold (bytes) - Windows only
// Used to classify GPUs as "integrated" (dedicated VRAM < threshold) vs. "discrete".
// Integrated GPUs typically share system RAM and have <256MB dedicated VRAM.
// This affects how we report GPU memory to avoid confusion (system RAM vs. VRAM).
// Configurable via [metrics] integrated_gpu_vram_threshold_mb in config.toml.
inline constexpr int64_t INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_DEFAULT = 128ULL * 1024 * 1024; // 128MB
inline constexpr int64_t INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN = 16ULL * 1024 * 1024;      // 16MB
inline constexpr int64_t INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MAX = 512ULL * 1024 * 1024;     // 512MB

// -----------------------------------------------------------------------------
// UI Behavior Parameters (User-Configurable via TOML)
// -----------------------------------------------------------------------------
// These control how the UI renders data and responds to user interaction.

// Exponential smoothing factor for charts (0.0 = no smoothing, 1.0 = full averaging)
// Controls how much of the previous smoothed value carries to the next sample.
// Higher = smoother lines but delayed response to changes. Lower = noisier but more responsive.
// Mathematically: smoothed_value = current_sample * (1 - SMOOTH_FACTOR) + prev_smoothed * SMOOTH_FACTOR
// Configurable via [ui] chart_smooth_factor in config.toml.
inline constexpr double CHART_SMOOTH_FACTOR_DEFAULT = 0.5;
inline constexpr double CHART_SMOOTH_FACTOR_MIN = 0.0;
inline constexpr double CHART_SMOOTH_FACTOR_MAX = 0.95;

// Adaptive time constant range for chart smoothing (milliseconds)
// The smoothing tau (time constant) is adapted based on refresh interval to maintain
// consistent visual behavior across different refresh rates.
// TAU_MIN = responsiveness floor (minimum smoothing window)
// TAU_MAX = responsiveness ceiling (maximum smoothing window)
// Configurable via [ui] chart_tau_ms_min and chart_tau_ms_max in config.toml.
inline constexpr int CHART_TAU_MS_MIN_DEFAULT = 20;
inline constexpr int CHART_TAU_MS_MIN_BOUND = 5;
inline constexpr int CHART_TAU_MS_MIN_MAX = 100;

inline constexpr int CHART_TAU_MS_MAX_DEFAULT = 400;
inline constexpr int CHART_TAU_MS_MAX_BOUND = 100;
inline constexpr int CHART_TAU_MS_MAX_MAX = 2000;

// Progress bar color thresholds (percentage, 0-100)
// Controls which color is used for progress bars (CPU%, memory%, etc.)
// LOW_THRESHOLD = percentage below which progressLow color is used
// HIGH_THRESHOLD = percentage above which progressHigh color is used
// Between = progressMedium color
// Configurable via [ui] progress_color_low_threshold and progress_color_high_threshold in config.toml.
inline constexpr double PROGRESS_COLOR_LOW_THRESHOLD_DEFAULT = 50.0;
inline constexpr double PROGRESS_COLOR_LOW_THRESHOLD_MIN = 0.0;
inline constexpr double PROGRESS_COLOR_LOW_THRESHOLD_MAX = 100.0;

inline constexpr double PROGRESS_COLOR_HIGH_THRESHOLD_DEFAULT = 80.0;
inline constexpr double PROGRESS_COLOR_HIGH_THRESHOLD_MIN = 0.0;
inline constexpr double PROGRESS_COLOR_HIGH_THRESHOLD_MAX = 100.0;

// Note: If low == high threshold, the "medium" color range (yellow) disappears and utilization
// is either low (green) or high (red).
//
// The static_assert below only verifies the relationship between the *default* constants.
// At runtime, user-configurable thresholds loaded from config.toml are validated and, if needed,
// normalized (e.g., swapped when low > high) by UserConfig::load(). See that implementation
// for the full runtime validation/clamping rules.
static_assert(PROGRESS_COLOR_LOW_THRESHOLD_DEFAULT <= PROGRESS_COLOR_HIGH_THRESHOLD_DEFAULT,
              "PROGRESS_COLOR_LOW_THRESHOLD_DEFAULT must be <= PROGRESS_COLOR_HIGH_THRESHOLD_DEFAULT");

// Clamp helpers
// -------------
// These functions provide common guardrails for numeric sampling settings that are directly
// represented as min/max constants in this header (e.g., refresh interval, history window).
//
// Progress bar color thresholds are also user-configurable, but their runtime validation and
// normalization (including the low > high swap described above) are handled in UserConfig::load()
// rather than via a dedicated clamp helper here. This keeps all progress-color-specific logic
// centralized in the configuration layer while documenting the guardrails in this header.

template<typename T> [[nodiscard]] constexpr T clampRefreshInterval(T value) noexcept
{
    return std::clamp(value, static_cast<T>(REFRESH_INTERVAL_MIN_MS), static_cast<T>(REFRESH_INTERVAL_MAX_MS));
}

template<typename T> [[nodiscard]] constexpr T clampHistorySeconds(T value) noexcept
{
    return std::clamp(value, static_cast<T>(HISTORY_SECONDS_MIN), static_cast<T>(HISTORY_SECONDS_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampSocketStatsCacheTtlMs(T value) noexcept
{
    return std::clamp(value, static_cast<T>(SOCKET_STATS_CACHE_TTL_MS_MIN), static_cast<T>(SOCKET_STATS_CACHE_TTL_MS_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampMinTimeForRateSeconds(T value) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        // Guard against NaN and infinity: std::clamp has undefined behavior with NaN.
        // +inf → MAX; NaN and -inf → MIN.
        if (!std::isfinite(value))
        {
            return (std::isinf(value) && (value > T{0})) ? static_cast<T>(MIN_TIME_FOR_RATE_SECONDS_MAX)
                                                         : static_cast<T>(MIN_TIME_FOR_RATE_SECONDS_MIN);
        }
    }
    return std::clamp(value, static_cast<T>(MIN_TIME_FOR_RATE_SECONDS_MIN), static_cast<T>(MIN_TIME_FOR_RATE_SECONDS_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampMaxSaneRateBps(T value) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        // Guard against NaN and infinity: std::clamp has undefined behavior with NaN.
        // +inf → MAX; NaN and -inf → MIN.
        if (!std::isfinite(value))
        {
            return (std::isinf(value) && (value > T{0})) ? static_cast<T>(MAX_SANE_RATE_BPS_MAX) : static_cast<T>(MAX_SANE_RATE_BPS_MIN);
        }
    }
    return std::clamp(value, static_cast<T>(MAX_SANE_RATE_BPS_MIN), static_cast<T>(MAX_SANE_RATE_BPS_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampIntegratedGpuVramThresholdBytes(T value) noexcept
{
    return std::clamp(
        value, static_cast<T>(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MIN), static_cast<T>(INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampChartSmoothFactor(T value) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        // Guard against NaN and infinity: std::clamp has undefined behavior with NaN.
        // +inf → MAX; NaN and -inf → MIN.
        if (!std::isfinite(value))
        {
            return (std::isinf(value) && (value > T{0})) ? static_cast<T>(CHART_SMOOTH_FACTOR_MAX)
                                                         : static_cast<T>(CHART_SMOOTH_FACTOR_MIN);
        }
    }
    return std::clamp(value, static_cast<T>(CHART_SMOOTH_FACTOR_MIN), static_cast<T>(CHART_SMOOTH_FACTOR_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampChartTauMsMin(T value) noexcept
{
    return std::clamp(value, static_cast<T>(CHART_TAU_MS_MIN_BOUND), static_cast<T>(CHART_TAU_MS_MIN_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampChartTauMsMax(T value) noexcept
{
    return std::clamp(value, static_cast<T>(CHART_TAU_MS_MAX_BOUND), static_cast<T>(CHART_TAU_MS_MAX_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampProgressColorLowThreshold(T value) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        // Guard against NaN and infinity: std::clamp has undefined behavior with NaN.
        // +inf → MAX; NaN and -inf → MIN.
        if (!std::isfinite(value))
        {
            return (std::isinf(value) && (value > T{0})) ? static_cast<T>(PROGRESS_COLOR_LOW_THRESHOLD_MAX)
                                                         : static_cast<T>(PROGRESS_COLOR_LOW_THRESHOLD_MIN);
        }
    }
    return std::clamp(value, static_cast<T>(PROGRESS_COLOR_LOW_THRESHOLD_MIN), static_cast<T>(PROGRESS_COLOR_LOW_THRESHOLD_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampProgressColorHighThreshold(T value) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        // Guard against NaN and infinity: std::clamp has undefined behavior with NaN.
        // +inf → MAX; NaN and -inf → MIN.
        if (!std::isfinite(value))
        {
            return (std::isinf(value) && (value > T{0})) ? static_cast<T>(PROGRESS_COLOR_HIGH_THRESHOLD_MAX)
                                                         : static_cast<T>(PROGRESS_COLOR_HIGH_THRESHOLD_MIN);
        }
    }
    return std::clamp(value, static_cast<T>(PROGRESS_COLOR_HIGH_THRESHOLD_MIN), static_cast<T>(PROGRESS_COLOR_HIGH_THRESHOLD_MAX));
}

} // namespace Domain::Sampling
