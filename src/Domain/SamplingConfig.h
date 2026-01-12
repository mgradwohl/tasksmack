#pragma once

#include <algorithm>
#include <array>

namespace Domain::Sampling
{

// Refresh cadence (milliseconds)
inline constexpr int REFRESH_INTERVAL_DEFAULT_MS = 1000;
inline constexpr int REFRESH_INTERVAL_MIN_MS = 100;
inline constexpr int REFRESH_INTERVAL_MAX_MS = 5000;

// Common refresh rate presets (milliseconds) used for UI snapping and tick marks
inline constexpr std::array<int, 4> COMMON_REFRESH_INTERVALS_MS = {100, 250, 500, 1000};

// History window (seconds)
inline constexpr int HISTORY_SECONDS_DEFAULT = 300; // 5 minutes
inline constexpr int HISTORY_SECONDS_MIN = 10;
inline constexpr int HISTORY_SECONDS_MAX = 1800; // 30 minutes

// Cache TTL for network interface link speed (seconds)
// Link speed rarely changes (only on cable replug or driver reload)
inline constexpr int64_t LINK_SPEED_CACHE_TTL_SECONDS = 60;

// PDH GPU instance refresh interval (seconds)
// Controls how often we re-enumerate GPU instances to detect new GPU-using processes.
// This is separate from ProcessModel's process enumeration (which discovers all processes).
// PDH specifically discovers which processes actively use GPU via Windows Performance Counters.
// Shorter intervals = faster detection but more CPU overhead from PdhEnumObjectItems calls.
inline constexpr int PDH_INSTANCE_REFRESH_SECONDS_DEFAULT = 5;
inline constexpr int PDH_INSTANCE_REFRESH_SECONDS_MIN = 1;
inline constexpr int PDH_INSTANCE_REFRESH_SECONDS_MAX = 60;

template<typename T> [[nodiscard]] constexpr T clampRefreshInterval(T value) noexcept
{
    return std::clamp(value, static_cast<T>(REFRESH_INTERVAL_MIN_MS), static_cast<T>(REFRESH_INTERVAL_MAX_MS));
}

template<typename T> [[nodiscard]] constexpr T clampHistorySeconds(T value) noexcept
{
    return std::clamp(value, static_cast<T>(HISTORY_SECONDS_MIN), static_cast<T>(HISTORY_SECONDS_MAX));
}

template<typename T> [[nodiscard]] constexpr T clampPdhInstanceRefreshSeconds(T value) noexcept
{
    return std::clamp(value, static_cast<T>(PDH_INSTANCE_REFRESH_SECONDS_MIN),
                      static_cast<T>(PDH_INSTANCE_REFRESH_SECONDS_MAX));
}

} // namespace Domain::Sampling
