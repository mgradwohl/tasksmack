#pragma once

#include "Core/AnimationRequest.h"
#include "Domain/Numeric.h"
#include "UI/Format.h"
#include "UI/RateAxis.h"
#include "UI/RenderMetrics.h"
#include "UI/StyleScale.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UI::Widgets
{

namespace Detail
{
// Single global toggle, mirrors the pattern of other small UI-owned render state (e.g.
// RenderMetrics' enabled flag). Must be a named-namespace `inline` variable (external linkage,
// one instance program-wide), not an anonymous-namespace one -- this header is included from
// multiple translation units, and an anonymous-namespace variable would give each TU its own
// separate copy, silently breaking the "one shared toggle" contract
// setChartAntiAliasingEnabled()/chartAntiAliasingEnabled() below are meant to provide.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline bool g_ChartAntiAliasingEnabled = true;

// The frame on which an eased Y axis last asked for full-rate frames (easedChartUpperBound()), or -1.
// The axis is eased before its chart is drawn, so the request is held here and made by the next
// HistoryChart only if that chart is actually visible (#1125, #1281 review).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline int g_PendingEaseRequestFrame = -1;
} // namespace Detail

/// Whether a HistoryChart should ask for full-rate frames for an axis that was eased just before it:
/// only when an ease request is pending from this same frame and the chart is visible (BeginPlot
/// returned true). A chart clipped below a scrolling child, whose axis is still easing, asks for
/// nothing.
[[nodiscard]] constexpr bool shouldRequestEaseFrames(int pendingFrame, int currentFrame, bool chartVisible) noexcept
{
    return chartVisible && pendingFrame >= 0 && pendingFrame == currentFrame;
}

/// Whether history chart plots render anti-aliased lines (see CHART_ANTI_ALIASING_FLAGS_MASK's
/// doc comment for why "lines", not "lines/fills": ImPlot's shaded-fill path doesn't currently
/// consult the fill AA bit at all). Mirrors App::UserConfig::Settings::chartAntiAliasing, but UI
/// must not depend on App (see tasksmack.md's Dependency Rules), so the App composition root
/// pushes this value in via setChartAntiAliasingEnabled() once at startup and again whenever the
/// setting changes, instead of ChartWidgets reading UserConfig directly. Defaults to true
/// (current visual behavior preserved) until the composition root sets it. Disabling trades
/// chart-edge smoothness for lower CPU/GPU cost -- profiling showed Dear ImGui's AddPolyline/
/// PathArcToFastEx as a real, non-trivial share of both idle and interactive frame time
/// (perf-plan #843 phase 1).
inline void setChartAntiAliasingEnabled(bool enabled) noexcept
{
    Detail::g_ChartAntiAliasingEnabled = enabled;
}

[[nodiscard]] inline bool chartAntiAliasingEnabled() noexcept
{
    return Detail::g_ChartAntiAliasingEnabled;
}

/// The ImDrawList AA bits HistoryChart overrides for the lifetime of one chart. Correction vs.
/// this feature's original commit message: verified against the vendored implot_items.cpp that
/// ImPlot 1.0's line renderer checks AntiAliasedLines/AntiAliasedLinesUseTex, but its shaded-fill
/// renderer (RendererShaded::Render, backing PlotShaded) does not consult AntiAliasedFill at all
/// -- it always emits the same triangle-strip geometry regardless of that bit. Clearing it here
/// is therefore harmless-but-currently-inert for ImPlot's fills specifically (kept for
/// forward-compatibility and because other draw-list content within the plot region, e.g.
/// ImGui's own filled shapes, does honor it); the real, measured benefit is confined to line/
/// gridline rendering (AddPolyline/_PathArcToFastEx).
inline constexpr ImDrawListFlags CHART_ANTI_ALIASING_FLAGS_MASK =
    ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedLinesUseTex | ImDrawListFlags_AntiAliasedFill;

/// Pure bit-manipulation backing HistoryChart's anti-aliasing override, extracted so it's
/// testable without a live ImGui context (see CONTRIBUTING.md's "extract the pure decision
/// logic into a small header" pattern). Clears exactly CHART_ANTI_ALIASING_FLAGS_MASK's bits
/// from `flags`, preserving every other bit untouched.
[[nodiscard]] constexpr ImDrawListFlags clearChartAntiAliasingFlags(ImDrawListFlags flags) noexcept
{
    return flags & ~CHART_ANTI_ALIASING_FLAGS_MASK;
}

// NoMouseText: every history chart has its own hover tooltip (renderHistoryTooltip), so ImPlot's raw
// "-16, 31.0" cursor readout was a second, unlabelled and partly hidden readout of the same point (#1039).
inline constexpr ImPlotFlags PLOT_FLAGS_DEFAULT = ImPlotFlags_NoMenus | ImPlotFlags_NoMouseText;
inline constexpr ImPlotAxisFlags X_AXIS_FLAGS_DEFAULT = ImPlotAxisFlags_NoHighlight;
inline constexpr ImPlotAxisFlags Y_AXIS_FLAGS_DEFAULT = ImPlotAxisFlags_NoHighlight;
inline constexpr float HISTORY_PLOT_HEIGHT_DEFAULT = 180.0F;
/// Width of one "now" bar beside a history chart, in ems: 24px at the reference em (32/3 px), the
/// fixed pixel width it replaces. As pixels the bars ignored both the Font Size setting and the
/// display's density, so on a 175% display they were 24 physical pixels beside 37px text: thin, and
/// a small hover target for the only place their value is shown, the tooltip (#971).
inline constexpr float NOW_BAR_WIDTH_EM = 2.25F;

/// Width of one "now" bar in whole pixels at the given em (ImGui::GetFontSize()).
[[nodiscard]] inline float nowBarWidth(float emPx) noexcept
{
    const float em = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;
    return std::max(1.0F, std::round(NOW_BAR_WIDTH_EM * em));
}
inline constexpr double SMOOTH_FACTOR = 0.5; // fraction of refresh interval used for tau
inline constexpr double TAU_MS_MIN = 20.0;
inline constexpr double TAU_MS_MAX = 400.0;
inline constexpr int LINE_PLOT_MAX_POINTS_DENSE = 720;

/// RAII guard that pushes the chart font (see UI::chartFontSize()) for axis labels, legends and hints.
class PlotFontGuard
{
  public:
    PlotFontGuard()
    {
        ImFont* chartFont = UI::Theme::get().chartFont();
        if (chartFont != nullptr)
        {
            ImGui::PushFont(chartFont);
            m_FontPushed = true;
        }
    }

    ~PlotFontGuard()
    {
        if (m_FontPushed)
        {
            ImGui::PopFont();
        }
    }

    PlotFontGuard(const PlotFontGuard&) = delete;
    PlotFontGuard& operator=(const PlotFontGuard&) = delete;
    PlotFontGuard(PlotFontGuard&&) = delete;
    PlotFontGuard& operator=(PlotFontGuard&&) = delete;

  private:
    bool m_FontPushed = false;
};

inline double computeAlpha(double deltaTimeSeconds, std::chrono::milliseconds refreshInterval)
{
    const double baseIntervalMs = Domain::Numeric::toDouble(refreshInterval.count());
    const double tauMs = std::clamp(baseIntervalMs * SMOOTH_FACTOR, TAU_MS_MIN, TAU_MS_MAX);
    const double dtMs = (deltaTimeSeconds > 0.0) ? deltaTimeSeconds * 1000.0 : baseIntervalMs;
    return std::clamp(1.0 - std::exp(-dtMs / std::max(1.0, tauMs)), 0.0, 1.0);
}

inline double computeAlpha(float deltaTimeSeconds, std::chrono::milliseconds refreshInterval)
{
    return computeAlpha(Domain::Numeric::toDouble(deltaTimeSeconds), refreshInterval); // Explicit: float seconds -> double smoothing math
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

inline std::string formatAgeSeconds(double relativeSeconds)
{
    const double ageSeconds = std::abs(relativeSeconds);
    return std::format("Age: {:.1f}s", ageSeconds);
}

/// One row of a history chart's hover tooltip: a series' label and colour -- the same ones its
/// legend entry and NowBar use -- and its value at the hovered sample, already formatted ("N/A" for
/// a sample with no reading; see formatSampleOrNA).
struct TooltipRow
{
    std::string_view label;
    ImVec4 color;
    std::string value;
};

/// "label: value", the text of one tooltip row.
[[nodiscard]] inline std::string formatTooltipRow(std::string_view label, std::string_view value)
{
    return std::format("{}: {}", label, value);
}

/// `format(value)`, or "N/A" for a non-finite value: a history sample with no reading is NaN.
template<typename Format> [[nodiscard]] std::string formatSampleOrNA(double value, Format&& format)
{
    return std::isfinite(value) ? std::string(std::forward<Format>(format)(value)) : std::string("N/A");
}

/// Side of the colour swatch before each tooltip row, as a fraction of the text line height.
inline constexpr float TOOLTIP_SWATCH_LINE_FRACTION = 0.7F;

/// The tooltip every history chart shows on hover (#1020): the hovered sample's age, a separator,
/// then one row per series: a swatch in the series' colour and "label: value" in the normal text
/// colour. Charts used to write this out by hand, and the copies drifted -- whole-second ages,
/// colours matching nothing on the chart, series left out, labels different from the legend's.
///
/// The text was drawn in the series colour until #1192. Series colours are tuned to be seen as
/// lines (3:1), not read as text (4.5:1), and CPU Idle's was close to the tooltip's own background,
/// so the colour moved to an opaque swatch and the text stays readable in every theme.
inline void renderHistoryTooltip(double relativeSeconds, std::span<const TooltipRow> rows)
{
    ImGui::BeginTooltip();
    const std::string age = formatAgeSeconds(relativeSeconds);
    ImGui::TextUnformatted(age.c_str());
    ImGui::Separator();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float side = std::floor(lineHeight * TOOLTIP_SWATCH_LINE_FRACTION);
    for (const auto& row : rows)
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float inset = std::floor((lineHeight - side) * 0.5F);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + inset),
                                                  ImVec2(at.x + side, at.y + inset + side),
                                                  ImGui::ColorConvertFloat4ToU32(withAlpha(row.color, 1.0F)));
        ImGui::Dummy(ImVec2(side, lineHeight));
        ImGui::SameLine();
        const std::string text = formatTooltipRow(row.label, row.value);
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::EndTooltip();
}

/// Calls `onRun(start, length)` for each maximal run of finite values in `values[0, count)`.
///
/// NaN marks a sample with no reading. Splitting a series into its finite runs is how a gap is drawn
/// as a gap by renderers that do not handle NaN themselves (ImPlot's shaded renderer, #989).
// onRun is called once per run, so it is used as an lvalue rather than forwarded; a forwarding
// reference keeps mutable lambdas and stateful functors working.
template<typename T, typename OnRun>
inline void forEachFiniteRun(const T* values, int count, OnRun&& onRun) // NOLINT(cppcoreguidelines-missing-std-forward)
{
    int runStart = 0;
    while (runStart < count)
    {
        while (runStart < count && !std::isfinite(static_cast<double>(values[runStart])))
        {
            ++runStart;
        }
        int runEnd = runStart;
        while (runEnd < count && std::isfinite(static_cast<double>(values[runEnd])))
        {
            ++runEnd;
        }
        if (runEnd > runStart)
        {
            onRun(runStart, runEnd - runStart);
        }
        runStart = runEnd;
    }
}

/// One point a history-chart reduction keeps: the index of the source sample it is drawn from, and
/// whether it is drawn as a gap (NaN: no reading) instead of that sample's value. The reductions
/// below emit these rather than copying values, so the choice of points can be cached and replayed
/// every frame (ReducedPointsCache, #1139): buckets are anchored in absolute time, so the choice
/// depends only on the samples, not on "now", and holds until the data changes.
struct ReducedPoint
{
    int index = 0;
    bool gap = false;

    [[nodiscard]] bool operator==(const ReducedPoint&) const = default;
};

/// The stride reduction behind reduceSeriesKeepingGaps(): calls `emit(sourceIndex, isGap)` once for
/// each of the `outCount` (> 1, < count) points it keeps, in order.
// emit is called once per point, so it is used as an lvalue rather than forwarded.
template<typename TY, typename Emit>
inline void
forEachStrideReducedPoint(const TY* yData, int count, int outCount, Emit&& emit) // NOLINT(cppcoreguidelines-missing-std-forward)
{
    int previousSource = -1;
    for (int resultIdx = 0; resultIdx < outCount; ++resultIdx)
    {
        const std::size_t numerator = static_cast<std::size_t>(resultIdx) * static_cast<std::size_t>(count - 1);
        const auto denominator = static_cast<std::size_t>(outCount - 1);
        const int sourceIdx = static_cast<int>(numerator / denominator);

        bool gap = false;
        if constexpr (std::is_floating_point_v<TY>)
        {
            for (int skipped = previousSource + 1; skipped <= sourceIdx; ++skipped)
            {
                if (!std::isfinite(yData[skipped]))
                {
                    gap = true;
                    break;
                }
            }
        }
        emit(sourceIdx, gap);
        previousSource = sourceIdx;
    }
}

/// Writes the point a reduction kept as output point `written` of `outX`/`outY`: the source sample,
/// or NaN in y for a gap point.
template<typename TX, typename TY>
inline void writeReducedPoint(const TX* xData, const TY* yData, int sourceIdx, bool gap, int written, TX* outX, TY* outY)
{
    outX[written] = xData[sourceIdx];
    if constexpr (std::is_floating_point_v<TY>)
    {
        outY[written] = gap ? std::numeric_limits<TY>::quiet_NaN() : yData[sourceIdx];
    }
    else
    {
        outY[written] = yData[sourceIdx];
    }
}

/// Stride-reduce `count` samples to `outCount` (> 1, < count) points in `outX`/`outY`, keeping gaps.
///
/// Output point k takes source sample s_k = k * (count - 1) / (outCount - 1). A plain stride would
/// skip any NaN that falls between two picked samples and draw straight across a missing reading,
/// so if any sample in (s_{k-1}, s_k] is non-finite, point k's value is NaN instead.
template<typename TX, typename TY>
inline void reduceSeriesKeepingGaps(const TX* xData, const TY* yData, int count, int outCount, TX* outX, TY* outY)
{
    int written = 0;
    forEachStrideReducedPoint(yData,
                              count,
                              outCount,
                              [&](int sourceIdx, bool gap)
                              {
                                  writeReducedPoint(xData, yData, sourceIdx, gap, written, outX, outY);
                                  ++written;
                              });
}

/// Width, in x units, of the buckets reduceSeriesMinMax() groups a series spanning `span` into, for
/// at most `bucketCount` buckets: span / bucketCount rounded *up* to a power of two. Rounding makes
/// the width a step function of the span, so it stays put while the span drifts by a sample as the
/// window scrolls -- a width that moved every sample would move every bucket boundary with it.
/// Returns 0 for an empty or unusable span.
[[nodiscard]] inline double minMaxBucketWidth(double span, int bucketCount) noexcept
{
    if (!std::isfinite(span) || span <= 0.0 || bucketCount <= 0)
    {
        return 0.0;
    }
    return std::exp2(std::ceil(std::log2(span / static_cast<double>(bucketCount))));
}

/// Reduce `count` samples to at most `maxOut` points for drawing, keeping peaks and gaps (#1010).
///
/// The samples are grouped into buckets of minMaxBucketWidth() along x, and each bucket contributes
/// its lowest and highest sample, in their original order. Unlike picking every k-th sample, this
/// keeps a single-sample peak, so the line agrees with the tooltip, which reads full-resolution data.
///
/// Bucket boundaries are fixed in absolute x -- `x + xOffset` -- not counted from either end of the
/// series. History charts plot x as "seconds before now", so with `xOffset` = now a sample keeps its
/// bucket as the window scrolls and the reduced line does not shimmer; counted from an end, every new
/// or trimmed sample would regroup the whole series.
///
/// Gaps (NaN: no reading) survive the reduction. A bucket with one run of non-finite samples emits
/// a NaN point at the run's start: every other point it emits lies wholly before or after the run,
/// so none is drawn connected across it. A bucket with two or more separate runs cannot show them
/// all within its budget, so it collapses to a single NaN point (plus the series' end samples if it
/// holds them): a short stretch drawn as missing rather than a line drawn across a gap.
/// The first and last samples are always emitted, so the line still starts at the oldest sample and
/// ends at the newest instead of at its bucket's extremes.
///
/// The points are emitted as `emit(sourceIndex, isGap)`, in order: reduceSeriesMinMax() writes them
/// out, and reduceSeriesMinMaxPoints() keeps them for a ReducedPointsCache.
///
/// @return Points emitted, at most `maxOut`. With an unusable span (fewer than two samples, or x not
///         increasing) the series is stride-reduced to `maxOut` points instead.
// emit is called once per point, so it is used as an lvalue rather than forwarded.
template<typename TX, typename TY, typename Emit>
inline int forEachMinMaxReducedPoint(const TX* xData,
                                     const TY* yData,
                                     int count,
                                     int maxOut,
                                     double xOffset,
                                     Emit&& emit) // NOLINT(cppcoreguidelines-missing-std-forward)
{
    // At most three points per bucket (min, max, gap marker) plus the two end samples, and a span
    // of n widths can touch n + 1 buckets once both ends fall mid-bucket: so (maxOut - 2) / 3 - 1
    // buckets always fit.
    const int bucketCount = ((maxOut - 2) / 3) - 1;
    const double width =
        (count > 1) ? minMaxBucketWidth(static_cast<double>(xData[count - 1]) - static_cast<double>(xData[0]), bucketCount) : 0.0;
    if (width <= 0.0)
    {
        const int outCount = std::min(count, maxOut);
        if (outCount == count)
        {
            for (int i = 0; i < count; ++i)
            {
                emit(i, false);
            }
        }
        else
        {
            forEachStrideReducedPoint(yData, count, outCount, emit);
        }
        return outCount;
    }

    const auto bucketOf = [&](int index)
    {
        return std::floor((static_cast<double>(xData[index]) + xOffset) / width);
    };
    int written = 0;
    int bucketStart = 0;
    while (bucketStart < count)
    {
        const double bucket = bucketOf(bucketStart);
        int minIdx = -1;
        int maxIdx = -1;
        int gapIdx = -1;
        int gapRuns = 0;
        bool inGap = false;
        int next = bucketStart;
        for (; next < count && bucketOf(next) == bucket; ++next)
        {
            const auto value = static_cast<double>(yData[next]);
            if (!std::isfinite(value))
            {
                gapIdx = (gapIdx < 0) ? next : gapIdx;
                gapRuns += inGap ? 0 : 1;
                inGap = true;
                continue;
            }
            inGap = false;
            if (minIdx < 0 || value < static_cast<double>(yData[minIdx]))
            {
                minIdx = next;
            }
            if (maxIdx < 0 || value > static_cast<double>(yData[maxIdx]))
            {
                maxIdx = next;
            }
        }

        const int firstIdx = (bucketStart == 0) ? 0 : -1;
        const int lastIdx = (next == count) ? count - 1 : -1;
        // Two or more gap runs: drop the bucket's extremes and keep only the gap (see above).
        const bool collapse = gapRuns > 1;
        std::array<int, 5> picks{firstIdx, collapse ? -1 : minIdx, collapse ? -1 : maxIdx, gapIdx, lastIdx};
        std::ranges::sort(picks);
        int previous = -1;
        for (const int pick : picks)
        {
            if (pick < 0 || pick == previous || written >= maxOut)
            {
                continue;
            }
            emit(pick, pick == gapIdx);
            ++written;
            previous = pick;
        }
        bucketStart = next;
    }
    return written;
}

/// Reduce `count` samples to at most `maxOut` points in `outX`/`outY` (each must hold `maxOut`),
/// keeping peaks and gaps; see forEachMinMaxReducedPoint() for how the points are chosen.
///
/// @return Points written to outX/outY.
template<typename TX, typename TY>
[[nodiscard]] inline int reduceSeriesMinMax(const TX* xData, const TY* yData, int count, int maxOut, double xOffset, TX* outX, TY* outY)
{
    int written = 0;
    return forEachMinMaxReducedPoint(xData,
                                     yData,
                                     count,
                                     maxOut,
                                     xOffset,
                                     [&](int sourceIdx, bool gap)
                                     {
                                         writeReducedPoint(xData, yData, sourceIdx, gap, written, outX, outY);
                                         ++written;
                                     });
}

/// The points reduceSeriesMinMax() keeps, as source indices in `out` (cleared first) rather than
/// values, for a ReducedPointsCache.
template<typename TX, typename TY>
inline void
reduceSeriesMinMaxPoints(const TX* xData, const TY* yData, int count, int maxOut, double xOffset, std::vector<ReducedPoint>& out)
{
    out.clear();
    if (count <= 0 || maxOut <= 0)
    {
        return;
    }
    out.reserve(static_cast<std::size_t>(std::min(count, maxOut)));
    std::ignore =
        forEachMinMaxReducedPoint(xData,
                                  yData,
                                  count,
                                  maxOut,
                                  xOffset,
                                  [&out](int sourceIdx, bool gap) { out.push_back(ReducedPoint{.index = sourceIdx, .gap = gap}); });
}

/// Most series reduceAlignedSeries() can select points by (see there).
inline constexpr std::size_t MAX_ALIGNED_KEY_SERIES = 4;

/// The point selection behind reduceAlignedSeries() and reduceAlignedPoints(): series sharing the x
/// axis `x`, reduced to at most `maxOut` common points, each emitted as `emit(sourceIndex, isGap)` in
/// ascending source order. Requires alignedReductionApplies(), and every keyed series as long as
/// `x`. See reduceAlignedSeries() for how the points are chosen.
// emit is called once per point, so it is used as an lvalue rather than forwarded.
template<typename T, typename Emit>
inline void forEachAlignedReducedPoint(std::span<const double> x,
                                       std::span<const std::span<const T>> keyed,
                                       int maxOut,
                                       double xOffset,
                                       Emit&& emit) // NOLINT(cppcoreguidelines-missing-std-forward)
{
    const int count = UI::Format::checkedCount(x.size());
    const auto keyCount = static_cast<int>(keyed.size());
    const auto valueAt = [](std::span<const T> series, int index)
    {
        return static_cast<double>(series[static_cast<std::size_t>(index)]);
    };

    int written = 0;
    const auto keep = [&](int pick, bool asGap)
    {
        emit(pick, asGap);
        ++written;
    };

    // At most three points per keyed series per bucket plus the two end samples, over at most
    // bucketCount + 1 buckets (see forEachMinMaxReducedPoint()).
    const int bucketCount = ((maxOut - 2) / (3 * keyCount)) - 1;
    const double width = (bucketCount > 0) ? minMaxBucketWidth(x.back() - x.front(), bucketCount) : 0.0;
    if (width <= 0.0)
    {
        // As reduceSeriesKeepingGaps(): a point whose stride skipped a gap in any keyed series is a gap.
        int previousSource = -1;
        for (int k = 0; k < maxOut; ++k)
        {
            const auto source = static_cast<int>((static_cast<std::size_t>(k) * static_cast<std::size_t>(count - 1)) /
                                                 static_cast<std::size_t>(maxOut - 1));
            bool skippedGap = false;
            for (const auto series : keyed)
            {
                for (int i = previousSource + 1; i <= source && !skippedGap; ++i)
                {
                    skippedGap = !std::isfinite(valueAt(series, i));
                }
            }
            keep(source, skippedGap);
            previousSource = source;
        }
        return;
    }

    const auto bucketOf = [&](int index)
    {
        return std::floor((x[static_cast<std::size_t>(index)] + xOffset) / width);
    };
    int bucketStart = 0;
    while (bucketStart < count)
    {
        const double bucket = bucketOf(bucketStart);
        int next = bucketStart;
        while (next < count && bucketOf(next) == bucket)
        {
            ++next;
        }

        std::array<int, (3 * MAX_ALIGNED_KEY_SERIES) + 2> picks{};
        picks.fill(-1);
        std::size_t pickCount = 0;
        std::array<int, MAX_ALIGNED_KEY_SERIES> gapPicks{};
        std::size_t gapCount = 0;
        bool collapseBucket = false;
        picks[pickCount++] = (bucketStart == 0) ? 0 : -1;
        picks[pickCount++] = (next == count) ? count - 1 : -1;
        for (const auto series : keyed)
        {
            int minIdx = -1;
            int maxIdx = -1;
            int gapIdx = -1;
            int gapRuns = 0;
            bool inGap = false;
            for (int i = bucketStart; i < next; ++i)
            {
                const double value = valueAt(series, i);
                if (!std::isfinite(value))
                {
                    gapIdx = (gapIdx < 0) ? i : gapIdx;
                    gapRuns += inGap ? 0 : 1;
                    inGap = true;
                    continue;
                }
                inGap = false;
                if (minIdx < 0 || value < valueAt(series, minIdx))
                {
                    minIdx = i;
                }
                if (maxIdx < 0 || value > valueAt(series, maxIdx))
                {
                    maxIdx = i;
                }
            }
            collapseBucket = collapseBucket || (gapRuns > 1);
            picks[pickCount++] = minIdx;
            picks[pickCount++] = maxIdx;
            picks[pickCount++] = gapIdx;
            gapPicks[gapCount++] = gapIdx;
        }
        if (collapseBucket)
        {
            // Only the gap points and the series' ends survive (see reduceAlignedSeries()).
            picks.fill(-1);
            picks[0] = (bucketStart == 0) ? 0 : -1;
            picks[1] = (next == count) ? count - 1 : -1;
            std::copy_n(gapPicks.begin(), gapCount, picks.begin() + 2);
        }
        std::ranges::sort(picks);
        int previous = -1;
        for (const int pick : picks)
        {
            if (pick < 0 || pick == previous || written >= maxOut)
            {
                continue;
            }
            keep(pick, collapseBucket && pick != 0 && pick != count - 1);
            previous = pick;
        }
        bucketStart = next;
    }
}

/// Whether an aligned reduction has anything to do: series longer than `maxOut`, and a usable number
/// of keyed series. Otherwise the series are drawn as they are.
[[nodiscard]] inline bool alignedReductionApplies(std::size_t count, int maxOut, std::size_t keyCount) noexcept
{
    return maxOut >= 2 && std::cmp_greater(count, maxOut) && keyCount > 0 && keyCount <= MAX_ALIGNED_KEY_SERIES;
}

/// Reduce series that share one x axis to at most `maxOut` common points, in place: the stacked
/// bands and lines a chart draws with ImPlot directly, which plotLineWithFill() cannot reduce
/// because each series would keep different samples and the bands would no longer line up (#1022).
///
/// The same bucketing as reduceSeriesMinMax(), anchored at `xOffset`: each bucket keeps, for every
/// series in `keyed`, its lowest and highest sample and its first gap (NaN), plus the series' first
/// and last samples. The kept indices are then applied to `x`, every `keyed` series and every
/// `carried` series (drawn alongside but not used to choose points), so all stay aligned and each
/// keyed series keeps its peaks. When any keyed series has two or more gap runs in a bucket, the
/// whole bucket collapses: it keeps only its gap points, NaN in every series, plus the series' end
/// samples. Collapsing only that series' extrema would not do: another series' picks would still give
/// it finite points on both sides of a later gap and draw it across that gap (#1061 review), and the
/// carried series are built from the keyed ones. With an unusable span the series are stride-reduced.
/// Series no longer than `maxOut` are left unchanged. Every series must be as long as `x`.
///
/// This reduces on every call. A chart that redraws the same data every frame should instead keep
/// the choice of points in a ReducedPointsCache via reduceAlignedPoints() (#1139).
inline void reduceAlignedSeries(std::vector<double>& x,
                                std::initializer_list<std::vector<double>*> keyed,
                                std::initializer_list<std::vector<double>*> carried,
                                int maxOut,
                                double xOffset)
{
    if (!alignedReductionApplies(x.size(), maxOut, keyed.size()))
    {
        return;
    }

    std::array<std::span<const double>, MAX_ALIGNED_KEY_SERIES> keyedSpans{};
    std::size_t keyedCount = 0;
    for (const auto* series : keyed)
    {
        keyedSpans[keyedCount++] = *series;
    }

    // Keeps source index `pick` as output point `written`. Picks ascend and each is at or after its
    // output slot, so compacting in place never overwrites a sample still to be read.
    // With `asGap`, every series gets NaN there instead of its sample.
    int written = 0;
    const auto keep = [&](int pick, bool asGap)
    {
        const auto copy = [&](std::vector<double>& series)
        {
            series[static_cast<std::size_t>(written)] =
                asGap ? std::numeric_limits<double>::quiet_NaN() : series[static_cast<std::size_t>(pick)];
        };
        x[static_cast<std::size_t>(written)] = x[static_cast<std::size_t>(pick)];
        for (auto* series : keyed)
        {
            copy(*series);
        }
        for (auto* series : carried)
        {
            copy(*series);
        }
        ++written;
    };
    forEachAlignedReducedPoint<double>(x, std::span<const std::span<const double>>(keyedSpans.data(), keyedCount), maxOut, xOffset, keep);

    x.resize(static_cast<std::size_t>(written));
    for (auto* series : keyed)
    {
        series->resize(static_cast<std::size_t>(written));
    }
    for (auto* series : carried)
    {
        series->resize(static_cast<std::size_t>(written));
    }
}

/// The points reduceAlignedSeries() keeps, as source indices in `out` (cleared first) rather than
/// values, for a ReducedPointsCache. Series that need no reduction keep every sample. The keyed
/// series may be float or double; each must be as long as `x`.
template<typename T>
inline void reduceAlignedPoints(
    std::span<const double> x, std::initializer_list<std::span<const T>> keyed, int maxOut, double xOffset, std::vector<ReducedPoint>& out)
{
    out.clear();
    if (!alignedReductionApplies(x.size(), maxOut, keyed.size()))
    {
        out.reserve(x.size());
        for (int i = 0; i < UI::Format::checkedCount(x.size()); ++i)
        {
            out.push_back(ReducedPoint{.index = i, .gap = false});
        }
        return;
    }
    out.reserve(static_cast<std::size_t>(maxOut));
    forEachAlignedReducedPoint<T>(x,
                                  std::span<const std::span<const T>>(keyed.begin(), keyed.size()),
                                  maxOut,
                                  xOffset,
                                  [&out](int sourceIdx, bool gap) { out.push_back(ReducedPoint{.index = sourceIdx, .gap = gap}); });
}

/// A data generation no earlier call has returned, for ReducedPointsCache keys (never 0, which means
/// "not cacheable"). A panel takes a new one whenever the history it charts changes -- a publication
/// adopted, a sample recorded, a selection reset -- and gives it to the chart drawing that history
/// (HistoryChartConfig::dataGeneration). One counter for every source, rather than each model's own
/// version number, so two sources' generations never collide. UI thread only.
[[nodiscard]] inline std::uint64_t nextChartDataGeneration() noexcept
{
    static std::uint64_t generation = 0;
    return ++generation;
}

/// Remembers the points a reduction kept for one series (or one set of aligned series), so a chart
/// that redraws unchanged data every frame -- every history chart does, since its x is "seconds
/// before now" -- replays them instead of reducing its whole history again (#1139). At the largest
/// history setting that is about 18,000 samples per series per frame, against at most
/// LINE_PLOT_MAX_POINTS_DENSE points to replay.
///
/// Sound because the reductions anchor their buckets in absolute time: the indices kept depend only
/// on the samples, which a Key names -- the data generation they were read under, how many there
/// are, and the point budget -- with `dataId` telling apart series that share a generation and a
/// length (two lines in one chart; see seriesFingerprint()). A generation of 0 is never cached: points() then rebuilds on
/// every call, which is the uncached behaviour.
/// A ReducedPointsCache::Key::dataId for a series, from its content rather than its address: the
/// first and last samples' bit patterns. Some series are copied into a buffer rebuilt every frame (a
/// local vector, a normalised copy), so an address would change each frame and the cache would
/// never hit. Two series under the same (plot, label) cache entry, generation and length -- e.g.
/// the network chart switched to another interface -- almost always differ in an end sample; if
/// they don't, only the choice of points is stale until the next generation, never the values.
template<typename TY> [[nodiscard]] std::uintptr_t seriesFingerprint(const TY* yData, int count) noexcept
{
    if (count <= 0)
    {
        return 0;
    }
    const auto bitsOf = [](TY value) -> std::uint64_t
    {
        if constexpr (std::is_floating_point_v<TY>)
        {
            return std::bit_cast<std::uint64_t>(static_cast<double>(value));
        }
        else
        {
            return static_cast<std::uint64_t>(value);
        }
    };
    const std::uint64_t first = bitsOf(yData[0]);
    const std::uint64_t last = bitsOf(yData[count - 1]);
    // Mix so (a, b) and (b, a) differ (boost::hash_combine's constant).
    const std::uint64_t mixed = first ^ (last + 0x9e3779b97f4a7c15ULL + (first << 6U) + (first >> 2U));
    return static_cast<std::uintptr_t>(mixed);
}

class ReducedPointsCache
{
  public:
    struct Key
    {
        std::uint64_t generation = 0;
        std::uintptr_t dataId = 0;
        std::size_t count = 0;
        int maxOut = 0;

        [[nodiscard]] bool operator==(const Key&) const = default;
    };

    /// The points for `key`: the remembered ones if they were built for exactly this key, otherwise
    /// rebuilt by `rebuild(std::vector<ReducedPoint>& out)`, which fills `out`, and remembered.
    // rebuild is called at most once, so it is used as an lvalue rather than forwarded.
    template<typename Rebuild>
    [[nodiscard]] std::span<const ReducedPoint> points(const Key& key, Rebuild&& rebuild) // NOLINT(cppcoreguidelines-missing-std-forward)
    {
        if (!m_Valid || key.generation == 0 || key != m_Key)
        {
            rebuild(m_Points);
            m_Key = key;
            m_Valid = key.generation != 0;
            ++m_RebuildCount;
        }
        return m_Points;
    }

    /// Forget the remembered points, so the next points() call rebuilds whatever its key.
    void invalidate() noexcept
    {
        m_Valid = false;
    }

    /// How many times points() has rebuilt.
    [[nodiscard]] std::uint64_t rebuildCount() const noexcept
    {
        return m_RebuildCount;
    }

  private:
    std::vector<ReducedPoint> m_Points;
    Key m_Key;
    std::uint64_t m_RebuildCount = 0;
    bool m_Valid = false;
};

/// "Now" for history charts, in seconds since the steady_clock epoch, read once per ImGui frame.
///
/// Every chart builds its time axis as `timestamp - historyFrameNowSeconds()` (buildTimeAxis), and
/// plotLineWithFill() adds the same value back to anchor its reduction buckets in absolute time, so
/// x + anchor is exactly the sample's timestamp. If each chart read the clock itself, the anchor and
/// the axis would differ by however long the frame took to reach the chart, and a sample near a
/// bucket boundary could change bucket from one frame to the next -- the shimmer the anchoring exists
/// to prevent.
[[nodiscard]] inline double historyFrameNowSeconds()
{
    static int cachedFrame = -1;
    static double cachedNow = 0.0;
    if (const int frame = ImGui::GetFrameCount(); frame != cachedFrame)
    {
        cachedFrame = frame;
        cachedNow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    return cachedNow;
}

/// A chart line weight authored at the reference configuration (Medium, 100%), scaled with the font
/// and display like the rest of the style (#971). Every ImPlotProp_LineWeight goes through this.
[[nodiscard]] inline float lineWeight(float authoredPx)
{
    return scaledLineWeight(authoredPx, Theme::get().styleScale());
}

/// Extend a history series to x = 0 ("now") by repeating its last value there.
///
/// Samples arrive once per refresh interval while the chart scrolls every frame, so the newest
/// point sits up to an interval left of the right edge: the line stopped short of "now" and jumped
/// forward with each new sample (#1016). Holding the latest reading until the next one -- the usual
/// sample-and-hold reading of a sampled series -- draws it to the edge. Nothing is added when the
/// last sample is a gap (NaN: no reading to hold) or already at or past x = 0.
template<typename T> inline void holdLastValueToNow(std::vector<T>& x, std::vector<T>& y)
{
    if (x.empty() || y.size() != x.size())
    {
        return;
    }
    const auto lastX = static_cast<double>(x.back());
    const auto lastY = static_cast<double>(y.back());
    if (!(lastX < 0.0) || !std::isfinite(lastY))
    {
        return;
    }
    x.push_back(T{0});
    y.push_back(y.back());
}

/// The data generation of the HistoryChart being drawn (HistoryChartConfig::dataGeneration) and the
/// ID of its plot. HistoryChart sets it for its lifetime, so plotLineWithFill() can cache its series'
/// reductions (#1139) without every call site passing a key of its own.
struct ChartDataScope
{
    std::uint64_t generation = 0; // 0: the chart did not name one, so nothing is cached
    ImGuiID plotId = 0;
};

namespace Detail
{
// UI thread only, like everything else in ImGui. Named-namespace inline: one instance program-wide
// (see g_ChartAntiAliasingEnabled above).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline ChartDataScope g_ActiveChartDataScope;
} // namespace Detail

[[nodiscard]] inline ChartDataScope activeChartDataScope() noexcept
{
    return Detail::g_ActiveChartDataScope;
}

/// The reduction cache for one plotLineWithFill() series: its plot and its label. A collision
/// between two series' hashes costs only caching, never correctness, since a cached entry is also
/// keyed on the series' data (ReducedPointsCache::Key). Bounded like easedChartUpperBound()'s
/// state: entries not drawn for a while are dropped once there are many (per-disk and per-interface
/// charts come and go).
[[nodiscard]] inline ReducedPointsCache& seriesReductionCache(ImGuiID plotId, std::string_view label)
{
    struct Entry
    {
        ReducedPointsCache cache;
        int lastFrame = 0;
    };
    static std::unordered_map<std::uint64_t, Entry> entries;
    static int lastPruneFrame = -1;
    constexpr std::size_t PRUNE_ABOVE = 256;
    constexpr int STALE_FRAMES = 600;

    const int frame = ImGui::GetFrameCount();
    if (entries.size() > PRUNE_ABOVE && frame != lastPruneFrame)
    {
        lastPruneFrame = frame;
        std::erase_if(entries, [frame](const auto& entry) { return (frame - entry.second.lastFrame) > STALE_FRAMES; });
    }
    const std::uint64_t key =
        (static_cast<std::uint64_t>(plotId) << 32U) ^ static_cast<std::uint64_t>(std::hash<std::string_view>{}(label));
    Entry& entry = entries[key];
    entry.lastFrame = frame;
    return entry.cache;
}

/// @p lineThickness is authored at the reference configuration; it is scaled by lineWeight().
///
/// Inside a HistoryChart with a data generation (HistoryChartConfig::dataGeneration), a long series'
/// reduction is cached per plot and label and replayed until the generation, the series' buffer or
/// its length changes (#1139). The generation must then cover everything `yData` is computed from.
template<typename TX, typename TY>
inline void plotLineWithFill(const char* label,
                             const TX* xData,
                             const TY* yData,
                             int count,
                             const ImVec4& lineColor,
                             std::optional<ImVec4> fillColor = std::nullopt,
                             float lineThickness = 2.0F,
                             bool drawFill = true,
                             int maxPointCount = LINE_PLOT_MAX_POINTS_DENSE)
{
    if (count <= 0)
    {
        return;
    }

    // ImPlot takes x and y of one type. The time axis is double (buildTimeAxis) while some series
    // are float, so y is drawn as TX: converted into reused buffers when the types differ.
    const auto renderSeries = [&](const TX* plotXData, const TX* plotYData, int plotCount)
    {
        if (drawFill)
        {
            // Callers pass the theme's fill for their series (charts.*_fill); a series with no theme
            // fill gets its line colour at 35 % alpha.
            const ImVec4 fill = fillColor.value_or(ImVec4{lineColor.x, lineColor.y, lineColor.z, lineColor.w * 0.35F});
            // Render fill with same label as line so ImPlot treats them as one series.
            // When user clicks legend to hide the series, both fill and line hide together.
            // Render fill first so line appears on top.
            //
            // A NaN sample means "no reading" and must be a gap. ImPlot's line renderer breaks at
            // NaN by itself, but its shaded renderer has no NaN handling at all, so the fill is drawn
            // run by run over the finite samples only. Each run uses the same label, so the legend
            // still shows one item.
            forEachFiniteRun(
                plotYData,
                plotCount,
                [&](int runStart, int runLength)
                { ImPlot::PlotShaded(label, plotXData + runStart, plotYData + runStart, runLength, 0.0, {ImPlotProp_FillColor, fill}); });
        }

        ImPlot::PlotLine(
            label, plotXData, plotYData, plotCount, {ImPlotProp_LineColor, lineColor, ImPlotProp_LineWeight, lineWeight(lineThickness)});
    };

    // Clamp effective max so the reduction and buffer capacity stay in sync.
    const int effectiveMax = (maxPointCount > 1) ? std::min(maxPointCount, static_cast<int>(LINE_PLOT_MAX_POINTS_DENSE)) : maxPointCount;

    // The points actually drawn, as TX: the series (reduced if it is long), then its last reading held
    // out to x = 0 (holdLastValueToNow). UI thread only; reused, so drawing costs no allocation once
    // the buffers have grown to the longest series.
    static std::vector<TX> drawX;
    static std::vector<TX> drawY;
    if ((effectiveMax > 1) && (count > effectiveMax))
    {
        // x is "seconds before historyFrameNowSeconds()" on every history chart, so adding it back
        // anchors the reduction's buckets in absolute time (see forEachMinMaxReducedPoint).
        const auto reduce = [&](std::vector<ReducedPoint>& out)
        {
            reduceSeriesMinMaxPoints(xData, yData, count, effectiveMax, historyFrameNowSeconds(), out);
        };

        // Inside a HistoryChart that names its data generation, the points chosen are kept and
        // replayed until the data changes, instead of reducing the whole history every frame (#1139).
        // Otherwise -- generation 0 -- they are chosen afresh on every call, as before.
        static ReducedPointsCache uncached; // Scratch only: a generation-0 key is never kept
        const ChartDataScope scope = activeChartDataScope();
        const ReducedPointsCache::Key key{.generation = scope.generation,
                                          .dataId = seriesFingerprint(yData, count),
                                          .count = static_cast<std::size_t>(count),
                                          .maxOut = effectiveMax};
        ReducedPointsCache& cache = (scope.generation != 0) ? seriesReductionCache(scope.plotId, label) : uncached;
        const std::span<const ReducedPoint> points = cache.points(key, reduce);

        drawX.resize(points.size());
        drawY.resize(points.size());
        for (std::size_t k = 0; k < points.size(); ++k)
        {
            const auto source = static_cast<std::size_t>(points[k].index);
            TY value = yData[source];
            if constexpr (std::is_floating_point_v<TY>)
            {
                value = points[k].gap ? std::numeric_limits<TY>::quiet_NaN() : value;
            }
            drawX[k] = xData[source];
            drawY[k] = static_cast<TX>(value);
        }
    }
    else
    {
        drawX.assign(xData, xData + count);
        drawY.assign(yData, yData + count);
    }
    holdLastValueToNow(drawX, drawY);
    renderSeries(drawX.data(), drawY.data(), UI::Format::checkedCount(drawX.size()));
}

/// Helper for line-only rendering, reduced to at most LINE_PLOT_MAX_POINTS_DENSE points (see reduceSeriesMinMax).
/// Fills are intentionally disabled; pass only the line color.
/// NOTE: For visual consistency, prefer plotLineWithFill(..., drawFill=true) to show fills.
/// Use plotDenseLine only for charts that should remain line-only (e.g., sparse event streams).
template<typename TX, typename TY>
inline void plotDenseLine(const char* label, const TX* xData, const TY* yData, int count, const ImVec4& lineColor)
{
    plotLineWithFill(label, xData, yData, count, lineColor, std::nullopt, 2.0F, false, LINE_PLOT_MAX_POINTS_DENSE);
}

// ============================================================================
// Axis formatters for ImPlot Y-axis tick labels
// These use C callbacks required by ImPlot::SetupAxisFormat
// All formatters produce fixed-width output to ensure chart alignment
// ============================================================================

/// Minimum character width for Y-axis labels to ensure all charts align
inline constexpr int AXIS_LABEL_MIN_WIDTH = 8;

/// Format large numbers with K/M/G suffixes (e.g., 400000 -> "400K")
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisLocalized)
inline int formatAxisLocalized(double value, char* buff, int size, void* /*userData*/)
{
    // Clamp tiny values to zero to avoid "-0" display
    if (std::abs(value) < 0.5)
    {
        value = 0.0;
    }

    const double absValue = std::abs(value);
    std::string str;

    if (absValue >= 1'000'000'000.0)
    {
        str = std::format("{:.1f}G", value / 1'000'000'000.0);
    }
    else if (absValue >= 1'000'000.0)
    {
        str = std::format("{:.1f}M", value / 1'000'000.0);
    }
    else if (absValue >= 1'000.0)
    {
        str = std::format("{:.1f}K", value / 1'000.0);
    }
    else
    {
        str = std::format("{:.1f}", value);
    }

    const int len = static_cast<int>(str.size());
    if (len < size)
    {
        std::ranges::copy(str, buff);
        buff[len] = '\0';
        return len;
    }
    return 0;
}

/// Shared body of formatAxisBytes and formatAxisBytesPerSec: scale a byte count to B, KB, MB or GB
/// (binary, matching UI::Format::formatBytes) and append `suffix` ("" or "/s").
inline int formatAxisBinaryBytes(double value, char* buff, int size, std::string_view suffix)
{
    // Clamp tiny values to zero to avoid a "-0B" display
    if (std::abs(value) < 0.5)
    {
        value = 0.0;
    }

    const double absValue = std::abs(value);
    std::string str;

    if (absValue >= 1024.0 * 1024.0 * 1024.0)
    {
        str = std::format("{:.1f}GB{}", value / (1024.0 * 1024.0 * 1024.0), suffix);
    }
    else if (absValue >= 1024.0 * 1024.0)
    {
        str = std::format("{:.1f}MB{}", value / (1024.0 * 1024.0), suffix);
    }
    else if (absValue >= 1024.0)
    {
        str = std::format("{:.1f}KB{}", value / 1024.0, suffix);
    }
    else
    {
        str = std::format("{:.1f}B{}", value, suffix);
    }

    const int len = static_cast<int>(str.size());
    if (len < size)
    {
        std::ranges::copy(str, buff);
        buff[len] = '\0';
        return len;
    }
    return 0;
}

/// Format values as bytes with appropriate unit scaling (B, KB, MB, GB)
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytes)
inline int formatAxisBytes(double value, char* buff, int size, void* /*userData*/)
{
    return formatAxisBinaryBytes(value, buff, size, "");
}

/// Format values as bytes/s with appropriate unit scaling (B/s, KB/s, MB/s, GB/s)
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytesPerSec)
inline int formatAxisBytesPerSec(double value, char* buff, int size, void* /*userData*/)
{
    return formatAxisBinaryBytes(value, buff, size, "/s");
}

/// Format values as watts (always in W with decimal places for consistency)
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisWatts)
inline int formatAxisWatts(double value, char* buff, int size, void* /*userData*/)
{
    // Clamp tiny values to zero to avoid "-0W" display
    if (std::abs(value) < 0.0001)
    {
        value = 0.0;
    }

    std::string str;
    const double absValue = std::abs(value);

    // Always use W with 1 decimal place for visual consistency
    if (absValue >= 1.0)
    {
        str = std::format("{:.1f}W", value);
    }
    else
    {
        // Show small values in mW with 1 decimal place
        str = std::format("{:.1f}mW", value * 1000.0);
    }

    const int len = static_cast<int>(str.size());
    if (len < size)
    {
        std::ranges::copy(str, buff);
        buff[len] = '\0';
        return len;
    }
    return 0;
}

/// Format values as percentages (0-100%)
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisPercent)
inline int formatAxisPercent(double value, char* buff, int size, void* /*userData*/)
{
    // Clamp values that print as zero to zero, to avoid "-0.0%". Only those: a percent axis can now
    // scale down to 5 % (#1195), where ticks such as 0.2 % must not read 0.0 %.
    if (std::abs(value) < 0.05)
    {
        value = 0.0;
    }

    // Use format with % suffix and 1 decimal place for visual consistency
    const auto str = std::format("{:.1f}%", value);
    const int len = static_cast<int>(str.size());
    if (len < size)
    {
        std::ranges::copy(str, buff);
        buff[len] = '\0';
        return len;
    }
    return 0;
}

struct NowBar
{
    std::string valueText;
    std::string label;       // Label used in fallback tooltip construction (e.g., "CPU Total")
    std::string tooltipText; // Rich tooltip text shown on bar hover; falls back to "label: valueText",
                             // then label, then valueText when empty. Leave it empty unless it says more
                             // than that fallback: it is built every frame, the fallback only on hover (#1019).
    double value01 = 0.0;
    ImVec4 color;
};

/// A chart's NowBars, held in place: built every frame without a heap allocation (#1018). A chart
/// has at most five bars (the GPU core chart); the capacity leaves room. Converts to the span
/// renderHistoryWithNowBars() takes.
class NowBarList
{
  public:
    static constexpr std::size_t CAPACITY = 8;

    // Named like std::vector::push_back: NowBarList replaced a std::vector at every call site (#1067).
    void push_back(NowBar bar) // NOLINT(readability-identifier-naming)
    {
        assert(m_Size < CAPACITY && "NowBarList is full: raise CAPACITY");
        if (m_Size < CAPACITY)
        {
            m_Bars[m_Size++] = std::move(bar);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Size;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_Size == 0;
    }

    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions) - stands in for the vector it replaced
    operator std::span<const NowBar>() const noexcept
    {
        return {m_Bars.data(), m_Size};
    }

  private:
    std::array<NowBar, CAPACITY> m_Bars{};
    std::size_t m_Size = 0;
};

[[nodiscard]] inline double normalizeToUnitInterval(double value, double maxValue)
{
    // NaN would pass straight through std::clamp; a bar with no value is empty.
    if (!(maxValue > 0.0) || !std::isfinite(value))
    {
        return 0.0;
    }
    return std::clamp(value / maxValue, 0.0, 1.0);
}

template<typename T> struct TailAlignedSpan
{
    std::span<const T> values;
    std::size_t offset = 0;
};

template<typename T> [[nodiscard]] inline TailAlignedSpan<T> tailAlignedSpan(std::span<const T> data, std::size_t count)
{
    const std::size_t clampedCount = std::min(count, data.size());
    const std::size_t offset = data.size() - clampedCount;
    return {data.subspan(offset, clampedCount), offset};
}

template<typename T> [[nodiscard]] inline TailAlignedSpan<T> tailAlignedSpan(const std::vector<T>& data, std::size_t count)
{
    return tailAlignedSpan(std::span<const T>(data), count);
}

// Returns the tooltip string to display for a NowBar, using the fallback chain:
//   tooltipText (if non-empty) -> "label: valueText" (if both non-empty) -> label -> valueText
// Callers should invoke this only when the bar is actually hovered to avoid per-frame allocations.
[[nodiscard]] inline std::string selectNowBarTooltip(const NowBar& bar)
{
    if (!bar.tooltipText.empty())
    {
        return bar.tooltipText;
    }
    if (!bar.label.empty() && !bar.valueText.empty())
    {
        return std::format("{}: {}", bar.label, bar.valueText);
    }
    if (!bar.label.empty())
    {
        return bar.label;
    }
    return bar.valueText;
}

struct TimeAxisConfig
{
    double xMin = 0.0;
    double xMax = 0.0;
    double span = 0.0;
    double maxOffset = 0.0;
    double clampedOffset = 0.0;
};

inline TimeAxisConfig makeTimeAxisConfig(std::span<const double> timestamps, double maxHistorySeconds, double desiredOffsetSeconds)
{
    TimeAxisConfig cfg;
    cfg.xMin = -maxHistorySeconds;
    cfg.xMax = 0.0;

    if (!timestamps.empty())
    {
        const double earliest = timestamps.front();
        const double latest = timestamps.back();
        cfg.span = std::max(0.0, latest - earliest);
    }

    const double visible = maxHistorySeconds;
    cfg.maxOffset = std::max(0.0, cfg.span - visible);
    cfg.clampedOffset = std::clamp(desiredOffsetSeconds, 0.0, cfg.maxOffset);
    cfg.xMin = -visible - cfg.clampedOffset;
    cfg.xMax = -cfg.clampedOffset;

    return cfg;
}

/// Write the time axis for a history chart into @p out: the newest `desiredCount` timestamps as
/// seconds before `nowSeconds` (pass historyFrameNowSeconds()). Reuses @p out's capacity.
///
/// double, not float: plotLineWithFill() adds now back to x to bucket samples in absolute time
/// (reduceSeriesMinMax), and a float x carries a rounding error that changes as now advances, so a
/// sample near a bucket boundary could still change bucket from frame to frame (#1051 review).
inline void fillTimeAxis(std::vector<double>& out, std::span<const double> timestamps, size_t desiredCount, double nowSeconds)
{
    const size_t n = std::min(desiredCount, timestamps.size());
    const size_t offset = timestamps.size() - n;
    out.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        out[i] = timestamps[offset + i] - nowSeconds;
    }
}

/// fillTimeAxis() into a new vector.
[[nodiscard]] inline std::vector<double> buildTimeAxis(std::span<const double> timestamps, size_t desiredCount, double nowSeconds)
{
    std::vector<double> timeData;
    fillTimeAxis(timeData, timestamps, desiredCount, nowSeconds);
    return timeData;
}

/// Buffers for one frame's time axes, reused from frame to frame (#1018).
///
/// x is "seconds before now", so every history chart rebuilds its time axis every frame; with a new
/// vector each time that was a heap allocation per chart per frame. acquire() hands out the pool's
/// buffers in turn and starts over when the frame number changes, so once each buffer has grown to
/// its chart's length, building the axes allocates nothing. A buffer, and any span of it, stays valid
/// until the same buffer is handed out again in a later frame.
class TimeAxisPool
{
  public:
    [[nodiscard]] std::vector<double>& acquire(int frame)
    {
        if (frame != m_Frame)
        {
            m_Frame = frame;
            m_Next = 0;
        }
        if (m_Next == m_Buffers.size())
        {
            // Growing the outer vector moves the inner ones, which keeps their heap buffers: spans
            // already handed out this frame stay valid.
            m_Buffers.emplace_back();
        }
        return m_Buffers[m_Next++];
    }

    [[nodiscard]] std::size_t bufferCount() const noexcept
    {
        return m_Buffers.size();
    }

  private:
    std::vector<std::vector<double>> m_Buffers;
    std::size_t m_Next = 0;
    int m_Frame = -1;
};

/// The time axis for a history chart (see fillTimeAxis()), in a buffer from this frame's
/// TimeAxisPool rather than a new vector. Valid for the rest of the ImGui frame. UI thread only.
[[nodiscard]] inline std::span<const double> frameTimeAxis(std::span<const double> timestamps, size_t desiredCount, double nowSeconds)
{
    static TimeAxisPool pool;
    auto& buffer = pool.acquire(ImGui::GetFrameCount());
    fillTimeAxis(buffer, timestamps, desiredCount, nowSeconds);
    return buffer;
}

inline auto hoveredIndexFromPlotX(const std::vector<float>& timeData, double mouseX) -> std::optional<size_t>
{
    if (timeData.empty())
    {
        return std::nullopt;
    }

    const float x = UI::Format::toFloatNarrow(mouseX);
    const auto it = std::ranges::lower_bound(timeData, x);

    if (it == timeData.begin())
    {
        return 0U;
    }

    if (it == timeData.end())
    {
        return timeData.size() - 1;
    }

    const auto upperDist = std::distance(timeData.begin(), it);
    if (!std::in_range<size_t>(upperDist))
    {
        return std::nullopt;
    }
    const auto upperIdx = static_cast<size_t>(upperDist); // Safe: checked by std::in_range
    const size_t lowerIdx = upperIdx - 1;

    const float distLower = std::abs(timeData[lowerIdx] - x);
    const float distUpper = std::abs(timeData[upperIdx] - x);

    return (distUpper < distLower) ? upperIdx : lowerIdx;
}

inline auto hoveredIndexFromPlotX(std::span<const double> timeData, double mouseX) -> std::optional<size_t>
{
    if (timeData.empty())
    {
        return std::nullopt;
    }

    const auto it = std::ranges::lower_bound(timeData, mouseX);

    if (it == timeData.begin())
    {
        return 0U;
    }

    if (it == timeData.end())
    {
        return timeData.size() - 1;
    }

    const auto upperDist = std::distance(timeData.begin(), it);
    if (!std::in_range<size_t>(upperDist))
    {
        return std::nullopt;
    }
    const auto upperIdx = static_cast<size_t>(upperDist); // Safe: checked by std::in_range
    const size_t lowerIdx = upperIdx - 1;

    const double distLower = std::abs(timeData[lowerIdx] - mouseX);
    const double distUpper = std::abs(timeData[upperIdx] - mouseX);

    return (distUpper < distLower) ? upperIdx : lowerIdx;
}

/// @param horizontal  Lay the entries out in one row instead of a column; see
///                    HistoryChartConfig::legendHorizontal for when.
inline void setupLegendDefault(bool horizontal = false)
{
    ImPlot::SetupLegend(ImPlotLocation_NorthWest,
                        ImPlotLegendFlags_NoHighlightItem | (horizontal ? ImPlotLegendFlags_Horizontal : ImPlotLegendFlags_None));
}

/// Samples a history chart needs before its "collecting" hint is dropped.
///
/// A chart's time axis spans the whole history window (300 s by default), so the first few samples
/// occupy a few pixels at its right edge and the plot still looks empty. A freshly selected process
/// starts with no history at all, and four blank charts read as a pane that failed to load (#927).
inline constexpr std::size_t HISTORY_COLLECTING_SAMPLE_COUNT = 5;

/// Text shown over a history plot that has too few samples to draw anything visible yet.
inline constexpr const char* HISTORY_COLLECTING_TEXT = "Collecting data...";

/// Whether a chart with `sampleCount` samples should still show its "collecting" hint.
[[nodiscard]] constexpr bool historyChartIsCollecting(std::size_t sampleCount) noexcept
{
    return sampleCount < HISTORY_COLLECTING_SAMPLE_COUNT;
}

/// Draws the "collecting" hint centred in the current plot while it has too few samples to show
/// anything. Must be called between ImPlot::BeginPlot() and EndPlot(), i.e. while a HistoryChart is
/// active, and after any further axis setup (setupSecondaryRateAxis, SetupAxis): it reads the plot's
/// geometry, which locks ImPlot's setup.
inline void drawCollectingHint(std::size_t sampleCount)
{
    if (!historyChartIsCollecting(sampleCount))
    {
        return;
    }

    const ImVec2 plotPos = ImPlot::GetPlotPos();
    const ImVec2 plotSize = ImPlot::GetPlotSize();
    const ImVec2 textSize = ImGui::CalcTextSize(HISTORY_COLLECTING_TEXT);
    const ImVec2 textPos(plotPos.x + std::max(0.0F, (plotSize.x - textSize.x) * 0.5F),
                         plotPos.y + std::max(0.0F, (plotSize.y - textSize.y) * 0.5F));

    ImPlot::PushPlotClipRect();
    ImPlot::GetPlotDrawList()->AddText(textPos, ImGui::ColorConvertFloat4ToU32(Theme::get().scheme().textMuted), HISTORY_COLLECTING_TEXT);
    ImPlot::PopPlotClipRect();
}

/// Declarative configuration for a standard TaskSmack history chart.
/// yLimits set → Y axis locked to that range (percent charts pin 0-100; the non-negative charts
///   compute theirs from the data via rateHistoryConfig()).
/// yLimits empty → Y axis auto-fits the plotted data, including below zero. No chart does this
///   today -- see autoFitHistoryConfig() for why (#920).
struct HistoryChartConfig
{
    const char* id = "";
    double xMin = 0.0;
    double xMax = 0.0;
    ImPlotFormatter yFormatter = formatAxisLocalized;
    std::optional<std::pair<double, double>> yLimits;
    bool showLegend = true;
    /// One row of legend entries instead of a column. ImPlot clips a legend to the plot area: a column
    /// of four entries is taller than a short chart's data area at the largest font presets (the
    /// system CPU chart's User/System/I/O Wait/Total lost its last entry), while a row of long labels
    /// is wider than a narrow chart. So it is per chart: set for a chart with several short labels and
    /// little height, left off for one with long labels (adapter names, GPU engines).
    bool legendHorizontal = false;
    float height = HISTORY_PLOT_HEIGHT_DEFAULT;
    ImPlotFlags flags = PLOT_FLAGS_DEFAULT;
    /// Ease the Y upper bound toward yLimits->second over a few frames instead of jumping to it
    /// (see easeAxisUpperBound). Set by rateHistoryConfig(); a fixed range such as 0-100 % has
    /// nothing to ease.
    bool easeYUpper = false;
    /// The generation of the data this chart draws (nextChartDataGeneration()), or 0 if the caller
    /// does not track one. When set, plotLineWithFill() series drawn in the chart keep their reduced
    /// points until it changes instead of reducing their whole history every frame (#1139), so it
    /// must change whenever anything the series are computed from does. See withDataGeneration().
    std::uint64_t dataGeneration = 0;
};

/// Returns `config` with its data generation set (see HistoryChartConfig::dataGeneration).
[[nodiscard]] inline HistoryChartConfig withDataGeneration(HistoryChartConfig config, std::uint64_t generation)
{
    config.dataGeneration = generation;
    return config;
}

/// Returns `config` with its plot height replaced, for callers that size a chart to the space
/// available (see UI/HistoryPlotHeight.h) rather than taking the default.
[[nodiscard]] inline HistoryChartConfig withHeight(HistoryChartConfig config, float height)
{
    config.height = height;
    return config;
}

/// Returns `config` with its legend laid out in one row (see HistoryChartConfig::legendHorizontal).
[[nodiscard]] inline HistoryChartConfig withHorizontalLegend(HistoryChartConfig config)
{
    config.legendHorizontal = true;
    return config;
}

/// Config for a percent-based history chart: Y axis locked to 0-100 with a percent formatter.
[[nodiscard]] inline HistoryChartConfig percentHistoryConfig(const char* id, double xMin, double xMax)
{
    HistoryChartConfig cfg;
    cfg.id = id;
    cfg.xMin = xMin;
    cfg.xMax = xMax;
    cfg.yFormatter = formatAxisPercent;
    cfg.yLimits = std::pair{0.0, 100.0};
    return cfg;
}

/// Config for an auto-fit history chart: Y axis fits the plotted data, including below zero.
///
/// Prefer rateHistoryConfig() for any series that cannot be negative -- rates, counts and watts all
/// use that instead, so nothing in the app calls this today. Kept for a genuinely signed series:
/// plain auto-fit degenerates on an all-zero window into a +/-0.5 sliver, which renders an
/// impossible negative rate and a column of identical tick labels (#920).
[[nodiscard]] inline HistoryChartConfig autoFitHistoryConfig(const char* id, double xMin, double xMax, ImPlotFormatter yFormatter)
{
    HistoryChartConfig cfg;
    cfg.id = id;
    cfg.xMin = xMin;
    cfg.xMax = xMax;
    cfg.yFormatter = yFormatter;
    return cfg;
}

/// Config for a non-negative history chart (rates, counts, watts): Y axis pinned to 0 at the bottom
/// and drawn up to `upperBound` exactly -- pass easedRateAxisUpperBound(), the bound the chart's
/// NowBars are scaled to as well. See rateAxisUpperBound() in RateAxis.h for why the limits are
/// computed rather than left to ImPlot's auto-fit or its axis constraints.
[[nodiscard]] inline HistoryChartConfig
rateHistoryConfigWithUpper(const char* id, double xMin, double xMax, ImPlotFormatter yFormatter, double upperBound)
{
    HistoryChartConfig cfg;
    cfg.id = id;
    cfg.xMin = xMin;
    cfg.xMax = xMax;
    cfg.yFormatter = yFormatter;
    cfg.yLimits = std::pair{0.0, upperBound};
    // Already the bound to draw (easedRateAxisUpperBound), so HistoryChart does not ease it again.
    cfg.easeYUpper = false;
    return cfg;
}

/// A rate chart config whose upper bound comes from the data (rateAxisUpperBound) and is eased by
/// HistoryChart itself. A chart that also has NowBars should use easedRateAxisUpperBound() and
/// rateHistoryConfigWithUpper() instead, so its bars are scaled to the same per-frame bound (#1003).
[[nodiscard]] inline HistoryChartConfig
rateHistoryConfig(const char* id, double xMin, double xMax, ImPlotFormatter yFormatter, double dataMax, double minSpan)
{
    HistoryChartConfig cfg = rateHistoryConfigWithUpper(id, xMin, xMax, yFormatter, rateAxisUpperBound(dataMax, minSpan));
    cfg.easeYUpper = true;
    return cfg;
}

/// The Y upper bound a HistoryChart with easeYUpper draws this frame: its previous frame's bound
/// eased toward `target` (easeAxisUpperBound). Kept per chart, keyed by the chart's ImGui ID. A chart
/// that was not drawn last frame -- just opened, or its tab just shown -- starts at its target rather
/// than easing in from a stale value.
[[nodiscard]] inline double easedChartUpperBound(ImGuiID chartId, double target)
{
    // UI thread only, like everything else in ImGui. Bounded: one entry per chart ID ever drawn, and
    // entries not drawn for a while are dropped once there are many (per-disk charts come and go).
    static std::unordered_map<ImGuiID, EasedBound> state;
    static int lastPruneFrame = -1;
    constexpr std::size_t PRUNE_ABOVE = 256;
    constexpr int STALE_FRAMES = 600;

    const int frame = ImGui::GetFrameCount();
    // At most once per frame, not once per chart: a full scan per chart would make axis setup
    // quadratic in the number of charts exactly when there are many.
    if (state.size() > PRUNE_ABOVE && frame != lastPruneFrame)
    {
        lastPruneFrame = frame;
        std::erase_if(state, [frame](const auto& entry) { return (frame - entry.second.lastFrame) > STALE_FRAMES; });
    }
    const double bound = stepEasedBound(state[chartId], target, frame, static_cast<double>(ImGui::GetIO().DeltaTime));
    // A bound still easing rescales the whole chart every frame: keep the full animation rate until it
    // settles (easeAxisUpperBound snaps to the target once close), then let the chart idle (#1125) --
    // but only while the chart is visible.
    // easeAxisUpperBound snaps to the target once close, so "settled" is exact; compared with a
    // tolerance relative to the bound's size rather than with ==.
    // The request is held until the chart is known to be visible: see Detail::g_PendingEaseRequestFrame.
    if (std::abs(bound - target) > 1e-9 * std::max(1.0, std::abs(target)))
    {
        Detail::g_PendingEaseRequestFrame = frame;
    }
    return bound;
}

/// The Y upper bound a rate chart draws this frame, for its axis *and* its NowBars (#1003): the bound
/// rateAxisUpperBound() gives for the data, eased toward over a few frames (#1011). Computing it once
/// and passing the result to both -- rateHistoryConfigWithUpper() for the axis, normalizeToUnitInterval()
/// for the bars -- is what keeps a bar level with its line while the axis is still easing; the bars
/// used to scale to the target while the axis drew the eased value.
///
/// @param key  Names the chart's easing state, unique within the current ImGui ID scope: the chart's
///             own ID (e.g. "##ProcIoHistory"), plus a suffix for a second axis ("##.../Y2").
[[nodiscard]] inline double easedRateAxisUpperBound(const char* key, double dataMax, double minSpan)
{
    return easedChartUpperBound(ImGui::GetID(key), rateAxisUpperBound(dataMax, minSpan));
}

/// The Y upper bound a scaling percent chart draws this frame (percentAxisUpperBound(), eased like a
/// rate axis), for its axis and its NowBars alike (#1195, #1003). Pair it with
/// rateHistoryConfigWithUpper(..., formatAxisPercent, bound).
[[nodiscard]] inline double easedPercentAxisUpperBound(const char* key, double dataMax)
{
    return easedChartUpperBound(ImGui::GetID(key), percentAxisUpperBound(dataMax));
}

/// Maps the config's Y policy to ImPlot axis flags: locked range vs auto-fit.
[[nodiscard]] constexpr ImPlotAxisFlags historyChartYAxisFlags(bool hasFixedLimits)
{
    return hasFixedLimits ? (ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT) : (ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
}

/// The `BeginPlot` flags HistoryChart actually uses, folding in showLegend. Previously
/// `showLegend == false` only skipped setupLegendDefault() (which customizes the legend's
/// position/style) without ever setting ImPlotFlags_NoLegend, so ImPlot still rendered a legend
/// -- for the app's ID-only "##Core"-style single-series charts, an empty-text swatch with no
/// functional purpose, still costing per-frame layout/draw work. "No legend" now means no legend
/// (perf-plan #843 phase 1).
[[nodiscard]] constexpr ImPlotFlags historyChartBeginPlotFlags(ImPlotFlags configuredFlags, bool showLegend) noexcept
{
    return showLegend ? configuredFlags : (configuredFlags | ImPlotFlags_NoLegend);
}

/// Set up a right-hand Y2 axis for a series with its own scale -- a rate drawn beside counts, say
/// (#1024) -- from 0 to `upperBound`. Pass easedRateAxisUpperBound() for it, the value its NowBar
/// is scaled to as well. Call right after constructing the HistoryChart, while it is active() and
/// before plotting; then plot that series between ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2) and
/// ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1).
inline void setupSecondaryRateAxis(double upperBound, ImPlotFormatter formatter)
{
    // AuxDefault: no grid lines of its own, and Opposite, which puts its labels on the right.
    ImPlot::SetupAxis(ImAxis_Y2, nullptr, ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
    ImPlot::SetupAxisFormat(ImAxis_Y2, formatter);
    ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, upperBound, ImPlotCond_Always);
}

/// How fast a history chart's data scrolls on screen, in pixels per second: its x axis spans
/// `xMax - xMin` seconds across `plotWidthPx`, and "now" moves one second per second. 0 for an empty
/// span or width (#1125).
[[nodiscard]] constexpr double historyChartScrollPixelsPerSecond(double plotWidthPx, double xMin, double xMax) noexcept
{
    const double spanSeconds = xMax - xMin;
    if (!(spanSeconds > 0.0) || !(plotWidthPx > 0.0))
    {
        return 0.0;
    }
    return plotWidthPx / spanSeconds;
}

/// RAII frame for every history chart in the app: pushes the chart font, begins the plot,
/// and applies the shared legend/axis/format/limit setup so all charts look and behave
/// identically. When the Render Metrics overlay is active it also captures this chart's
/// vertex count and CPU time (not indices -- see ChartRenderSample in RenderMetrics.h for why).
/// Call series-plotting code only when active() is true;
/// extra axis setup (e.g., a Y2 axis) may be added right after construction.
///
/// Usage:
///   const HistoryChart chart(percentHistoryConfig("##CPUHistory", axis.xMin, axis.xMax));
///   if (chart.active()) { ImPlot::PlotLine(...); }
class HistoryChart
{
  public:
    // m_DrawList is captured unconditionally in the initializer list (not just when
    // m_Measure): also needed for the anti-aliasing override below, and
    // ImGui::GetWindowDrawList() is a cheap accessor, not an allocation.
    explicit HistoryChart(const HistoryChartConfig& config)
        : m_DrawList(ImGui::GetWindowDrawList()), m_Id(config.id), m_Measure(RenderMetrics::get().enabled())
    {
        if (m_Measure)
        {
            m_VtxBefore = m_DrawList->VtxBuffer.Size;
            m_Start = std::chrono::steady_clock::now();
        }

        // The ID ImPlot gives this plot (BeginPlot hashes the label in the current ID stack), taken
        // here because ImPlot's public API has no accessor for it once the plot has begun. Scoped by
        // the caller's PushID, so same-label charts in different scopes ease separately.
        const ImGuiID plotId = ImGui::GetID(config.id);
        // The plot fills the available width (size.x = -1); its data area is a little narrower (axis
        // labels), so this slightly overstates the scroll speed -- the safe side for pacing.
        const double plotWidthPx = static_cast<double>(ImGui::GetContentRegionAvail().x);
        m_Active = ImPlot::BeginPlot(config.id, ImVec2(-1, config.height), historyChartBeginPlotFlags(config.flags, config.showLegend));
        // An axis eased just before this chart asks for full-rate frames only if the chart is visible;
        // the pending request is consumed either way, so it can't carry to another chart.
        if (shouldRequestEaseFrames(Detail::g_PendingEaseRequestFrame, ImGui::GetFrameCount(), m_Active))
        {
            Core::AnimationRequest::request();
        }
        Detail::g_PendingEaseRequestFrame = -1;
        if (!m_Active)
        {
            return;
        }
        // A visible history chart scrolls continuously (#1037), at plotWidth / windowSeconds pixels per
        // second: ask for just the frames that motion needs (#1125). BeginPlot is false for a clipped
        // plot, so an off-screen chart asks for nothing.
        Core::AnimationRequest::requestForMotion(historyChartScrollPixelsPerSecond(plotWidthPx, config.xMin, config.xMax));

        // Lets plotLineWithFill() cache this chart's reductions (#1139); restored in the destructor.
        m_PreviousDataScope = Detail::g_ActiveChartDataScope;
        Detail::g_ActiveChartDataScope = ChartDataScope{.generation = config.dataGeneration, .plotId = plotId};
        m_DataScopeSet = true;

        if (!chartAntiAliasingEnabled())
        {
            // ImPlot 1.0 has no per-plot AA flag of its own; it renders through the current
            // window's ImDrawList and its line renderer respects that draw list's AA bits (see
            // CHART_ANTI_ALIASING_FLAGS_MASK's doc comment for the measured scope: line/gridline
            // rendering, not ImPlot's shaded-fill path). Clearing them for the lifetime of this
            // plot -- and restoring them in the destructor -- disables AA for exactly this
            // chart's geometry without touching the ambient ImGuiStyle every other widget uses.
            m_SavedDrawListFlags = m_DrawList->Flags;
            m_DrawList->Flags = clearChartAntiAliasingFlags(m_SavedDrawListFlags);
            m_AntiAliasingOverridden = true;
        }

        if (config.showLegend)
        {
            setupLegendDefault(config.legendHorizontal);
        }
        ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, historyChartYAxisFlags(config.yLimits.has_value()));
        ImPlot::SetupAxisFormat(ImAxis_Y1, config.yFormatter);
        if (config.yLimits.has_value())
        {
            const double upper = config.easeYUpper ? easedChartUpperBound(plotId, config.yLimits->second) : config.yLimits->second;
            ImPlot::SetupAxisLimits(ImAxis_Y1, config.yLimits->first, upper, ImPlotCond_Always);
        }
        ImPlot::SetupAxisLimits(ImAxis_X1, config.xMin, config.xMax, ImPlotCond_Always);
    }

    ~HistoryChart()
    {
        if (m_DataScopeSet)
        {
            Detail::g_ActiveChartDataScope = m_PreviousDataScope;
        }
        if (m_Active)
        {
            ImPlot::EndPlot();
        }

        // Restored after EndPlot (not before): EndPlot may still emit plot-area geometry
        // (e.g. mouse-position text, box-select rectangle) that should honor the same
        // override as the rest of the plot's content.
        if (m_AntiAliasingOverridden && (m_DrawList != nullptr))
        {
            m_DrawList->Flags = m_SavedDrawListFlags;
        }

        // Gate on m_Active: when BeginPlot fails, this scope may span unrelated UI work,
        // so a recorded delta would be misattributed to the chart.
        if (m_Active && m_Measure && (m_DrawList != nullptr))
        {
            const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - m_Start).count();
            RenderMetrics::get().record(m_Id, m_DrawList->VtxBuffer.Size - m_VtxBefore, elapsed, ImGui::GetFrameCount());
        }
    }

    HistoryChart(const HistoryChart&) = delete;
    HistoryChart& operator=(const HistoryChart&) = delete;
    HistoryChart(HistoryChart&&) = delete;
    HistoryChart& operator=(HistoryChart&&) = delete;

    [[nodiscard]] bool active() const noexcept
    {
        return m_Active;
    }

  private:
    PlotFontGuard m_FontGuard;
    ImDrawList* m_DrawList = nullptr;
    std::chrono::steady_clock::time_point m_Start;
    const char* m_Id = "";
    int m_VtxBefore = 0;
    ImDrawListFlags m_SavedDrawListFlags = 0;
    ChartDataScope m_PreviousDataScope;
    bool m_DataScopeSet = false;
    bool m_Measure = false;
    bool m_Active = false;
    bool m_AntiAliasingOverridden = false;
};

/// RAII scope that records draw-list geometry (and CPU time) added between construction and
/// destruction under the given id in the Render Metrics overlay. No-op while capture is off.
class RenderMetricsScope
{
  public:
    /// Builds "baseId + suffix" only when capture is enabled, so disabled builds skip the allocation.
    /// noexcept: purely instrumentation used inside render code — on any failure the
    /// scope silently disables itself for this frame instead of failing UI rendering.
    RenderMetricsScope(const char* baseId, const char* suffix) noexcept : m_Measure(RenderMetrics::get().enabled())
    {
        if (m_Measure)
        {
            try
            {
                m_Id = std::string(baseId) + suffix;
            }
            catch (...)
            {
                m_Measure = false;
                return;
            }
            m_DrawList = ImGui::GetWindowDrawList();
            m_VtxBefore = m_DrawList->VtxBuffer.Size;
            m_Start = std::chrono::steady_clock::now();
        }
    }

    ~RenderMetricsScope()
    {
        if (m_Measure && (m_DrawList != nullptr))
        {
            const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - m_Start).count();
            RenderMetrics::get().record(m_Id, m_DrawList->VtxBuffer.Size - m_VtxBefore, elapsed, ImGui::GetFrameCount());
        }
    }

    RenderMetricsScope(const RenderMetricsScope&) = delete;
    RenderMetricsScope& operator=(const RenderMetricsScope&) = delete;
    RenderMetricsScope(RenderMetricsScope&&) = delete;
    RenderMetricsScope& operator=(RenderMetricsScope&&) = delete;

  private:
    std::string m_Id;
    ImDrawList* m_DrawList = nullptr;
    std::chrono::steady_clock::time_point m_Start;
    int m_VtxBefore = 0;
    bool m_Measure = false;
};

/// Whether renderHistoryWithNowBars() prints each bar's current value above the chart (#1193).
enum class NowBarValues : std::uint8_t
{
    Strip, ///< A line of "swatch label value" entries above the chart
    None,  ///< No strip: grid cells, which show the value in their own label and have a fixed height
};

/// A value strip entry for a series with no NowBar, e.g. the network totals drawn behind a selected
/// interface. The label is a view, typically of a constant; `value` holds a short formatted rate or
/// percent, which fits std::string's small-buffer storage, so building one allocates nothing.
struct ValueStripEntry
{
    std::string_view label;
    std::string value;
    ImVec4 color;
};

namespace Detail
{
/// Lays out one value strip entry: a swatch in `color` (alpha kept, so a translucent series reads as
/// muted), then `head` in muted text -- with `colon` appended when `head` does not already end in one
/// -- and `tail` in primary text. With `wrap`, an entry that does not fit the row starts a new line;
/// without it the row runs on and the container clips it.
inline void drawValueStripEntry(
    std::string_view head, std::string_view tail, const ImVec4& color, bool first, bool wrap, float rowRight, const ImVec4& muted)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float side = std::floor(lineHeight * TOOLTIP_SWATCH_LINE_FRACTION);
    const float inset = std::floor((lineHeight - side) * 0.5F);
    const bool addColon = !head.empty() && !head.ends_with(':');
    const float headWidth = head.empty() ? 0.0F
                                         : ImGui::CalcTextSize(head.data(), head.data() + head.size()).x +
                                               (addColon ? ImGui::CalcTextSize(":").x : 0.0F) + style.ItemInnerSpacing.x;
    const float entryWidth = side + style.ItemInnerSpacing.x + headWidth + ImGui::CalcTextSize(tail.data(), tail.data() + tail.size()).x;
    if (!first)
    {
        ImGui::SameLine(0.0F, style.ItemSpacing.x * 2.0F);
        if (wrap && ImGui::GetCursorPosX() + entryWidth > rowRight)
        {
            ImGui::NewLine();
        }
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(at.x, at.y + inset), ImVec2(at.x + side, at.y + inset + side), ImGui::ColorConvertFloat4ToU32(color));
    ImGui::Dummy(ImVec2(side, lineHeight));
    if (!head.empty())
    {
        ImGui::SameLine(0.0F, style.ItemInnerSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextUnformatted(head.data(), head.data() + head.size());
        if (addColon)
        {
            ImGui::SameLine(0.0F, 0.0F);
            ImGui::TextUnformatted(":");
        }
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0.0F, style.ItemInnerSpacing.x);
    ImGui::TextUnformatted(tail.data(), tail.data() + tail.size());
}
} // namespace Detail

/// How renderNowBarValueStrip() lays out its entries.
enum class ValueStripLayout : std::uint8_t
{
    Wrap,    ///< Each bar's tooltip text; entries that do not fit start a new line
    Compact, ///< One line of "label: valueText": for containers that budget exactly one line (grid
             ///< cells), where a longer tooltip text could run past the edge. The hover keeps it.
};

/// Each series' current value, readable without hovering (#1193): per bar, a swatch in the bar's
/// colour and the same text its tooltip shows -- its tooltipText when it has one (richer, e.g. bytes
/// beside a percent), otherwise the tooltip's own fallback "label: valueText" -- with the leading
/// "label:" muted; then any `extras`, series the chart draws without a bar. Bar strings are already
/// built for the frame, so the bars add no allocation.
inline void renderNowBarValueStrip(std::span<const NowBar> bars,
                                   std::span<const ValueStripEntry> extras = {},
                                   ValueStripLayout layout = ValueStripLayout::Wrap)
{
    const bool wrap = layout == ValueStripLayout::Wrap;
    const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const ImVec4 muted = UI::Theme::get().scheme().textMuted;
    bool first = true;
    for (const NowBar& bar : bars)
    {
        std::string_view head = bar.label;
        std::string_view tail = bar.valueText;
        if (wrap && !bar.tooltipText.empty())
        {
            // A tooltipText that starts with "label:" ("Handles: 266,257") splits like the fallback.
            const std::string_view tip = bar.tooltipText;
            const bool labelled = !bar.label.empty() && tip.starts_with(bar.label) && tip.substr(bar.label.size()).starts_with(':');
            head = labelled ? tip.substr(0, bar.label.size() + 1) : std::string_view{};
            tail = labelled ? tip.substr(bar.label.size() + 1) : tip;
            if (tail.starts_with(' '))
            {
                tail.remove_prefix(1);
            }
        }
        Detail::drawValueStripEntry(head, tail, bar.color, first, wrap, rowRight, muted);
        first = false;
    }
    for (const ValueStripEntry& entry : extras)
    {
        Detail::drawValueStripEntry(entry.label, entry.value, entry.color, first, wrap, rowRight, muted);
        first = false;
    }
}

/// How fast a NowBar's fill moves on screen, in pixels per second, between two frames @p deltaSeconds
/// apart: its 0..1 value went from @p previous01 to @p current01 on a bar @p heightPx tall. 0 when
/// the frame time is unknown (#1125).
[[nodiscard]] inline double nowBarMotionPixelsPerSecond(double previous01, double current01, double heightPx, double deltaSeconds) noexcept
{
    if (!(deltaSeconds > 0.0) || !(heightPx > 0.0))
    {
        return 0.0;
    }
    return std::abs(current01 - previous01) * heightPx / deltaSeconds;
}

namespace Detail
{
/// Ask the frame loop for the frames a NowBar's easing needs (#1125): the bar's on-screen speed since
/// the previous frame, so a bar easing toward a new sample animates smoothly and a settled one stops
/// asking. Before, any visible bar held the loop at the full animation rate forever (#1037). Keyed
/// per bar by @p barId; a bar not drawn last frame (its tab was hidden) starts from rest.
inline void requestNowBarMotion(ImGuiID barId, double value01, float heightPx)
{
    struct LastDrawn
    {
        double value01 = 0.0;
        int frame = -1;
    };
    // UI thread only. Bounded like easedChartUpperBound(): stale bars are dropped once there are many.
    static std::unordered_map<ImGuiID, LastDrawn> state;
    static int lastPruneFrame = -1;
    constexpr std::size_t PRUNE_ABOVE = 256;
    constexpr int STALE_FRAMES = 600;

    const int frame = ImGui::GetFrameCount();
    if (state.size() > PRUNE_ABOVE && frame != lastPruneFrame)
    {
        lastPruneFrame = frame;
        std::erase_if(state, [frame](const auto& entry) { return (frame - entry.second.frame) > STALE_FRAMES; });
    }
    LastDrawn& last = state[barId];
    if (last.frame == frame - 1)
    {
        Core::AnimationRequest::requestForMotion(nowBarMotionPixelsPerSecond(
            last.value01, value01, static_cast<double>(heightPx), static_cast<double>(ImGui::GetIO().DeltaTime)));
    }
    last = LastDrawn{.value01 = value01, .frame = frame};
}

/// The motion-tracking key of bar @p index in the chart named @p tableId, unique within the current
/// ImGui ID scope like the chart itself.
[[nodiscard]] inline ImGuiID nowBarMotionId(const char* tableId, std::size_t index)
{
    ImGui::PushID(tableId);
    const ImGuiID id = ImGui::GetID(static_cast<int>(index));
    ImGui::PopID();
    return id;
}
} // namespace Detail

inline void renderHistoryWithNowBars(const char* tableId,
                                     float plotHeight,
                                     const std::function<void()>& plotFn,
                                     std::span<const NowBar> bars,
                                     bool barsOnly = false,
                                     size_t minBarColumns = 0,
                                     bool compactSpacing = false,
                                     NowBarValues values = NowBarValues::Strip,
                                     std::span<const ValueStripEntry> stripExtras = {})
{
    // Renders a history plot side-by-side with a compact "now" bar column. When barsOnly is true we
    // skip the ImPlot area and show only the bars (used when history is unavailable). The table layout
    // reserves a fixed-width column sized to the larger of the provided bar count or minBarColumns,
    // applying optional compact spacing for tight UI regions. Each bar can show a value label or a
    // custom label; spacing mirrors ImGui style spacing to stay consistent with surrounding widgets.
    if (bars.empty())
    {
        // Scoped by tableId like the table path below (BeginTable pushes its ID), so a chart drawn
        // per item -- one per GPU, per disk -- gets a distinct plot ID either way, and with it its own
        // ImPlot state and point cache (seriesReductionCache keys on the plot ID, #1139).
        ImGui::PushID(tableId);
        plotFn();
        ImGui::PopID();
        return;
    }

    if (values == NowBarValues::Strip)
    {
        // stripExtras: series the chart draws without a bar (a peak line), so the strip lists every
        // series its tooltip does.
        renderNowBarValueStrip(bars, stripExtras);
    }

    if (barsOnly)
    {
        const float widthPerBar = nowBarWidth(ImGui::GetFontSize());
        const ImGuiStyle& style = ImGui::GetStyle();

        const RenderMetricsScope barsScope(tableId, "/bars");
        ImGui::BeginGroup();
        for (size_t i = 0; i < bars.size(); ++i)
        {
            ImGui::PushID(&bars[i]);
            if (i > 0)
            {
                ImGui::SameLine(0.0F, style.ItemSpacing.x);
            }

            drawVerticalBarWithValue("##NowBar", bars[i].value01, bars[i].color, plotHeight, widthPerBar, "", "");
            Detail::requestNowBarMotion(Detail::nowBarMotionId(tableId, i), bars[i].value01, plotHeight);
            if (ImGui::IsItemHovered())
            {
                const std::string tooltip = selectNowBarTooltip(bars[i]);
                if (!tooltip.empty())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(tooltip.c_str());
                    ImGui::EndTooltip();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndGroup();
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const size_t barColumnCount = std::max(bars.size(), minBarColumns);
    const float barColumnCountF = UI::Format::toFloatNarrow(Domain::Numeric::toDouble(barColumnCount));
    const float spacing = (barColumnCount > 1) ? style.ItemSpacing.x * (barColumnCountF - 1.0F) : 0.0F;
    const float barWidth = nowBarWidth(ImGui::GetFontSize());
    const float columnWidth = (barWidth * barColumnCountF) + spacing;

    int pushedVars = 0;
    if (compactSpacing)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0F, style.CellPadding.y));
        ++pushedVars;
    }

    if (ImGui::BeginTable(tableId, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoBordersInBody))
    {
        ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, columnWidth);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        plotFn();

        ImGui::TableNextColumn();

        const float widthPerBar = barWidth;

        const RenderMetricsScope barsScope(tableId, "/bars");
        ImGui::BeginGroup();
        for (size_t i = 0; i < bars.size(); ++i)
        {
            ImGui::PushID(&bars[i]);
            if (i > 0)
            {
                ImGui::SameLine();
            }

            ImGui::BeginGroup();
            drawVerticalBarWithValue("##NowBar", bars[i].value01, bars[i].color, plotHeight, widthPerBar, "", "");
            Detail::requestNowBarMotion(Detail::nowBarMotionId(tableId, i), bars[i].value01, plotHeight);
            if (ImGui::IsItemHovered())
            {
                const std::string tooltip = selectNowBarTooltip(bars[i]);
                if (!tooltip.empty())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(tooltip.c_str());
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndGroup();
            ImGui::PopID();

            if (i + 1 < bars.size())
            {
                ImGui::SameLine(0.0F, style.ItemSpacing.x);
            }
        }
        ImGui::EndGroup();

        ImGui::EndTable();
    }
    else
    {
        plotFn();
    }

    if (pushedVars > 0)
    {
        ImGui::PopStyleVar(pushedVars);
    }
}

/// renderHistoryWithNowBars() for bars listed in place, e.g. `{readBar, writeBar}`: the list's backing
/// array lives on the stack, where a braced std::vector argument allocated every frame (#1018).
inline void renderHistoryWithNowBars(const char* tableId,
                                     float plotHeight,
                                     const std::function<void()>& plotFn,
                                     std::initializer_list<NowBar> bars,
                                     bool barsOnly = false,
                                     size_t minBarColumns = 0,
                                     bool compactSpacing = false,
                                     NowBarValues values = NowBarValues::Strip,
                                     std::span<const ValueStripEntry> stripExtras = {})
{
    renderHistoryWithNowBars(tableId,
                             plotHeight,
                             plotFn,
                             std::span<const NowBar>(bars.begin(), bars.size()),
                             barsOnly,
                             minBarColumns,
                             compactSpacing,
                             values,
                             stripExtras);
}

} // namespace UI::Widgets
