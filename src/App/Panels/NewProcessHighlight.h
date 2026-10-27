#pragma once

// The Processes table's tint for a process that started a moment ago (#3060), as Process Explorer and
// System Informer highlight new processes: short-lived processes are otherwise easy to miss. Pure, so
// tested without ImGui (test_NewProcessHighlight.cpp).

#include <algorithm>
#include <cstdint>
#include <optional>

namespace App::NewProcessHighlight
{

/// How long a new process's row stays tinted, fading out over it, in seconds.
inline constexpr double DURATION_SECONDS = 5.0;

/// The tint's opacity at the start, fading linearly to nothing.
inline constexpr float MAX_ALPHA = 0.28F;

/// The tint's opacity for a process that started at @p startEpochSeconds (Unix seconds), seen at
/// @p nowEpochSeconds; nullopt when it is older than DURATION_SECONDS or its start time isn't known (0).
/// A start a little in the future (the clock and /proc disagree by a fraction of a second) counts as now.
[[nodiscard]] inline std::optional<float> alpha(std::uint64_t startEpochSeconds, double nowEpochSeconds) noexcept
{
    if (startEpochSeconds == 0)
    {
        return std::nullopt;
    }
    const double age = std::max(0.0, nowEpochSeconds - static_cast<double>(startEpochSeconds));
    if (age >= DURATION_SECONDS)
    {
        return std::nullopt;
    }
    return MAX_ALPHA * static_cast<float>(1.0 - (age / DURATION_SECONDS));
}

} // namespace App::NewProcessHighlight
