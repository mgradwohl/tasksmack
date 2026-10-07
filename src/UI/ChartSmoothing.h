#pragma once

// How live values and "now" bars ease toward each new sample: the time constant, the per-update
// alpha, and initializeOrSmooth(), the one smoothing idiom every NowBar uses. Moved out of
// ChartWidgets.h, which re-exports it, so code that smooths values without drawing them (Process
// Details' now-bar state, #1179) stays free of ImGui and is unit-testable directly.

#include "Domain/Numeric.h"
#include "Domain/SamplingConfig.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <type_traits>

namespace UI::Widgets
{

/// How live values and "now" bars ease toward each new sample (computeAlpha): the time constant is
/// `smoothFactor` times the refresh interval, kept within [tauMsMin, tauMsMax].
struct ChartSmoothing
{
    double smoothFactor = Domain::Sampling::CHART_SMOOTH_FACTOR_DEFAULT;
    double tauMsMin = static_cast<double>(Domain::Sampling::CHART_TAU_MS_MIN_DEFAULT);
    double tauMsMax = static_cast<double>(Domain::Sampling::CHART_TAU_MS_MAX_DEFAULT);
};

static_assert(Domain::Sampling::CHART_TAU_MS_MIN_MAX <= Domain::Sampling::CHART_TAU_MS_MAX_BOUND,
              "a clamped chart_tau_ms_min must never exceed a clamped chart_tau_ms_max (std::clamp needs lo <= hi)");

namespace Detail
{
// One instance program-wide, like g_ChartAntiAliasingEnabled in ChartWidgets.h. Read and written on
// the UI thread only.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline ChartSmoothing g_ChartSmoothing;
} // namespace Detail

/// Sets the smoothing computeAlpha() uses: the [ui] chart_smooth_factor / chart_tau_ms_min /
/// chart_tau_ms_max settings, pushed in by the App composition root at startup (UI must not read
/// UserConfig, #1123). Each value is clamped to its SamplingConfig range, so the minimum time
/// constant can never exceed the maximum.
inline void setChartSmoothing(double smoothFactor, int tauMsMin, int tauMsMax) noexcept
{
    Detail::g_ChartSmoothing = ChartSmoothing{
        .smoothFactor = Domain::Sampling::clampChartSmoothFactor(smoothFactor),
        .tauMsMin = static_cast<double>(Domain::Sampling::clampChartTauMsMin(tauMsMin)),
        .tauMsMax = static_cast<double>(Domain::Sampling::clampChartTauMsMax(tauMsMax)),
    };
}

[[nodiscard]] inline ChartSmoothing chartSmoothing() noexcept
{
    return Detail::g_ChartSmoothing;
}

inline double computeAlpha(double deltaTimeSeconds, std::chrono::milliseconds refreshInterval)
{
    const ChartSmoothing smoothing = chartSmoothing();
    const double baseIntervalMs = Domain::Numeric::toDouble(refreshInterval.count());
    const double tauMs = std::clamp(baseIntervalMs * smoothing.smoothFactor, smoothing.tauMsMin, smoothing.tauMsMax);
    const double dtMs = (deltaTimeSeconds > 0.0) ? deltaTimeSeconds * 1000.0 : baseIntervalMs;
    return std::clamp(1.0 - std::exp(-dtMs / std::max(1.0, tauMs)), 0.0, 1.0);
}

inline double computeAlpha(float deltaTimeSeconds, std::chrono::milliseconds refreshInterval)
{
    return computeAlpha(Domain::Numeric::toDouble(deltaTimeSeconds),
                        refreshInterval); // Explicit: float seconds -> double smoothing math
}

inline double smoothTowards(double current, double target, double alpha)
{
    return current + (alpha * (target - current));
}

template<typename T> inline T initializeOrSmooth(T current, T target, double alpha, bool initialized)
{
    static_assert(std::is_arithmetic_v<T>, "initializeOrSmooth requires arithmetic types");
    if (!initialized)
    {
        return target;
    }
    return static_cast<T>(smoothTowards(static_cast<double>(current), static_cast<double>(target), alpha));
}

} // namespace UI::Widgets
