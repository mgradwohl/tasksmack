#pragma once

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
#include <chrono>
#include <cmath>
#include <cstddef>
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
} // namespace Detail

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

/// RAII guard to push smaller font for chart axis labels and legends
/// RAII guard that pushes smaller font for chart rendering.
class PlotFontGuard
{
  public:
    PlotFontGuard()
    {
        ImFont* smallerFont = UI::Theme::get().smallerFont();
        if (smallerFont != nullptr)
        {
            ImGui::PushFont(smallerFont);
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

/// The tooltip every history chart shows on hover (#1020): the hovered sample's age, a separator,
/// then one "label: value" row per series in that series' colour. Charts used to write this out by
/// hand, and the copies drifted -- whole-second ages, colours matching nothing on the chart, series
/// left out, labels different from the legend's.
inline void renderHistoryTooltip(double relativeSeconds, std::span<const TooltipRow> rows)
{
    ImGui::BeginTooltip();
    const std::string age = formatAgeSeconds(relativeSeconds);
    ImGui::TextUnformatted(age.c_str());
    ImGui::Separator();
    for (const auto& row : rows)
    {
        const std::string text = formatTooltipRow(row.label, row.value);
        ImGui::TextColored(row.color, "%s", text.c_str());
    }
    ImGui::EndTooltip();
}

/// Calls `onRun(start, length)` for each maximal run of finite values in `values[0, count)`.
///
/// NaN marks a sample with no reading. Splitting a series into its finite runs is how a gap is drawn
/// as a gap by renderers that do not handle NaN themselves (ImPlot's shaded renderer, #989).
template<typename T, typename OnRun> inline void forEachFiniteRun(const T* values, int count, OnRun&& onRun)
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

/// Stride-reduce `count` samples to `outCount` (> 1, < count) points in `outX`/`outY`, keeping gaps.
///
/// Output point k takes source sample s_k = k * (count - 1) / (outCount - 1). A plain stride would
/// skip any NaN that falls between two picked samples and draw straight across a missing reading,
/// so if any sample in (s_{k-1}, s_k] is non-finite, point k's value is NaN instead.
template<typename TX, typename TY>
inline void reduceSeriesKeepingGaps(const TX* xData, const TY* yData, int count, int outCount, TX* outX, TY* outY)
{
    int previousSource = -1;
    for (int resultIdx = 0; resultIdx < outCount; ++resultIdx)
    {
        const std::size_t numerator = static_cast<std::size_t>(resultIdx) * static_cast<std::size_t>(count - 1);
        const auto denominator = static_cast<std::size_t>(outCount - 1);
        const int sourceIdx = static_cast<int>(numerator / denominator);

        TY value = yData[sourceIdx];
        if constexpr (std::is_floating_point_v<TY>)
        {
            for (int skipped = previousSource + 1; skipped <= sourceIdx; ++skipped)
            {
                if (!std::isfinite(yData[skipped]))
                {
                    value = std::numeric_limits<TY>::quiet_NaN();
                    break;
                }
            }
        }
        outX[resultIdx] = xData[sourceIdx];
        outY[resultIdx] = value;
        previousSource = sourceIdx;
    }
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
/// @return Points written to outX/outY (each must hold `maxOut`). With an unusable span (fewer than
///         two samples, or x not increasing) the series is stride-reduced to `maxOut` points instead.
template<typename TX, typename TY>
[[nodiscard]] inline int reduceSeriesMinMax(const TX* xData, const TY* yData, int count, int maxOut, double xOffset, TX* outX, TY* outY)
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
            std::copy_n(xData, count, outX);
            std::copy_n(yData, count, outY);
        }
        else
        {
            reduceSeriesKeepingGaps(xData, yData, count, outCount, outX, outY);
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
            outX[written] = xData[pick];
            if constexpr (std::is_floating_point_v<TY>)
            {
                outY[written] = (pick == gapIdx) ? std::numeric_limits<TY>::quiet_NaN() : yData[pick];
            }
            else
            {
                outY[written] = yData[pick];
            }
            ++written;
            previous = pick;
        }
        bucketStart = next;
    }
    return written;
}

/// Most series reduceAlignedSeries() can select points by (see there).
inline constexpr std::size_t MAX_ALIGNED_KEY_SERIES = 4;

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
inline void reduceAlignedSeries(std::vector<double>& x,
                                std::initializer_list<std::vector<double>*> keyed,
                                std::initializer_list<std::vector<double>*> carried,
                                int maxOut,
                                double xOffset)
{
    const int count = UI::Format::checkedCount(x.size());
    const auto keyCount = static_cast<int>(keyed.size());
    if (count <= maxOut || maxOut < 2 || keyCount == 0 || keyed.size() > MAX_ALIGNED_KEY_SERIES)
    {
        return;
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

    // At most three points per keyed series per bucket plus the two end samples, over at most
    // bucketCount + 1 buckets (see reduceSeriesMinMax()).
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
            for (const auto* series : keyed)
            {
                for (int i = previousSource + 1; i <= source && !skippedGap; ++i)
                {
                    skippedGap = !std::isfinite((*series)[static_cast<std::size_t>(i)]);
                }
            }
            keep(source, skippedGap);
            previousSource = source;
        }
    }
    else
    {
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
            for (const auto* series : keyed)
            {
                int minIdx = -1;
                int maxIdx = -1;
                int gapIdx = -1;
                int gapRuns = 0;
                bool inGap = false;
                for (int i = bucketStart; i < next; ++i)
                {
                    const double value = (*series)[static_cast<std::size_t>(i)];
                    if (!std::isfinite(value))
                    {
                        gapIdx = (gapIdx < 0) ? i : gapIdx;
                        gapRuns += inGap ? 0 : 1;
                        inGap = true;
                        continue;
                    }
                    inGap = false;
                    if (minIdx < 0 || value < (*series)[static_cast<std::size_t>(minIdx)])
                    {
                        minIdx = i;
                    }
                    if (maxIdx < 0 || value > (*series)[static_cast<std::size_t>(maxIdx)])
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
                // Only the gap points and the series' ends survive (see above).
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

/// @p lineThickness is authored at the reference configuration; it is scaled by lineWeight().
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
        std::array<TX, LINE_PLOT_MAX_POINTS_DENSE> reducedXData{};
        std::array<TY, LINE_PLOT_MAX_POINTS_DENSE> reducedYData{};
        // x is "seconds before historyFrameNowSeconds()" on every history chart, so adding it back
        // anchors the reduction's buckets in absolute time (see reduceSeriesMinMax).
        const int reducedCount =
            reduceSeriesMinMax(xData, yData, count, effectiveMax, historyFrameNowSeconds(), reducedXData.data(), reducedYData.data());
        drawX.assign(reducedXData.begin(), reducedXData.begin() + reducedCount);
        drawY.assign(reducedYData.begin(), reducedYData.begin() + reducedCount);
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
    // Clamp tiny values to zero to avoid "-0" display
    if (std::abs(value) < 0.5)
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
};

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
    return stepEasedBound(state[chartId], target, frame, static_cast<double>(ImGui::GetIO().DeltaTime));
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
        m_Active = ImPlot::BeginPlot(config.id, ImVec2(-1, config.height), historyChartBeginPlotFlags(config.flags, config.showLegend));
        if (!m_Active)
        {
            return;
        }

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

inline void renderHistoryWithNowBars(const char* tableId,
                                     float plotHeight,
                                     const std::function<void()>& plotFn,
                                     const std::vector<NowBar>& bars,
                                     bool barsOnly = false,
                                     size_t minBarColumns = 0,
                                     bool compactSpacing = false)
{
    // Renders a history plot side-by-side with a compact "now" bar column. When barsOnly is true we
    // skip the ImPlot area and show only the bars (used when history is unavailable). The table layout
    // reserves a fixed-width column sized to the larger of the provided bar count or minBarColumns,
    // applying optional compact spacing for tight UI regions. Each bar can show a value label or a
    // custom label; spacing mirrors ImGui style spacing to stay consistent with surrounding widgets.
    if (bars.empty())
    {
        plotFn();
        return;
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

} // namespace UI::Widgets
