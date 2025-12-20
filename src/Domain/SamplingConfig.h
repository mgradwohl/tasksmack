#pragma once

#include <algorithm>

namespace Domain::Sampling
{

// Refresh cadence (milliseconds)
inline constexpr int REFRESH_INTERVAL_DEFAULT_MS = 1000;
inline constexpr int REFRESH_INTERVAL_MIN_MS = 100;
inline constexpr int REFRESH_INTERVAL_MAX_MS = 5000;

// History window (seconds)
inline constexpr int HISTORY_SECONDS_DEFAULT = 300; // 5 minutes
inline constexpr int HISTORY_SECONDS_MIN = 10;
inline constexpr int HISTORY_SECONDS_MAX = 1800; // 30 minutes

template<typename T> [[nodiscard]] constexpr T clampRefreshInterval(T value)
{
    return std::clamp(value, static_cast<T>(REFRESH_INTERVAL_MIN_MS), static_cast<T>(REFRESH_INTERVAL_MAX_MS));
}

template<typename T> [[nodiscard]] constexpr T clampHistorySeconds(T value)
{
    return std::clamp(value, static_cast<T>(HISTORY_SECONDS_MIN), static_cast<T>(HISTORY_SECONDS_MAX));
}

} // namespace Domain::Sampling
