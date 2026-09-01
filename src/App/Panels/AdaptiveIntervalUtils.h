#pragma once

#include "Domain/SamplingConfig.h"

#include <algorithm>
#include <chrono>

namespace App::AdaptiveIntervalUtils
{

/// Adaptive refresh-interval policy used by ProcessesPanel, extracted so it can be
/// unit-tested without linking the panel's ImGui rendering code. Scales `baseInterval` up
/// (slower) while the user is actively resizing/dragging the window or while this tab isn't
/// visible, and enforces Domain::Sampling::REFRESH_INTERVAL_MIN_MS as a floor on the base
/// interval before scaling.
[[nodiscard]] inline std::chrono::milliseconds
chooseAdaptiveProcessInterval(std::chrono::milliseconds baseInterval, bool isActiveTab, bool interactionRedrawActive)
{
    const auto baseMs = std::max(baseInterval.count(), static_cast<long long>(Domain::Sampling::REFRESH_INTERVAL_MIN_MS));

    // During active resize/move interactions, dramatically reduce update frequency to preserve UI responsiveness.
    // ProcessModel refresh includes expensive probe work; batching updates prevents stalls during interaction.
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
    const auto baseMs = baseInterval.count();
    if (interactionRedrawActive)
    {
        // 3x multiplier during active resize/move: defer expensive model refreshes.
        // Clamp to REFRESH_INTERVAL_MAX_MS so the scaled value never exceeds the guardrail.
        return std::chrono::milliseconds(std::min(baseMs * 3LL, static_cast<long long>(Domain::Sampling::REFRESH_INTERVAL_MAX_MS)));
    }
    if (!isActiveTab)
    {
        // 2x multiplier when tab is not visible
        return std::chrono::milliseconds(std::min(baseMs * 2LL, static_cast<long long>(Domain::Sampling::REFRESH_INTERVAL_MAX_MS)));
    }
    return baseInterval;
}

} // namespace App::AdaptiveIntervalUtils
