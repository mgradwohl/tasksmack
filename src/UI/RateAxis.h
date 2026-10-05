#pragma once

// Pure Y-axis sizing for non-negative history charts (rates, counts, watts), extracted so it can be
// unit-tested without a live ImPlot context, following CONTRIBUTING.md's "extract the pure decision
// logic into a small header" pattern (as ProcessTreeFlatten.h and ProcessTreeIndent.h do).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>

namespace UI::Widgets
{

/// Minimum spans for the non-negative history axes, chosen so an all-zero window renders one
/// readable decade rather than a sliver around zero.
inline constexpr double RATE_AXIS_MIN_SPAN_BYTES_PER_SEC = 1024.0; // 1 KB/s
inline constexpr double RATE_AXIS_MIN_SPAN_BYTES = 1024.0;         // 1 KiB (a size, not a rate)
inline constexpr double RATE_AXIS_MIN_SPAN_WATTS = 1.0;            // 1 W
inline constexpr double RATE_AXIS_MIN_SPAN_COUNT = 10.0;           // 10 items
/// Smallest span of a percent axis that scales to its data (#1195): small enough that a process using
/// half a percent of the machine draws a visible line, large enough that noise does not fill the chart.
inline constexpr double PERCENT_AXIS_MIN_SPAN = 5.0; // 5 %

/// Headroom above the observed maximum, so the peak of a series is not drawn flush against the top
/// of the plot.
inline constexpr double RATE_AXIS_HEADROOM = 1.10;

/// Upper bound for a non-negative history axis whose lower bound is pinned to 0.
///
/// ImPlot's auto-fit cannot express "non-negative, with a floor". Given an all-zero series its
/// ApplyFit() hits `ImAlmostEqual(Range.Min, Range.Max)` and pads to +/-0.5, which renders as a
/// negative rate (impossible for bytes/sec or watts) plus a column of identical tick labels, since
/// every tick in that sliver formats to the same string. Its constraint API cannot fix it either:
/// Constrain() clamps to ConstraintRange first and *then* expands symmetrically to satisfy
/// ConstraintZoom, pushing the minimum back below zero. So the limits are computed here from the
/// data instead and applied as fixed limits (see #920).
///
/// @param dataMax  Largest value plotted in the window; negatives and NaN are treated as 0.
/// @param minSpan  Smallest acceptable axis span, e.g. RATE_AXIS_MIN_SPAN_BYTES_PER_SEC.
/// @return An upper bound >= minSpan, with headroom above dataMax.
[[nodiscard]] inline double rateAxisUpperBound(double dataMax, double minSpan) noexcept
{
    const double safeMax = (std::isfinite(dataMax) && dataMax > 0.0) ? dataMax : 0.0;
    const double safeSpan = (std::isfinite(minSpan) && minSpan > 0.0) ? minSpan : 1.0;
    return std::max(safeMax * RATE_AXIS_HEADROOM, safeSpan);
}

/// Upper bound for a percent axis that scales to its data instead of always spanning 0-100 (#1195):
/// rateAxisUpperBound() with a PERCENT_AXIS_MIN_SPAN floor, never above 100. A process's CPU is a
/// percent of the whole machine, so one busy thread on 16 logical CPUs is 6.25 % and a typical process
/// well under 1 %: on a fixed 0-100 axis it was a flat line at zero.
[[nodiscard]] inline double percentAxisUpperBound(double dataMax, double minSpan = PERCENT_AXIS_MIN_SPAN) noexcept
{
    return std::min(100.0, rateAxisUpperBound(dataMax, minSpan));
}

/// Most labels a history chart's Y axis shows, however tall it is (#1202): maximized, ImPlot's own
/// tick density put 21 labels on a percent axis.
inline constexpr int AXIS_MAX_TICKS = 8;

/// Labels a Y axis of `plotHeightPx` can show without crowding: one per two text lines, at least
/// 2 (0 and the top) and at most AXIS_MAX_TICKS. A non-positive or non-finite height gets the cap.
[[nodiscard]] inline int axisMaxTicksForHeight(float plotHeightPx, float lineHeightPx) noexcept
{
    if (!std::isfinite(plotHeightPx) || plotHeightPx <= 0.0F || !std::isfinite(lineHeightPx) || lineHeightPx <= 0.0F)
    {
        return AXIS_MAX_TICKS;
    }
    const double fit = std::floor(static_cast<double>(plotHeightPx) / (2.0 * static_cast<double>(lineHeightPx)));
    return static_cast<int>(std::clamp(fit, 2.0, static_cast<double>(AXIS_MAX_TICKS)));
}

/// The smallest "nice" tick step -- 1, 2 or 5 times a power of ten -- that spaces at most
/// `maxTicks` labels (both ends included) across `span` (#1202). So 0-100 % steps by 20, 0-9.5 MB/s
/// by 2 MB/s (with niceBinaryAxisStep()), and 0-5 % by 1.
///
/// @return The step, or 0 if `span` is not a positive finite number.
[[nodiscard]] inline double niceAxisStep(double span, int maxTicks) noexcept
{
    if (!std::isfinite(span) || span <= 0.0)
    {
        return 0.0;
    }
    const int intervals = std::max(1, maxTicks - 1);
    const double raw = span / static_cast<double>(intervals);
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    // A relative tolerance, so a raw step that is a nice number up to rounding (2.0000000001) is
    // not pushed up to the next one.
    constexpr double TOLERANCE = 1e-9;
    for (const double multiple : {1.0, 2.0, 5.0})
    {
        const double step = multiple * magnitude;
        if (step >= raw * (1.0 - TOLERANCE))
        {
            return step;
        }
    }
    return 10.0 * magnitude;
}

/// niceAxisStep() for a byte axis: the 1-2-5 step is chosen in the axis's binary unit (`unitScale`,
/// e.g. 1024 * 1024 for MB) and returned in bytes, so ticks fall on 2 MB rather than on
/// 2,000,000 bytes (1.9 MB).
[[nodiscard]] inline double niceBinaryAxisStep(double spanBytes, double unitScale, int maxTicks) noexcept
{
    const double scale = (std::isfinite(unitScale) && unitScale > 0.0) ? unitScale : 1.0;
    return niceAxisStep(spanBytes / scale, maxTicks) * scale;
}

/// Ticks from 0 at every multiple of a step up to an axis's upper bound.
struct AxisTickRange
{
    double last = 0.0; ///< Largest multiple of the step not above the bound
    int count = 0;     ///< Number of ticks, 0 included; 0 if there are none
};

/// The ticks from 0 to `upper` at multiples of `step` (niceAxisStep()), for ImPlot::SetupAxisTicks.
[[nodiscard]] inline AxisTickRange axisTickRange(double upper, double step) noexcept
{
    if (!std::isfinite(upper) || upper <= 0.0 || !std::isfinite(step) || step <= 0.0)
    {
        return {};
    }
    // Tolerance, so an upper bound that is a multiple of the step up to rounding keeps its tick.
    const double intervals = std::floor((upper / step) + 1e-9);
    // Bounded by the caller's maxTicks in practice; the cap only guards a nonsensical step.
    constexpr double MAX_INTERVALS = 1000.0;
    const double clamped = std::min(intervals, MAX_INTERVALS);
    return {.last = clamped * step, .count = static_cast<int>(clamped) + 1};
}

/// Largest finite, non-negative value across the given series. Empty input yields 0.
///
/// Templated over the range rather than fixed to std::span<const float>: the system panels keep
/// their history in std::vector<float> while the process details panel uses std::vector<double>,
/// and both feed the same axis.
template<std::ranges::input_range R> [[nodiscard]] double maxOfSeries(const R& series) noexcept
{
    double best = 0.0;
    for (const auto& element : series)
    {
        const auto value = static_cast<double>(element);
        if (std::isfinite(value) && value > best)
        {
            best = value;
        }
    }
    return best;
}

/// Largest finite, non-negative value across several series plotted on the same axis.
template<std::ranges::input_range... Rs>
    requires(sizeof...(Rs) >= 2)
[[nodiscard]] double maxOfSeries(const Rs&... series) noexcept
{
    return std::max({maxOfSeries(series)...});
}

/// Index of the first entry of the ascending time axis @p x at or after @p xMin; x.size() if none is.
[[nodiscard]] inline std::size_t firstIndexAtOrAfter(std::span<const double> x, double xMin) noexcept
{
    return static_cast<std::size_t>(std::ranges::lower_bound(x, xMin) - x.begin());
}

/// maxOfSeries() over the samples a chart's window shows: those at x >= @p xMin on the time axis
/// @p x, where @p series is aligned to the tail of @p x (its last value is at x.back(), as with
/// tailAlignedSpan()). Values with no x, or before xMin, are left out.
///
/// History trimming keeps one sample before the window's left edge, so a chart's line runs off that
/// edge (HistoryUtils::keepTrimAnchor, #1016), and scrolling back leaves older samples off-screen.
/// Neither is drawn, so neither may set the axis or a peak line: a peak just left of the window kept
/// a rate axis scaled to it with nothing visible near the top (#1145). The right edge is not checked:
/// the newest sample can be stamped a moment after the frame's "now", a little right of x = 0.
template<std::ranges::sized_range R>
    requires std::ranges::random_access_range<const R>
[[nodiscard]] double maxOfSeriesSince(std::span<const double> x, double xMin, const R& series) noexcept
{
    const std::size_t visible = x.size() - firstIndexAtOrAfter(x, xMin);
    const auto count = static_cast<std::size_t>(std::ranges::size(series));
    const std::size_t skip = (count > visible) ? count - visible : 0;
    return maxOfSeries(std::ranges::subrange(std::ranges::begin(series) + static_cast<std::ptrdiff_t>(skip), std::ranges::end(series)));
}

/// maxOfSeriesSince() across several series plotted on the same axis, each aligned to the tail of @p x.
template<std::ranges::sized_range... Rs>
    requires(sizeof...(Rs) >= 2)
[[nodiscard]] double maxOfSeriesSince(std::span<const double> x, double xMin, const Rs&... series) noexcept
{
    return std::max({maxOfSeriesSince(x, xMin, series)...});
}

/// A current value a NowBar shows, or NaN -- which withCurrentValues() ignores -- when the bar shows
/// N/A instead (an unreadable counter, a series the chart does not draw).
[[nodiscard]] inline double currentIfAvailable(bool available, double value) noexcept
{
    return available ? value : std::numeric_limits<double>::quiet_NaN();
}

/// The target for an axis whose NowBars show smoothed current values: the larger of @p visibleMax
/// (maxOfSeriesSince()) and every finite @p current value. Non-finite values are ignored, so an
/// unavailable reading (currentIfAvailable()) does not move the axis.
///
/// The axis is sized to the samples in the window (#1145), but a bar's smoothed value can still be
/// easing down from a peak that has just scrolled out of it, and when a tab resumes the axis restarts
/// at the lower target. Either way the bar would exceed the axis and normalizeToUnitInterval() would
/// clamp it to full height, disagreeing with its own value. Folding the bars' values in keeps every
/// bar on the axis (#1003). An initializer_list, so per-frame callers allocate nothing (#1171).
[[nodiscard]] inline double withCurrentValues(double visibleMax, std::initializer_list<double> current) noexcept
{
    double best = (std::isfinite(visibleMax) && visibleMax > 0.0) ? visibleMax : 0.0;
    for (const double value : current)
    {
        if (std::isfinite(value) && value > best)
        {
            best = value;
        }
    }
    return best;
}

/// Time constants for easing a rate chart's Y upper bound toward rateAxisUpperBound() (#1011).
/// Rising is quick, so a new peak is clipped for only a few frames; falling is slower, so the chart
/// settles rather than snapping when a peak scrolls out of the window.
inline constexpr double RATE_AXIS_EASE_UP_TAU_SECONDS = 0.12;
inline constexpr double RATE_AXIS_EASE_DOWN_TAU_SECONDS = 0.5;
/// Within this fraction of the target the eased bound snaps to it, so it settles exactly instead of
/// creeping toward it forever (and the axis labels stop changing).
inline constexpr double RATE_AXIS_EASE_SNAP_FRACTION = 0.002;

/// One frame of easing a rate chart's Y upper bound from `current` toward `target`.
///
/// Rate charts recompute their upper bound from the visible data every frame, so when a peak entered
/// or left the window the whole chart rescaled in a single frame. This moves the bound exponentially
/// instead, with RATE_AXIS_EASE_UP/DOWN_TAU_SECONDS. A non-finite or non-positive `current` (no
/// previous bound) or `deltaSeconds` returns `target` unchanged.
[[nodiscard]] inline double easeAxisUpperBound(double current, double target, double deltaSeconds) noexcept
{
    if (!std::isfinite(current) || current <= 0.0 || !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0 || !std::isfinite(target))
    {
        return target;
    }
    const double tau = (target > current) ? RATE_AXIS_EASE_UP_TAU_SECONDS : RATE_AXIS_EASE_DOWN_TAU_SECONDS;
    const double alpha = 1.0 - std::exp(-deltaSeconds / tau);
    const double next = current + ((target - current) * alpha);
    return (std::abs(next - target) <= std::abs(target) * RATE_AXIS_EASE_SNAP_FRACTION) ? target : next;
}

/// One chart's eased upper bound, carried from frame to frame.
struct EasedBound
{
    double value = 0.0;
    int lastFrame = -1; ///< ImGui frame the value was last computed for; -1 = never.
};

/// Advance `bound` to `frame` toward `target` and return the bound to draw this frame.
///
/// - Asked again in the frame it was already computed for, it returns the same value: a chart's axis
///   and its NowBars both read it, and must agree (#1003).
/// - Continuing from the previous frame, it eases (easeAxisUpperBound).
/// - Otherwise -- never drawn, or not drawn last frame (its tab was hidden) -- it starts at the
///   target rather than easing in from a stale value.
[[nodiscard]] inline double stepEasedBound(EasedBound& bound, double target, int frame, double deltaSeconds) noexcept
{
    if (bound.lastFrame == frame)
    {
        return bound.value;
    }
    const bool continuing = bound.lastFrame == frame - 1;
    bound.value = continuing ? easeAxisUpperBound(bound.value, target, deltaSeconds) : target;
    bound.lastFrame = frame;
    return bound.value;
}

} // namespace UI::Widgets
