#pragma once

#include "Domain/SamplingConfig.h"

#include <algorithm>
#include <chrono>

namespace App::AdaptiveIntervalUtils
{

namespace Detail
{

/// Shared adaptive refresh-interval policy behind chooseAdaptiveProcessInterval and
/// chooseAdaptiveSystemInterval below: scales `baseInterval` up (slower) while the user is
/// actively resizing/dragging the window (3x) or while the owning tab isn't visible (2x),
/// clamped to Domain::Sampling::REFRESH_INTERVAL_MAX_MS. `clampBaseToMin` selects whether
/// `baseInterval` itself is floored to REFRESH_INTERVAL_MIN_MS first -- the two public
/// wrappers pass their own historical behavior through this parameter (ProcessesPanel always
/// clamped, SystemMetricsPanel never has) rather than this extraction silently unifying them;
/// neither behavior has been judged correct here, so both are preserved exactly.
[[nodiscard]] inline std::chrono::milliseconds
chooseAdaptiveInterval(std::chrono::milliseconds baseInterval, bool isActiveTab, bool interactionRedrawActive, bool clampBaseToMin)
{
    const auto baseMs = clampBaseToMin ? std::max(baseInterval.count(), static_cast<long long>(Domain::Sampling::REFRESH_INTERVAL_MIN_MS))
                                       : baseInterval.count();

    // During active resize/move interactions, dramatically reduce update frequency to preserve UI responsiveness.
    // Model refreshes include expensive probe work; batching updates prevents stalls during interaction.
    if (interactionRedrawActive)
    {
        // 3x multiplier during interaction: defer model updates while user is actively resizing/dragging
        const auto throttledMs = std::min(baseMs * 3LL, static_cast<long long>(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
        return std::chrono::milliseconds(throttledMs);
    }

    if (!isActiveTab)
    {
        // 2x multiplier for inactive tabs (already optimized but far less critical than interaction)
        const auto relaxedMs = std::min(baseMs * 2LL, static_cast<long long>(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
        return std::chrono::milliseconds(relaxedMs);
    }

    // Active tab, no interaction: use base interval
    return std::chrono::milliseconds(baseMs);
}

} // namespace Detail

/// Adaptive refresh-interval policy used by ProcessesPanel, extracted so it can be
/// unit-tested without linking the panel's ImGui rendering code. Enforces
/// Domain::Sampling::REFRESH_INTERVAL_MIN_MS as a floor on the base interval before scaling.
[[nodiscard]] inline std::chrono::milliseconds
chooseAdaptiveProcessInterval(std::chrono::milliseconds baseInterval, bool isActiveTab, bool interactionRedrawActive)
{
    return Detail::chooseAdaptiveInterval(baseInterval, isActiveTab, interactionRedrawActive, /*clampBaseToMin=*/true);
}

/// Adaptive refresh-interval policy used by SystemMetricsPanel, matching
/// chooseAdaptiveProcessInterval's cadence strategy (3x during interaction, 2x while
/// inactive).
///
/// Note: unlike chooseAdaptiveProcessInterval, this does not clamp `baseInterval` itself to
/// Domain::Sampling::REFRESH_INTERVAL_MIN_MS before scaling - the active-tab/no-interaction
/// path returns `baseInterval` unmodified even if it is below the configured minimum. That
/// discrepancy is preserved here exactly as it already behaved; it has not been judged
/// correct or incorrect as part of this extraction.
[[nodiscard]] inline std::chrono::milliseconds
chooseAdaptiveSystemInterval(std::chrono::milliseconds baseInterval, bool isActiveTab, bool interactionRedrawActive)
{
    return Detail::chooseAdaptiveInterval(baseInterval, isActiveTab, interactionRedrawActive, /*clampBaseToMin=*/false);
}

} // namespace App::AdaptiveIntervalUtils
