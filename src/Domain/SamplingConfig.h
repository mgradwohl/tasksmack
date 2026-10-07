#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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

// Earliest rebuild of the inode-to-PID map (milliseconds) - Linux only
// A socket that shows up with no owner after the map was built may have been opened since; the
// probe then rebuilds the map ahead of its TTL so the new connection is attributed in the reading
// it first appears in (#1259), but no more often than this, so a burst of short-lived connections
// (or other users' connections, which never resolve) can't turn the /proc scan into a per-refresh one.
inline constexpr int INODE_PID_CACHE_EARLY_REBUILD_MS = 1000;

// How long a connection seen with no owner holds its growth for the owner it gets (milliseconds)
// SocketTrafficAccumulator credits the bytes an unattributed connection moves to the process it is
// attributed to later (#1259). An attributable connection gets its owner at the next map rebuild:
// within one map TTL plus one refresh of its first sighting. One still unattributed after that
// belongs to a process we can't read; it stops holding, so if it is ever attributed it can't land
// hours of traffic in one interval.
inline constexpr int UNATTRIBUTED_SOCKET_HOLD_MS = INODE_PID_CACHE_TTL_MS + REFRESH_INTERVAL_MAX_MS;

// Longest a process's cached command line is reused (milliseconds) - Linux only
// A command line is set at exec, so LinuxProcessProbe reads /proc/[pid]/cmdline once per process
// (pid + start time, and the comm exec sets) instead of every sample (#1425). A process may still
// rewrite its argv (a process title: postgres, sshd, browsers), so each entry is re-read after at
// most this long -- the same staleness bound as network attribution's inode map. Entries expire
// spread over the second half of it, so the processes cached on one sample aren't all re-read on
// the same later one.
inline constexpr int PROCESS_CMDLINE_CACHE_TTL_MS = 3000;

// -----------------------------------------------------------------------------
// Instance Enumeration Caches (User-Configurable via TOML)
// -----------------------------------------------------------------------------
// These control how often we discover NEW entities. They are separate from the
// main refresh interval because discovery operations are often more expensive
// than updating known entities.

// GPU re-enumeration interval (seconds) - fixed, not configurable
// How often GPUModel asks its probe for a full rescan of the GPU set (IGPUProbe::rescanGPUs with
// GPURescan::Full): a hot-plugged eGPU, a GPU removed or lost after a driver reset (#1116). The
// Linux probes check sysfs, which never wakes a sleeping GPU, and only re-initialise a vendor
// library when something changed, so this costs a directory scan per interval.
inline constexpr int GPU_RESCAN_INTERVAL_SECONDS = 10;

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

// Network rate ceiling (bytes per second)
// A per-process network rate above this is treated as a bad reading (e.g. on Windows, a connection
// first attributed with traffic from before it was seen) and shown as 0 (ProcessModel); a
// per-interface rate above it is a counter glitch, shown as 0 and charted as a gap (SystemModel,
// #1291). Default is 100 Gbps (12.5 billion bytes/sec).
// Configurable via [metrics] max_sane_rate_bps in config.toml (read at startup, #1123).
inline constexpr double MAX_SANE_RATE_BPS_DEFAULT = 12'500'000'000.0; // 100 Gbps in bytes/sec
inline constexpr double MAX_SANE_RATE_BPS_MIN = 1'000'000'000.0;      // 8 Gbps (minimum reasonable)
inline constexpr double MAX_SANE_RATE_BPS_MAX = 100'000'000'000.0;    // 800 Gbps (upper bound)

// Per-disk I/O rate ceiling (bytes per second) - fixed, not configurable
// A disk read or write rate above this is a counter glitch (a reinitialised or re-registered
// device counter), not I/O: the sample has no rates and its history records a gap (StorageModel,
// #1291). Not max_sane_rate_bps: its 100 Gbps (12.5 GB/s) default is below what a single PCIe 5.0
// NVMe drive reads, so a network-sized ceiling would drop real samples. 1 TB/s is far beyond any
// block device while still catching a counter that jumps by a disk's lifetime bytes.
inline constexpr double MAX_SANE_DISK_RATE_BPS = 1'000'000'000'000.0;

// -----------------------------------------------------------------------------
// UI Behavior Parameters (User-Configurable via TOML)
// -----------------------------------------------------------------------------
// These control how the UI renders data and responds to user interaction.

// Easing of live values and the "now" bars beside charts toward each new sample
// (UI::Widgets::computeAlpha): alpha = 1 - exp(-dt / tau), with
// tau = clamp(refresh interval * CHART_SMOOTH_FACTOR, CHART_TAU_MS_MIN, CHART_TAU_MS_MAX).
// Higher = smoother but slower to follow changes; 0 = tau is CHART_TAU_MS_MIN (barely eased).
// Configurable via [ui] chart_smooth_factor in config.toml (read at startup, #1123).
inline constexpr double CHART_SMOOTH_FACTOR_DEFAULT = 0.5;
inline constexpr double CHART_SMOOTH_FACTOR_MIN = 0.0;
inline constexpr double CHART_SMOOTH_FACTOR_MAX = 0.95;

// Range the easing time constant above is kept within (milliseconds), so a very fast or very slow
// refresh interval neither disables easing nor makes it sluggish.
// TAU_MIN = shortest easing time; TAU_MAX = longest.
// Configurable via [ui] chart_tau_ms_min and chart_tau_ms_max in config.toml (read at startup, #1123).
inline constexpr int CHART_TAU_MS_MIN_DEFAULT = 20;
inline constexpr int CHART_TAU_MS_MIN_BOUND = 5;
inline constexpr int CHART_TAU_MS_MIN_MAX = 100;

inline constexpr int CHART_TAU_MS_MAX_DEFAULT = 400;
inline constexpr int CHART_TAU_MS_MAX_BOUND = 100;
inline constexpr int CHART_TAU_MS_MAX_MAX = 2000;

// Clamp helpers
// -------------
// These functions provide common guardrails for numeric sampling settings that are directly
// represented as min/max constants in this header (e.g., refresh interval, history window).

template<typename T> [[nodiscard]] constexpr T clampRefreshInterval(T value) noexcept
{
    return std::clamp(value, static_cast<T>(REFRESH_INTERVAL_MIN_MS), static_cast<T>(REFRESH_INTERVAL_MAX_MS));
}

/// NaN maps to HISTORY_SECONDS_MIN, as in historyCapacityForSeconds(), so a model's window and its ring
/// capacity agree; std::clamp would pass NaN through (#1325).
template<typename T> [[nodiscard]] constexpr T clampHistorySeconds(T value) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        // +inf → MAX; NaN and -inf → MIN.
        if (!std::isfinite(value))
        {
            return (std::isinf(value) && (value > T{0})) ? static_cast<T>(HISTORY_SECONDS_MAX) : static_cast<T>(HISTORY_SECONDS_MIN);
        }
    }
    return std::clamp(value, static_cast<T>(HISTORY_SECONDS_MIN), static_cast<T>(HISTORY_SECONDS_MAX));
}

/// Number of ring-buffer slots needed to retain `historySeconds` of data at the
/// fastest supported refresh cadence (REFRESH_INTERVAL_MIN_MS), plus two slots: one of
/// headroom so time-based trimming (not capacity) governs the retention window, and one
/// for the sample trimming keeps just before the window (HistoryUtils::discardBefore).
///
/// NaN and non-finite inputs are normalised to the supported range before
/// conversion so the cast to std::size_t is always well-defined.
[[nodiscard]] inline std::size_t historyCapacityForSeconds(double historySeconds) noexcept
{
    // Guard against NaN / infinity before any arithmetic.
    if (!std::isfinite(historySeconds))
    {
        historySeconds = (std::isinf(historySeconds) && historySeconds > 0.0) ? static_cast<double>(HISTORY_SECONDS_MAX)
                                                                              : static_cast<double>(HISTORY_SECONDS_MIN);
    }
    const double seconds = std::clamp(historySeconds, static_cast<double>(HISTORY_SECONDS_MIN), static_cast<double>(HISTORY_SECONDS_MAX));
    const double samplesPerSecond = 1000.0 / static_cast<double>(REFRESH_INTERVAL_MIN_MS);
    const auto samples = static_cast<std::size_t>(std::ceil(seconds * samplesPerSecond));
    return samples + 2;
}

template<typename T> [[nodiscard]] constexpr T clampSocketStatsCacheTtlMs(T value) noexcept
{
    return std::clamp(value, static_cast<T>(SOCKET_STATS_CACHE_TTL_MS_MIN), static_cast<T>(SOCKET_STATS_CACHE_TTL_MS_MAX));
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

} // namespace Domain::Sampling
