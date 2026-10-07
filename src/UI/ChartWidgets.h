#pragma once

#include "Core/AnimationRequest.h"
#include "Domain/Numeric.h"
#include "Domain/SamplingConfig.h"
#include "UI/ColorContrast.h"
#include "UI/Format.h"
#include "UI/InlineText.h"
#include "UI/LineLayout.h"
#include "UI/RateAxis.h"
#include "UI/RenderMetrics.h"
#include "UI/StyleScale.h"
#include "UI/TailAlignedSeries.h" // IWYU pragma: export
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <imgui_internal.h>
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

// Frame-keyed caches (#1181)
// --------------------------
// historyFrameNowSeconds(), frameTimeAxis() (its TimeAxisPool), plotLineWithFill() (its drawX/drawY),
// seriesReductionCache(), easedChartUpperBound() and Detail::requestNowBarMotion() keep function-local
// statics keyed on ImGui::GetFrameCount(). Their contract:
//
//   - Call them only while an ImGui frame is being built (between ImGui::NewFrame() and
//     ImGui::Render()), on the UI thread -- never from onUpdate(), which runs outside the frame, or
//     from another thread. Outside a frame the frame count is the previous frame's: "now" would be
//     stale and the time-axis buffers would carry on that frame's hand-out cycle.
//   - They are process-wide, not per ImGui context: there is one context, and a second one would
//     share (and confuse) their per-frame state.
//   - Never store what they return across frames. A span from frameTimeAxis() is valid only until
//     its buffer is handed out again in a later frame (TimeAxisPool); keep the timestamps and
//     rebuild the axis each frame instead.
//
// Debug builds check the first rule (assertWithinImGuiFrame()); the others are by convention.

/// Asserts, in debug builds, that @p withinFrame holds: a frame-keyed cache (see above) is being used
/// while an ImGui frame is being built. Separate from the ImGui query so the check is testable
/// without an ImGui context.
inline void requireWithinImGuiFrame([[maybe_unused]] bool withinFrame) noexcept
{
    assert(withinFrame && "frame-keyed chart cache used outside an ImGui frame (see ChartWidgets.h, #1181)");
}

/// Whether an ImGui frame is being built right now: between ImGui::NewFrame() and ImGui::Render().
[[nodiscard]] inline bool imguiWithinFrame() noexcept
{
    const ImGuiContext* context = ImGui::GetCurrentContext();
    return (context != nullptr) && context->WithinFrameScope;
}

/// requireWithinImGuiFrame() for the current ImGui context. Compiles to nothing with NDEBUG.
inline void assertWithinImGuiFrame() noexcept
{
#ifndef NDEBUG
    requireWithinImGuiFrame(imguiWithinFrame());
#endif
}
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

/// Most of a chart row's width the "now" column may take: in a narrow pane (Process Details beside
/// the process table) four full-width bars took about 30 % of it (#1300).
inline constexpr float NOW_BAR_COLUMN_MAX_FRACTION = 0.2F;
/// Narrowest a "now" bar gets when the column is capped, in ems: still a bar, and still something to
/// hover for its tooltip.
inline constexpr float NOW_BAR_MIN_WIDTH_EM = 1.0F;

/// Width of one "now" bar in a column of @p barColumnCount bars @p spacingPx apart, beside a chart in
/// a row @p availableWidthPx wide: nowBarWidth(), narrowed in whole pixels so the column takes at most
/// NOW_BAR_COLUMN_MAX_FRACTION of the row, but never below NOW_BAR_MIN_WIDTH_EM (#1300). An unknown
/// row width (not positive) leaves the bars at full width.
[[nodiscard]] inline float fittedNowBarWidth(float emPx, std::size_t barColumnCount, float spacingPx, float availableWidthPx) noexcept
{
    const float full = nowBarWidth(emPx);
    if (barColumnCount == 0 || !std::isfinite(availableWidthPx) || !(availableWidthPx > 0.0F))
    {
        return full;
    }
    const auto count = static_cast<float>(barColumnCount);
    const float spacing = std::isfinite(spacingPx) ? std::max(0.0F, spacingPx) * (count - 1.0F) : 0.0F;
    const float budget = (availableWidthPx * NOW_BAR_COLUMN_MAX_FRACTION) - spacing;
    const float em = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;
    const float minimum = std::max(1.0F, std::round(NOW_BAR_MIN_WIDTH_EM * em));
    return std::clamp(std::floor(budget / count), std::min(minimum, full), full);
}
/// The most points a history series is reduced to, whatever its plot's width: the ceiling under
/// plotPointBudget().
inline constexpr int LINE_PLOT_MAX_POINTS_DENSE = 720;

/// Points a history series is reduced to per pixel column of its plot (#1411): the min/max reduction
/// keeps a bucket's lowest and highest sample, so about two points per column keep every column's
/// extremes in reach while drawing no more than the plot can show.
inline constexpr double LINE_PLOT_POINTS_PER_PIXEL = 2.0;

/// The fewest points plotPointBudget() gives a plot, however narrow: still a recognisable line.
inline constexpr int LINE_PLOT_MIN_POINTS = 64;

/// plotPointBudget() rounds up to a multiple of this, so a plot whose width moves by a fraction of a
/// pixel between frames keeps the same budget, and its ReducedPointsCache entry stays valid.
inline constexpr int LINE_PLOT_POINT_BUDGET_STEP = 16;

/// How many points a history series in a plot @p plotWidth wide (ImGui units) is reduced to (#1411):
/// LINE_PLOT_POINTS_PER_PIXEL per physical pixel column (@p framebufferScale physical pixels per unit),
/// rounded up to LINE_PLOT_POINT_BUDGET_STEP, between LINE_PLOT_MIN_POINTS and @p maxPoints. A narrow
/// sparkline needs a fraction of the points a full-width chart does. An unknown width (not positive,
/// or not finite) gets @p maxPoints, the budget before widths were taken into account.
[[nodiscard]] inline int plotPointBudget(float plotWidth, float framebufferScale, int maxPoints = LINE_PLOT_MAX_POINTS_DENSE) noexcept
{
    if (!std::isfinite(plotWidth) || !(plotWidth > 0.0F) || maxPoints <= 0)
    {
        return maxPoints;
    }
    const float scale = (std::isfinite(framebufferScale) && framebufferScale > 0.0F) ? framebufferScale : 1.0F;
    const double wanted = std::ceil(static_cast<double>(plotWidth) * static_cast<double>(scale) * LINE_PLOT_POINTS_PER_PIXEL);
    constexpr double STEP = LINE_PLOT_POINT_BUDGET_STEP;
    const double stepped = std::ceil(wanted / STEP) * STEP;
    const double clamped =
        std::clamp(stepped, static_cast<double>(std::min(LINE_PLOT_MIN_POINTS, maxPoints)), static_cast<double>(maxPoints));
    return static_cast<int>(clamped);
}

/// RAII guard that pushes the chart font (see UI::chartFontSize()) for axis labels and hints.
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
// One instance program-wide, like g_ChartAntiAliasingEnabled above. Read and written on the UI thread only.
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

/// "Age: 2.5s" for a hovered sample under a minute old, then the duration grammar the time axis
/// uses ("Age: 1m 30s", Format::formatDuration()), not "Age: 90.0s" (#1202).
inline std::string formatAgeSeconds(double relativeSeconds)
{
    const double ageSeconds = std::abs(relativeSeconds);
    if (ageSeconds < 59.95) // Shown to a tenth: from 59.95 it would print as "60.0s"
    {
        return std::format("Age: {:.1Lf}s", ageSeconds);
    }
    return "Age: " + Format::formatDuration(ageSeconds);
}

/// One row of a history chart's hover tooltip: a series' label and colour -- the same ones its
/// value-strip entry and NowBar use -- and its value at the hovered sample, already formatted ("N/A" for
/// a sample with no reading; see formatSampleOrNA).
struct TooltipRow
{
    std::string_view label;
    ImVec4 color;
    std::string value;
};

/// The end of the label of a series drawn on its chart's right-hand axis (setupSecondaryRateAxis(),
/// #1206): "Page Faults →". The label keeps it -- it is the series' ImPlot ID and marker key -- but
/// text that names the series and then gives its value shows it after the value ("Page Faults:
/// 3.2K/s →"), where it points at the axis that value is read on rather than reading as part of the
/// name (#1300). splitSecondaryAxisMark() takes it off.
inline constexpr std::string_view SECONDARY_AXIS_MARK = " →";

/// A series label without its SECONDARY_AXIS_MARK, and whether it had one.
struct SeriesLabelParts
{
    std::string_view name;
    bool rightAxis = false;
};

[[nodiscard]] constexpr SeriesLabelParts splitSecondaryAxisMark(std::string_view label) noexcept
{
    if (label.size() > SECONDARY_AXIS_MARK.size() && label.ends_with(SECONDARY_AXIS_MARK))
    {
        // Built from pointer and length, not substr(), which may throw (bugprone-exception-escape)
        return {.name = std::string_view{label.data(), label.size() - SECONDARY_AXIS_MARK.size()}, .rightAxis = true};
    }
    return {.name = label, .rightAxis = false};
}

/// "label: value", the text of one tooltip row; a right-hand-axis series reads "name: value →"
/// (SECONDARY_AXIS_MARK).
[[nodiscard]] inline std::string formatTooltipRow(std::string_view label, std::string_view value)
{
    const SeriesLabelParts parts = splitSecondaryAxisMark(label);
    return std::format("{}: {}{}", parts.name, value, parts.rightAxis ? SECONDARY_AXIS_MARK : std::string_view{});
}

/// formatTooltipRow() into an InlineText, for a NowBar's tooltipText: no allocation (#1171).
[[nodiscard]] inline InlineText tooltipRowText(std::string_view label, std::string_view value)
{
    const SeriesLabelParts parts = splitSecondaryAxisMark(label);
    return InlineText::format("{}: {}{}", parts.name, value, parts.rightAxis ? SECONDARY_AXIS_MARK : std::string_view{});
}

/// A value-strip entry's text from its bar's tooltipText: `head` the series' name (muted, the strip
/// adds its colon) and `tail` the rest, with any SECONDARY_AXIS_MARK taken off both -- the strip draws
/// it after the value itself (#1300). A tip that doesn't start with "name:" (or "label:") is all tail.
struct StripTextParts
{
    std::string_view head;
    std::string_view tail;
};

[[nodiscard]] constexpr StripTextParts splitStripText(std::string_view tip, std::string_view seriesLabel) noexcept
{
    const SeriesLabelParts label = splitSecondaryAxisMark(seriesLabel);
    const auto prefixLength = [tip](std::string_view name) -> std::size_t
    {
        return (!name.empty() && tip.size() > name.size() && tip.starts_with(name) && tip[name.size()] == ':') ? name.size() + 1 : 0;
    };
    // "name: value →" (tooltipRowText()), or a tip still built from the whole label ("name →: value").
    std::size_t prefix = prefixLength(label.name);
    if (prefix == 0)
    {
        prefix = prefixLength(seriesLabel);
    }
    StripTextParts parts{.head = (prefix != 0) ? label.name : std::string_view{},
                         .tail = std::string_view{tip.data() + prefix, tip.size() - prefix}};
    if (parts.tail.starts_with(' '))
    {
        parts.tail.remove_prefix(1);
    }
    if (label.rightAxis && parts.tail.ends_with(SECONDARY_AXIS_MARK))
    {
        parts.tail.remove_suffix(SECONDARY_AXIS_MARK.size());
    }
    return parts;
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
/// colours matching nothing on the chart, series left out, labels different from the strip's.
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

/// Calls `onRun(start, length)` for each maximal run over which both `lower[i]` and `upper[i]` are
/// finite, for i in [0, count).
///
/// A band filled between two series (ImPlot::PlotShaded with two Y arrays, as the stacked CPU bands
/// are) is drawn only where both of its edges have a reading: ImPlot's shaded renderer has no NaN
/// handling, so a NaN in either edge would otherwise become garbage triangles (#1149).
// onRun is called once per run, so it is used as an lvalue rather than forwarded.
template<typename T, typename OnRun>
inline void forEachJointFiniteRun(const T* lower, const T* upper, int count, OnRun&& onRun) // NOLINT(cppcoreguidelines-missing-std-forward)
{
    const auto finiteAt = [&](int i)
    {
        return std::isfinite(static_cast<double>(lower[i])) && std::isfinite(static_cast<double>(upper[i]));
    };
    int runStart = 0;
    while (runStart < count)
    {
        while (runStart < count && !finiteAt(runStart))
        {
            ++runStart;
        }
        int runEnd = runStart;
        while (runEnd < count && finiteAt(runEnd))
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

/// The integer index of the min-max bucket of width `width` that holds `x`: floor(x / width), taken
/// once, so samples are grouped by comparing integers rather than doubles (#1380). Saturates at
/// +/-2^62 so the conversion is always defined; NaN, which no bucket holds, reads 0.
[[nodiscard]] inline std::int64_t minMaxBucketIndex(double x, double width) noexcept
{
    constexpr double LIMIT = 4611686018427387904.0; // 2^62, exactly representable
    const double bucket = std::floor(x / width);
    if (std::isnan(bucket))
    {
        return 0;
    }
    return static_cast<std::int64_t>(std::clamp(bucket, -LIMIT, LIMIT));
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
        return minMaxBucketIndex(static_cast<double>(xData[index]) + xOffset, width);
    };
    int written = 0;
    int bucketStart = 0;
    while (bucketStart < count)
    {
        const std::int64_t bucket = bucketOf(bucketStart);
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
        return minMaxBucketIndex(x[static_cast<std::size_t>(index)] + xOffset, width);
    };
    int bucketStart = 0;
    while (bucketStart < count)
    {
        const std::int64_t bucket = bucketOf(bucketStart);
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
/// are, and the point budget (which follows the plot's width, plotPointBudget(), so a resize that
/// changes it rebuilds) -- with `dataId` telling apart series that share a generation and a
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
    [[nodiscard]] std::span<const ReducedPoint> points(const Key& key,
                                                       Rebuild&& rebuild) // NOLINT(cppcoreguidelines-missing-std-forward)
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
/// Every chart builds its time axis as `timestamp - historyFrameNowSeconds()` (fillTimeAxis), and
/// plotLineWithFill() adds the same value back to anchor its reduction buckets in absolute time, so
/// x + anchor is exactly the sample's timestamp. If each chart read the clock itself, the anchor and
/// the axis would differ by however long the frame took to reach the chart, and a sample near a
/// bucket boundary could change bucket from one frame to the next -- the shimmer the anchoring exists
/// to prevent.
///
/// Frame-keyed: call it only while a frame is being built (see "Frame-keyed caches" above, #1181).
[[nodiscard]] inline double historyFrameNowSeconds()
{
    Detail::assertWithinImGuiFrame();
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

/// How many of a series' own sample intervals its last reading is held out to "now" for
/// (holdLastValueToNow(), #1147). Above 3 because the samplers slow to 3x the refresh interval while
/// the window is being resized or dragged (AdaptiveIntervalUtils), and the next reading also waits
/// for its publish.
inline constexpr double HOLD_MAX_SAMPLE_INTERVALS = 4.0;
/// The shortest hold limit, in seconds, so a fast refresh (100 ms) doesn't make the line flicker
/// between held and not held on ordinary sampling jitter.
inline constexpr double HOLD_MIN_SECONDS = 1.0;
/// The hold limit for a series with one sample, and so no interval of its own: as long as the slowest
/// refresh interval would allow.
inline constexpr double HOLD_FALLBACK_SECONDS =
    HOLD_MAX_SAMPLE_INTERVALS * static_cast<double>(Domain::Sampling::REFRESH_INTERVAL_MAX_MS) / 1000.0;

/// The longest a series' last reading is held out to "now" (holdLastValueToNow()), in seconds, from
/// its time axis @p x (seconds before now, oldest first): HOLD_MAX_SAMPLE_INTERVALS of the interval
/// between its last two samples, at least HOLD_MIN_SECONDS. Taken from the data rather than from the
/// refresh setting, so it follows the samplers' adaptive slow-downs and needs no plumbing; a stall
/// shows because the interval before it was an ordinary one (#1147).
template<typename T> [[nodiscard]] inline double maxHoldSecondsForAxis(const T* x, int count) noexcept
{
    if (x == nullptr || count < 2)
    {
        return HOLD_FALLBACK_SECONDS;
    }
    const double interval = static_cast<double>(x[count - 1]) - static_cast<double>(x[count - 2]);
    if (!std::isfinite(interval) || !(interval > 0.0))
    {
        return HOLD_FALLBACK_SECONDS;
    }
    return std::max(HOLD_MIN_SECONDS, HOLD_MAX_SAMPLE_INTERVALS * interval);
}

template<typename T> [[nodiscard]] inline double maxHoldSecondsForAxis(std::span<const T> x) noexcept
{
    return maxHoldSecondsForAxis(x.data(), UI::Format::checkedCount(x.size()));
}

/// Whether a series' last sample, @p lastX seconds before now (negative), is held out to x = 0: it is
/// in the past, and no older than @p maxHoldSeconds.
[[nodiscard]] inline bool lastSampleHoldsToNow(double lastX, double maxHoldSeconds) noexcept
{
    return (lastX < 0.0) && (-lastX <= maxHoldSeconds);
}

/// Extend a history series to x = 0 ("now") by repeating its last value there.
///
/// Samples arrive once per refresh interval while the chart scrolls every frame, so the newest
/// point sits up to an interval left of the right edge: the line stopped short of "now" and jumped
/// forward with each new sample (#1016). Holding the latest reading until the next one -- the usual
/// sample-and-hold reading of a sampled series -- draws it to the edge. Nothing is added when the
/// last sample is a gap (NaN: no reading to hold), already at or past x = 0, or older than
/// @p maxHoldSeconds (lastSampleHoldsToNow()): a sampler that has stalled must show as a line that
/// stops, not a flat one that looks live (#1147).
template<typename T> inline void holdLastValueToNow(std::vector<T>& x, std::vector<T>& y, double maxHoldSeconds)
{
    if (x.empty() || y.size() != x.size())
    {
        return;
    }
    const auto lastY = static_cast<double>(y.back());
    if (!lastSampleHoldsToNow(static_cast<double>(x.back()), maxHoldSeconds) || !std::isfinite(lastY))
    {
        return;
    }
    x.push_back(T{0});
    y.push_back(y.back());
}

/// holdLastValueToNow() for series drawn together on one time axis @p x -- a stacked chart's band
/// edges -- so they all reach "now", or none does. Each series gets its own last value repeated
/// (a trailing gap stays a gap: NaN repeated).
template<typename T> inline void holdLastValuesToNow(std::vector<T>& x, std::initializer_list<std::vector<T>*> ys, double maxHoldSeconds)
{
    if (x.empty() || !lastSampleHoldsToNow(static_cast<double>(x.back()), maxHoldSeconds) ||
        std::ranges::any_of(ys, [&x](const std::vector<T>* y) { return y->size() != x.size(); }))
    {
        return;
    }
    x.push_back(T{0});
    for (std::vector<T>* y : ys)
    {
        y->push_back(y->back());
    }
}

/// `values` at each of a reduction's kept `points`, in `out` (resized to match): the sample at the
/// point's source index, or NaN at a gap point (see reduceAlignedSeries()). For a series drawn with
/// ImPlot directly from a ReducedPointsCache's points (#1139).
template<typename T>
inline void gatherReducedValues(std::span<const ReducedPoint> points, std::span<const T> values, std::vector<double>& out)
{
    out.resize(points.size());
    for (std::size_t k = 0; k < points.size(); ++k)
    {
        out[k] = points[k].gap ? std::numeric_limits<double>::quiet_NaN()
                               : static_cast<double>(values[static_cast<std::size_t>(points[k].index)]);
    }
}

/// The x axis and band edges of a stacked User/System CPU chart, which the Overview and Process
/// Details both draw (#1180). PlotShaded fills between two Y series, so the stack needs cumulative
/// tops: the User band from `base` (0) to `userTop` (User), the System band from `userTop` to
/// `systemTop` (User + System).
struct UserSystemStack
{
    std::vector<double> x;         // The kept points' times, held to now by the caller (#1016)
    std::vector<double> base;      // 0: the User band's bottom
    std::vector<double> userTop;   // User
    std::vector<double> systemTop; // User + System
};

/// Builds `out` from a reduction's kept `points` over `time` and the User and System series: a gap
/// point is NaN in every edge but `base` (see reduceAlignedSeries()). Every series must be as long
/// as `time`. Buffers are resized in place, so a chart reusing one UserSystemStack across frames
/// allocates only when it draws more points than before.
template<typename T>
inline void buildUserSystemStack(std::span<const ReducedPoint> points,
                                 std::span<const double> time,
                                 std::span<const T> user,
                                 std::span<const T> system,
                                 UserSystemStack& out)
{
    const std::size_t pointCount = points.size();
    out.x.resize(pointCount);
    out.base.assign(pointCount, 0.0);
    out.userTop.resize(pointCount);
    out.systemTop.resize(pointCount);
    for (std::size_t k = 0; k < pointCount; ++k)
    {
        const auto i = static_cast<std::size_t>(points[k].index);
        out.x[k] = time[i];
        if (points[k].gap)
        {
            out.userTop[k] = out.systemTop[k] = std::numeric_limits<double>::quiet_NaN();
            continue;
        }
        out.userTop[k] = static_cast<double>(user[i]);
        out.systemTop[k] = out.userTop[k] + static_cast<double>(system[i]);
    }
}

/// The data generation of the HistoryChart being drawn (HistoryChartConfig::dataGeneration) and the
/// ID of its plot. HistoryChart sets it for its lifetime, so plotLineWithFill() can cache its series'
/// reductions (#1139) without every call site passing a key of its own.
struct ChartDataScope
{
    std::uint64_t generation = 0; // 0: the chart did not name one, so nothing is cached
    ImGuiID plotId = 0;
    /// Most Y-axis labels the chart has room for (axisMaxTicksForHeight()), so a second Y axis set up
    /// inside it (setupSecondaryRateAxis()) is no denser than the first (#1202).
    int maxYTicks = AXIS_MAX_TICKS;
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
/// charts come and go). Frame-keyed: see "Frame-keyed caches" above (#1181).
[[nodiscard]] inline ReducedPointsCache& seriesReductionCache(ImGuiID plotId, std::string_view label)
{
    Detail::assertWithinImGuiFrame();
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

/// The point budget for a series drawn in the current plot (#1411): plotPointBudget() for the plot
/// area's width, capped at @p maxPoints. Call between BeginPlot and EndPlot, after any axis setup: it
/// reads the plot's geometry, which locks ImPlot's setup (as drawing the series would).
[[nodiscard]] inline int currentPlotPointBudget(int maxPoints = LINE_PLOT_MAX_POINTS_DENSE)
{
    return plotPointBudget(ImPlot::GetPlotSize().x, ImGui::GetIO().DisplayFramebufferScale.x, maxPoints);
}

/// @p lineThickness is authored at the reference configuration; it is scaled by lineWeight().
/// A chart fills one series at most (#1198): a chart with several draws them with plotSeries() and
/// their SeriesRole rather than passing `drawFill` by hand.
///
/// A series longer than its point budget is min/max-reduced to it: @p maxPointCount, but no more than
/// the plot's width calls for (currentPlotPointBudget(), #1411), so a narrow sparkline draws a
/// fraction of a full-width chart's points. Each bucket's extremes are kept, so spikes still show.
///
/// Inside a HistoryChart with a data generation (HistoryChartConfig::dataGeneration), a long series'
/// reduction is cached per plot and label and replayed until the generation, the series' buffer, its
/// length or its point budget (so the plot's width) changes (#1139). The generation must then cover
/// everything `yData` is computed from.
///
/// Draws from function-local scratch buffers: call it only while a frame is being built, on the UI
/// thread (see "Frame-keyed caches" above, #1181).
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
    Detail::assertWithinImGuiFrame();
    if (count <= 0)
    {
        return;
    }

    // ImPlot takes x and y of one type. The time axis is double (fillTimeAxis) while some series
    // are float, so y is drawn as TX: converted into reused buffers when the types differ.
    const auto renderSeries = [&](const TX* plotXData, const TX* plotYData, int plotCount)
    {
        if (drawFill)
        {
            // Callers pass the theme's fill for their series (charts.*_fill); a series with no theme
            // fill gets its line colour at 35 % alpha.
            const ImVec4 fill = fillColor.value_or(ImVec4{lineColor.x, lineColor.y, lineColor.z, lineColor.w * 0.35F});
            // Render fill with same label as line so ImPlot treats them as one series.
            // Render fill first so line appears on top.
            //
            // A NaN sample means "no reading" and must be a gap. ImPlot's line renderer breaks at
            // NaN by itself, but its shaded renderer has no NaN handling at all, so the fill is drawn
            // run by run over the finite samples only. Each run uses the same label, so it is still
            // one series.
            forEachFiniteRun(
                plotYData,
                plotCount,
                [&](int runStart, int runLength)
                { ImPlot::PlotShaded(label, plotXData + runStart, plotYData + runStart, runLength, 0.0, {ImPlotProp_FillColor, fill}); });
        }

        ImPlot::PlotLine(
            label, plotXData, plotYData, plotCount, {ImPlotProp_LineColor, lineColor, ImPlotProp_LineWeight, lineWeight(lineThickness)});
    };

    // At most LINE_PLOT_MAX_POINTS_DENSE, and no more than the plot's width calls for (#1411). The
    // budget is part of the cache key below, so a resize that changes it rebuilds the points.
    const int effectiveMax =
        (maxPointCount > 1) ? currentPlotPointBudget(std::min(maxPointCount, static_cast<int>(LINE_PLOT_MAX_POINTS_DENSE))) : maxPointCount;

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
    holdLastValueToNow(drawX, drawY, maxHoldSecondsForAxis(xData, count)); // Interval from every sample, not the reduced ones
    renderSeries(drawX.data(), drawY.data(), UI::Format::checkedCount(drawX.size()));
}

/// What a series is to the chart it is drawn in, which decides how it is drawn (#1198): colour alone
/// must not be the only thing telling series apart (greyscale, colour-blind readers, overlaps).
enum class SeriesRole : std::uint8_t
{
    /// The chart's main series: the only one with a fill, at full weight. One per chart.
    Primary,
    /// Another series of the chart's own: a lighter line with no fill, and a marker shape of its own
    /// every few seconds (SeriesStyle::marker), so two secondaries differ by more than colour.
    Secondary,
    /// Context behind the series -- totals behind an interface's lines, say: a thin line with a
    /// line-only marker of its own (SeriesStyle::marker), so two references differ by more than colour.
    Reference,
};

/// How a series of a given SeriesRole is drawn. Weights are authored at the reference configuration
/// and scaled by lineWeight().
struct SeriesStyle
{
    bool fill = false;
    float lineWeightPx = 2.0F;
    ImPlotMarker marker = ImPlotMarker_None;
    /// Where in each marker interval this series' markers fall, as a fraction of it, so the markers of
    /// two secondaries are not drawn on top of each other.
    double markerPhase = 0.0;
};

inline constexpr float PRIMARY_SERIES_WEIGHT = 2.0F;
inline constexpr float SECONDARY_SERIES_WEIGHT = 1.5F;
inline constexpr float REFERENCE_SERIES_WEIGHT = 1.0F;
/// Marker shapes of a chart's secondary series, in the order they are drawn. Charts have at most four.
inline constexpr std::array<ImPlotMarker, 4> SECONDARY_SERIES_MARKERS{
    ImPlotMarker_Circle, ImPlotMarker_Square, ImPlotMarker_Diamond, ImPlotMarker_Up};
/// Marker shapes of a chart's reference series: line-only, unlike the secondaries' solid shapes.
inline constexpr std::array<ImPlotMarker, 2> REFERENCE_SERIES_MARKERS{ImPlotMarker_Cross, ImPlotMarker_Plus};
/// About this many markers per series across a chart's time axis.
inline constexpr double SERIES_MARKERS_PER_AXIS = 10.0;
/// Marker radius, authored at the reference configuration like a line weight.
inline constexpr float SERIES_MARKER_RADIUS = 3.0F;

/// The style of a series with role @p role; @p index numbers a chart's secondaries (or its references)
/// from 0 in the order they are drawn, and picks the marker shape (#1198).
[[nodiscard]] constexpr SeriesStyle seriesStyle(SeriesRole role, std::size_t index = 0) noexcept
{
    switch (role)
    {
    case SeriesRole::Primary:
        return SeriesStyle{.fill = true, .lineWeightPx = PRIMARY_SERIES_WEIGHT};
    case SeriesRole::Secondary:
    {
        const std::size_t slot = index % SECONDARY_SERIES_MARKERS.size();
        return SeriesStyle{.fill = false,
                           .lineWeightPx = SECONDARY_SERIES_WEIGHT,
                           .marker = SECONDARY_SERIES_MARKERS[slot],
                           .markerPhase = static_cast<double>(slot) / static_cast<double>(SECONDARY_SERIES_MARKERS.size())};
    }
    case SeriesRole::Reference:
    {
        // Phases offset from the secondaries' (multiples of a quarter) so their markers do not coincide.
        const std::size_t slot = index % REFERENCE_SERIES_MARKERS.size();
        return SeriesStyle{.fill = false,
                           .lineWeightPx = REFERENCE_SERIES_WEIGHT,
                           .marker = REFERENCE_SERIES_MARKERS[slot],
                           .markerPhase = (static_cast<double>(slot) + 0.25) / static_cast<double>(REFERENCE_SERIES_MARKERS.size())};
    }
    }
    return SeriesStyle{};
}

/// Calls `fn(index)` for each sample of a series that carries one of its markers: the first finite
/// sample after each boundary of a grid `intervalSeconds` wide in absolute time (x + anchorSeconds,
/// as plotLineWithFill() anchors its reduction), shifted by `phase` of an interval. Anchored in
/// absolute time, a marker stays on its sample as the chart scrolls; the oldest bucket, which loses
/// samples as history is pruned, gets none, so no marker hops along the left edge.
///
/// `xData` is a history's time axis: finite and ascending. Each boundary is found by binary search,
/// so a frame costs O(markers * log count) rather than a walk of the whole history -- 18,000 samples
/// per series at 30 minutes of 100 ms samples -- for each series, every frame. Only a run of gaps
/// (non-finite values) right after a boundary is stepped through.
///
/// Returns the number of markers placed.
template<typename TX, typename TY, typename Fn>
inline int
forEachMarkerSample(const TX* xData, const TY* yData, int count, double anchorSeconds, double intervalSeconds, double phase, const Fn& fn)
{
    if (count <= 0 || !(intervalSeconds > 0.0) || !std::isfinite(intervalSeconds))
    {
        return 0;
    }
    const auto bucketOf = [&](int i)
    {
        const double x = static_cast<double>(xData[static_cast<std::size_t>(i)]) + anchorSeconds;
        return static_cast<std::int64_t>(std::floor((x / intervalSeconds) + phase));
    };
    // The first sample from `from` on with a finite value, or count.
    const auto nextFinite = [&](int from)
    {
        while (from < count && !std::isfinite(static_cast<double>(yData[static_cast<std::size_t>(from)])))
        {
            ++from;
        }
        return from;
    };

    int placed = 0;
    int i = nextFinite(0);
    if (i >= count)
    {
        return placed;
    }
    std::int64_t lastBucket = bucketOf(i);
    while (true)
    {
        // The first sample past the end of lastBucket: buckets ascend with x.
        int lo = i + 1;
        int hi = count;
        while (lo < hi)
        {
            const int mid = lo + ((hi - lo) / 2);
            if (bucketOf(mid) <= lastBucket)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        i = nextFinite(lo);
        if (i >= count)
        {
            return placed;
        }
        fn(i);
        ++placed;
        lastBucket = bucketOf(i);
    }
}

/// One marker shape of @p radius at @p centre in @p colour: filled, or stroked for the line-only Cross
/// and Plus. Draws a key's shape on a value-strip swatch,
/// matching the series' markers on the data. Defined in ChartLegend.cpp.
void drawMarkerGlyph(ImDrawList& drawList, ImPlotMarker marker, ImVec2 centre, float radius, ImU32 colour);

namespace Detail
{
/// A series' marker shape under its plot label, recorded by plotSeriesMarkers() so the value strip,
/// the chart's only key, can show it on the series' swatch (#1198). The label is a view: the strip
/// looks it up in the same frame, right after the chart (drawPendingStripMarkers()), while the label
/// the series was plotted under -- a constant, or a string the panel keeps -- still exists, and an
/// owned copy allocated every frame for a label too long for the small-string buffer.
struct SeriesMarker
{
    std::string_view label;
    ImPlotMarker marker = ImPlotMarker_None;
    bool operator==(const SeriesMarker&) const = default;
};

/// The markers of the HistoryChart being drawn. UI thread only; HistoryChart clears it as it begins.
[[nodiscard]] inline std::vector<SeriesMarker>& seriesMarkers()
{
    static std::vector<SeriesMarker> markers;
    return markers;
}

/// The marker recorded for the series labelled @p label, or none.
[[nodiscard]] inline ImPlotMarker markerForLabel(std::span<const SeriesMarker> markers, std::string_view label) noexcept
{
    const auto it = std::ranges::find_if(markers, [label](const SeriesMarker& m) { return m.label == label; });
    return it == markers.end() ? ImPlotMarker_None : it->marker;
}

/// A value-strip swatch still waiting for its series' marker shape: the strip is drawn above its
/// chart, before the chart has recorded this frame's markers, so the shape is cut into the swatch once
/// it has (drawPendingStripMarkers()). Using this frame's markers, not the last frame's, the key has
/// its shapes on a chart's first frame too. The label views the bar's or extra's label, which outlive
/// the frame's layout call.
struct PendingStripMarker
{
    std::string_view label;
    ImVec2 centre;
    float radius = 0.0F;
};

/// The swatches of the strip drawn last, waiting for their chart's markers. UI thread only.
[[nodiscard]] inline std::vector<PendingStripMarker>& pendingStripMarkers()
{
    static std::vector<PendingStripMarker> pending;
    return pending;
}

/// Cuts each pending swatch's series marker (from @p markers, the chart just drawn) into its swatch,
/// then forgets them. Called after the chart, outside its layout table, so it draws in the window.
inline void drawPendingStripMarkers(std::span<const SeriesMarker> markers)
{
    auto& pending = pendingStripMarkers();
    if (!pending.empty())
    {
        ImDrawList& drawList = *ImGui::GetWindowDrawList();
        const ImU32 cutOut = ImGui::GetColorU32(ImGuiCol_WindowBg);
        for (const PendingStripMarker& swatch : pending)
        {
            if (const ImPlotMarker marker = markerForLabel(markers, swatch.label); marker != ImPlotMarker_None)
            {
                drawMarkerGlyph(drawList, marker, swatch.centre, swatch.radius, cutOut);
            }
        }
    }
    pending.clear();
}

/// How long a value-strip entry's value must stay narrower than its slot before the slot shrinks.
inline constexpr double VALUE_STRIP_SLOT_SHRINK_DELAY_SECONDS = 3.0;

/// A value-strip entry's width across frames, so a right-aligned strip does not jump each time a
/// value's text changes width ("9.8 KB/s" to "123.4 KB/s").
struct StripSlot
{
    float width = 0.0F;
    double narrowSince = -1.0; ///< When the entry first measured narrower than width; -1 while it is not
};

/// The width to give an entry measuring @p measured at time @p now: a wider entry widens its slot at
/// once; a narrower one shrinks it only once it has stayed narrower for @p shrinkDelaySeconds.
[[nodiscard]] inline float settleStripSlot(StripSlot& slot, float measured, double now, double shrinkDelaySeconds) noexcept
{
    const bool shrinkDue = slot.narrowSince >= 0.0 && now - slot.narrowSince >= shrinkDelaySeconds;
    if (measured >= slot.width || shrinkDue)
    {
        slot.width = measured;
        slot.narrowSince = -1.0;
    }
    else if (slot.narrowSince < 0.0)
    {
        slot.narrowSince = now;
    }
    return slot.width;
}

/// A chart layout's value-strip slots, and when they were last used.
struct StripSlots
{
    std::vector<StripSlot> slots;
    double lastUsed = 0.0;
};

/// Strip slots unused for this long are dropped once there are more than STRIP_SLOTS_PRUNE_ABOVE layouts:
/// layout ids include per-disk device names, so devices that come and go would otherwise leave slots behind.
inline constexpr double STRIP_SLOTS_STALE_SECONDS = 30.0;
inline constexpr std::size_t STRIP_SLOTS_PRUNE_ABOVE = 64;

/// Drops the layouts in @p byLayout not used since @p now - STRIP_SLOTS_STALE_SECONDS, when there are
/// more than STRIP_SLOTS_PRUNE_ABOVE of them.
inline void pruneStaleStripSlots(std::unordered_map<ImGuiID, StripSlots>& byLayout, double now)
{
    if (byLayout.size() > STRIP_SLOTS_PRUNE_ABOVE)
    {
        std::erase_if(byLayout, [now](const auto& entry) { return entry.second.lastUsed < now - STRIP_SLOTS_STALE_SECONDS; });
    }
}

/// Each chart layout's value-strip slots, keyed by the ImGui ID of the layout's id (renderHistoryWithNowBars()'s
/// tableId). UI thread only.
[[nodiscard]] inline std::unordered_map<ImGuiID, StripSlots>& stripSlotsByLayout()
{
    static std::unordered_map<ImGuiID, StripSlots> slots;
    return slots;
}
} // namespace Detail

/// The sample that carries a series' one marker while its history has not yet crossed a marker
/// boundary (forEachMarkerSample() places none): its oldest finite sample, which scrolls with the
/// chart like any marker. Without it a new series -- a process just selected, a counter just
/// available -- would have no marker for up to a whole interval (30 s at the default window), and two
/// secondaries would differ by colour alone. -1 when the series has no finite sample.
template<typename TY> [[nodiscard]] inline int fallbackMarkerSample(const TY* yData, int count) noexcept
{
    for (int i = 0; i < count; ++i)
    {
        if (std::isfinite(static_cast<double>(yData[static_cast<std::size_t>(i)])))
        {
            return i;
        }
    }
    return -1;
}

/// Draws a series' markers (see SeriesStyle::marker), under the series' own label so they are the
/// same plot item as its line, and records its shape for the value strip (Detail::seriesMarkers()). Call between BeginPlot and EndPlot.
template<typename TX, typename TY>
inline void plotSeriesMarkers(const char* label, const TX* xData, const TY* yData, int count, const ImVec4& color, const SeriesStyle& style)
{
    if (style.marker == ImPlotMarker_None)
    {
        return;
    }
    // Recorded even without samples, so the strip's swatch shows the shape from the series' first frame.
    auto& seriesMarkers = Detail::seriesMarkers();
    if (const std::string_view name = label;
        std::ranges::none_of(seriesMarkers, [name](const Detail::SeriesMarker& m) { return m.label == name; }))
    {
        seriesMarkers.push_back(Detail::SeriesMarker{.label = name, .marker = style.marker});
    }
    if (count <= 0)
    {
        return;
    }
    const double axisSpan = ImPlot::GetPlotLimits().X.Size();
    const double interval = axisSpan / SERIES_MARKERS_PER_AXIS;

    static std::vector<TX> markerX; // UI thread only; reused, like plotLineWithFill's buffers
    static std::vector<TX> markerY;
    markerX.clear();
    markerY.clear();
    const auto addMarker = [&](int i)
    {
        const auto index = static_cast<std::size_t>(i);
        markerX.push_back(xData[index]);
        markerY.push_back(static_cast<TX>(yData[index]));
    };
    if (forEachMarkerSample(xData, yData, count, historyFrameNowSeconds(), interval, style.markerPhase, addMarker) == 0)
    {
        if (const int fallback = fallbackMarkerSample(yData, count); fallback >= 0)
        {
            addMarker(fallback);
        }
    }
    if (markerX.empty())
    {
        return;
    }
    ImPlot::PlotScatter(label,
                        markerX.data(),
                        markerY.data(),
                        UI::Format::checkedCount(markerX.size()),
                        {ImPlotProp_Marker,
                         style.marker,
                         ImPlotProp_MarkerSize,
                         lineWeight(SERIES_MARKER_RADIUS),
                         ImPlotProp_MarkerFillColor,
                         color,
                         ImPlotProp_MarkerLineColor,
                         color,
                         ImPlotProp_LineColor,
                         color});
}

/// Draws a history series as its SeriesRole says (seriesStyle()): a primary with its fill, a
/// secondary as a lighter line with markers, a reference as a thin line (#1198). @p fillColor is used
/// only when the style fills.
template<typename TX, typename TY>
inline void plotSeries(const char* label,
                       const TX* xData,
                       const TY* yData,
                       int count,
                       const ImVec4& lineColor,
                       std::optional<ImVec4> fillColor,
                       const SeriesStyle& style)
{
    plotLineWithFill(label, xData, yData, count, lineColor, fillColor, style.lineWeightPx, style.fill, LINE_PLOT_MAX_POINTS_DENSE);
    plotSeriesMarkers(label, xData, yData, count, lineColor, style);
}

/// A band filled between `lower` and `upper` (ImPlot::PlotShaded with two Y arrays), as the stacked
/// CPU charts draw theirs, filled run by run over the points where both edges have a reading.
/// ImPlot's shaded renderer has no NaN handling, so a gap point (a missed sample, or a UI stall that
/// overran the sample ring, #1098) or a band with no reading is drawn as a gap rather than as
/// triangles through NaN (#1149). Call between BeginPlot and EndPlot.
inline void
plotShadedBand(const char* label, const double* xData, const double* lower, const double* upper, int count, const ImVec4& fillColor)
{
    forEachJointFiniteRun(lower,
                          upper,
                          count,
                          [&](int runStart, int runLength)
                          {
                              const auto at = static_cast<std::size_t>(runStart);
                              ImPlot::PlotShaded(label, &xData[at], &lower[at], &upper[at], runLength, {ImPlotProp_FillColor, fillColor});
                          });
}

/// A line drawn with ImPlot directly at `style`'s weight, with its markers (plotSeriesMarkers()): the
/// stacked CPU charts' band edges and User/System lines, whose fill the bands already are, so they
/// cannot go through plotSeries() (#1192, #1198). Call between BeginPlot and EndPlot.
inline void
plotStyledLine(const char* label, const double* xData, const double* yData, int count, const ImVec4& color, const SeriesStyle& style)
{
    ImPlot::PlotLine(label, xData, yData, count, {ImPlotProp_LineColor, color, ImPlotProp_LineWeight, lineWeight(style.lineWeightPx)});
    plotSeriesMarkers(label, xData, yData, count, color, style);
}

// ============================================================================
// Axis formatters for ImPlot Y-axis tick labels
// These use C callbacks required by ImPlot::SetupAxisFormat. Each is a thin adapter over the
// UI::Format function that formats the same quantity as a value, so a tick reads exactly like the
// tooltip and table beside it: "1.5 GB", "45.0 W", "42%", localized (#1202).
// ============================================================================

/// Minimum character width for Y-axis labels to ensure all charts align
inline constexpr int AXIS_LABEL_MIN_WIDTH = 8;

namespace Detail
{
/// Copy `str` into ImPlot's label buffer; 0 and an empty label if it does not fit. The buffer is
/// always left terminated when it has room for a byte: ImPlot ignores the return value and reads the
/// buffer as a C string, so an untouched buffer would show stale text (#1345).
inline int copyAxisLabel(const std::string& str, char* buff, int size)
{
    const int len = static_cast<int>(str.size());
    if (len < size)
    {
        std::ranges::copy(str, buff);
        buff[len] = '\0';
        return len;
    }
    if (size > 0)
    {
        buff[0] = '\0';
    }
    return 0;
}
} // namespace Detail

/// Format large numbers with K/M/G suffixes (e.g., 400000 -> "400.0K")
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisLocalized)
inline int formatAxisLocalized(double value, char* buff, int size, void* /*userData*/)
{
    // Clamp tiny values to zero to avoid "-0" display
    if (std::abs(value) < 0.5)
    {
        value = 0.0;
    }

    const double absValue = std::abs(value);
    double scaled = value;
    std::string_view suffix;
    if (absValue >= 1'000'000'000.0)
    {
        scaled = value / 1'000'000'000.0;
        suffix = "G";
    }
    else if (absValue >= 1'000'000.0)
    {
        scaled = value / 1'000'000.0;
        suffix = "M";
    }
    else if (absValue >= 1'000.0)
    {
        scaled = value / 1'000.0;
        suffix = "K";
    }

    // Straight into ImPlot's buffer, keeping one byte for the terminator (#1334).
    if (size > 1)
    {
        const auto capacity = static_cast<std::size_t>(size - 1);
        std::size_t length = Format::formatFixedLocalizedTo(buff, capacity, scaled, 1);
        if (length > 0)
        {
            Format::appendText(buff, capacity, length, suffix);
            if (length <= capacity)
            {
                buff[length] = '\0';
                return static_cast<int>(length);
            }
        }
        buff[0] = '\0'; // A partial label never reaches ImPlot, which reads the buffer up to its NUL
    }
    return Detail::copyAxisLabel(std::format("{:.1Lf}{}", scaled, suffix), buff, size);
}

/// The unit a byte axis is labelled in, as ImPlot formatter user data: a pointer to one of the
/// UI::Format::BYTE_UNIT_* constants, or nullptr to pick each tick's unit from its own value.
/// ImPlot's user data is a non-const void*; the formatters only ever read through it.
[[nodiscard]] inline void* byteAxisUserData(const Format::ByteUnit& unit) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) -- ImPlot takes void*; only read back as const.
    return const_cast<Format::ByteUnit*>(&unit);
}

/// Shared body of formatAxisBytes and formatAxisBytesPerSec: the value formatter's "1.5 GiB" /
/// "1.5 GiB/s" (IEC units, matching UI::Format::formatBytes). With a unit in `userData`
/// (byteAxisUserData()) every tick uses that one unit, so a 0-2 GiB axis reads 0.5 GiB rather than
/// 512.0 MiB between 0.0 B and 1.0 GiB.
inline int formatAxisBinaryBytes(double value, char* buff, int size, void* userData, bool perSecond)
{
    // Clamp tiny values to zero to avoid a "-0.0 B" display
    if (std::abs(value) < 0.5)
    {
        value = 0.0;
    }

    const Format::ByteUnit& unit = (userData != nullptr) ? *static_cast<const Format::ByteUnit*>(userData) : Format::byteUnitFor(value);
    // Straight into ImPlot's buffer, keeping one byte for the terminator (#1334).
    if (size > 1)
    {
        if (const std::size_t length = Format::formatBytesWithUnitTo(buff, static_cast<std::size_t>(size - 1), value, unit, perSecond);
            length > 0)
        {
            buff[length] = '\0';
            return static_cast<int>(length);
        }
        buff[0] = '\0'; // A partial label never reaches ImPlot, which reads the buffer up to its NUL
    }
    const std::string str = perSecond ? Format::formatBytesPerSecWithUnit(value, unit) : Format::formatBytesWithUnit(value, unit);
    return Detail::copyAxisLabel(str, buff, size);
}

/// Format values as bytes with appropriate unit scaling (B, KiB, MiB, GiB, TiB)
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytes)
inline int formatAxisBytes(double value, char* buff, int size, void* userData)
{
    return formatAxisBinaryBytes(value, buff, size, userData, false);
}

/// Format values as bytes/s with appropriate unit scaling (B/s, KiB/s, MiB/s, GiB/s, TiB/s)
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytesPerSec)
inline int formatAxisBytesPerSec(double value, char* buff, int size, void* userData)
{
    return formatAxisBinaryBytes(value, buff, size, userData, true);
}

/// Format values as watts: UI::Format::formatWatts ("45.0 W", "500.0 mW")
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisWatts)
inline int formatAxisWatts(double value, char* buff, int size, void* /*userData*/)
{
    // Clamp tiny values to zero to avoid "-0.0 W" display
    if (std::abs(value) < 0.0001)
    {
        value = 0.0;
    }
    return Detail::copyAxisLabel(Format::formatWatts(value), buff, size);
}

/// Format values as percentages: UI::Format::formatPercent ("40%", "0.2%")
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisPercent)
inline int formatAxisPercent(double value, char* buff, int size, void* /*userData*/)
{
    // formatPercent() prints anything under 0.05 % as "0%" (no "-0.0%"), and nothing larger: a
    // percent axis can scale down to 5 % (#1195), where ticks such as 0.2 % must not read 0 % (#1202).
    return Detail::copyAxisLabel(Format::formatPercent(value), buff, size);
}

/// Format a history chart's time axis: "now" at 0, then how long ago in the duration grammar
/// ("30s", "5m", "1m 30s"; Format::formatDuration()) instead of negative seconds (#1202).
/// Use with ImPlot::SetupAxisFormat(ImAxis_X1, formatAxisTimeAgo)
inline int formatAxisTimeAgo(double value, char* buff, int size, void* /*userData*/)
{
    if (std::abs(value) < 0.5)
    {
        return Detail::copyAxisLabel("now", buff, size);
    }
    return Detail::copyAxisLabel(Format::formatDuration(value, Format::DurationStyle::Compact), buff, size);
}

/// Put a history chart's time axis on round steps between xMin and xMax (niceTimeAxisStep()), at
/// most `maxTicks` labels, formatted by formatAxisTimeAgo().
inline void setupTimeAxisTicks(double xMin, double xMax, int maxTicks)
{
    ImPlot::SetupAxisFormat(ImAxis_X1, formatAxisTimeAgo);
    const TimeAxisTicks ticks = timeAxisTicks(xMin, xMax, niceTimeAxisStep(xMax - xMin, maxTicks));
    if (ticks.count >= 2)
    {
        ImPlot::SetupAxisTicks(ImAxis_X1, ticks.first, ticks.last, ticks.count);
    }
}

/// True for the two byte formatters, whose axes step in binary units (niceBinaryAxisStep()).
[[nodiscard]] inline bool isByteAxisFormatter(ImPlotFormatter formatter) noexcept
{
    return formatter == &formatAxisBytes || formatter == &formatAxisBytesPerSec;
}

/// Put a non-negative Y axis's ticks on round 1-2-5 steps in the axis's own unit (#1202): from 0 to
/// `upper` with at most `maxTicks` labels, the step chosen by niceAxisStep(), or for a byte axis by
/// niceBinaryAxisStep() with every label in the one unit the step is in. Without this ImPlot steps
/// in decimal units of the raw value, so a byte axis read 9.5, 7.6, 5.7 MB/s.
inline void setupNiceAxisTicks(ImAxis axis, double upper, ImPlotFormatter formatter, int maxTicks)
{
    if (!std::isfinite(upper) || upper <= 0.0)
    {
        ImPlot::SetupAxisFormat(axis, formatter);
        return;
    }

    double step = 0.0;
    void* userData = nullptr;
    if (isByteAxisFormatter(formatter))
    {
        const Format::ByteUnit& unit = Format::byteUnitFor(upper);
        step = niceBinaryAxisStep(upper, unit.scale, maxTicks);
        userData = byteAxisUserData(unit);
    }
    else
    {
        step = niceAxisStep(upper, maxTicks);
    }
    ImPlot::SetupAxisFormat(axis, formatter, userData);

    const AxisTickRange ticks = axisTickRange(upper, step);
    if (ticks.count >= 2)
    {
        ImPlot::SetupAxisTicks(axis, 0.0, ticks.last, ticks.count);
    }
}

/// One bar of a chart's "now" column. Built every frame, so building one allocates nothing (#1171):
/// valueText is a short formatted value that fits std::string's small-string buffer, label is a view,
/// and tooltipText is held in place.
struct NowBar
{
    std::string valueText;
    /// The series' name, also used to build the fallback tooltip (e.g., "CPU Total"). A view: what it
    /// names -- a constant, or a string the caller keeps -- must outlive the bar.
    std::string_view label;
    /// Rich tooltip text shown on bar hover and in the value strip; falls back to "label: valueText",
    /// then label, then valueText when empty. Leave it empty unless it says more than that fallback
    /// (#1019). Build it with InlineText::format() or tooltipRowText(), not from a std::string.
    InlineText tooltipText;
    double value01 = 0.0;
    ImVec4 color;
    /// The marker of the bar's series on the chart (its SeriesStyle::marker), drawn on the bar's value
    /// strip swatch so the strip keys the series by shape as well as colour (#1198). None for a series
    /// drawn without markers (a chart's filled primary).
    ImPlotMarker marker = ImPlotMarker_None;
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

// TailAlignedSpan / tailAlignedSpan(): UI/TailAlignedSeries.h, exported through this header.

// Returns the tooltip string to display for a NowBar, using the fallback chain:
//   tooltipText (if non-empty) -> "label: valueText" (if both non-empty) -> label -> valueText
// Callers should invoke this only when the bar is actually hovered to avoid per-frame allocations.
[[nodiscard]] inline std::string selectNowBarTooltip(const NowBar& bar)
{
    if (!bar.tooltipText.empty())
    {
        return std::string(bar.tooltipText.view());
    }
    if (!bar.label.empty() && !bar.valueText.empty())
    {
        return formatTooltipRow(bar.label, bar.valueText);
    }
    if (!bar.label.empty())
    {
        return std::string(bar.label);
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

/// Buffers for one frame's time axes, reused from frame to frame (#1018).
///
/// x is "seconds before now", so every history chart rebuilds its time axis every frame; with a new
/// vector each time that was a heap allocation per chart per frame. acquire() hands out the pool's
/// buffers in turn and starts over when the frame number changes, so once each buffer has grown to
/// its chart's length, building the axes allocates nothing. A buffer, and any span of it, stays valid
/// until the same buffer is handed out again in a later frame -- never keep one across frames.
///
/// A burst of charts (a busy frame, a tab with many) does not pin its buffers for good (#1173):
/// buffers no frame has asked for in RELEASE_AFTER_FRAMES frames are freed when a new frame starts.
/// Buffers are handed out in order, so the unused ones are always the last ones.
class TimeAxisPool
{
  public:
    /// Frames a buffer may go unasked-for before it is freed: about ten seconds at 60 fps, long enough
    /// that switching tabs back and forth does not churn the allocator.
    static constexpr int RELEASE_AFTER_FRAMES = 600;

    /// Starts @p frame: buffers handed out from now on belong to it, and those unused for
    /// RELEASE_AFTER_FRAMES are freed. acquire() does this itself on a frame's first request; calling
    /// it every frame as well (trimFrameCaches()) lets buffers age and go while no chart asks for an
    /// axis at all, e.g. on the Processes tab (#1173). Repeat calls within a frame do nothing.
    void beginFrame(int frame)
    {
        if (frame != m_Frame)
        {
            m_Frame = frame;
            m_Next = 0;
            releaseUnused(frame);
        }
    }

    [[nodiscard]] std::vector<double>& acquire(int frame)
    {
        beginFrame(frame);
        if (m_Next == m_Slots.size())
        {
            // Growing the outer vector moves the inner ones, which keeps their heap buffers: spans
            // already handed out this frame stay valid.
            m_Slots.emplace_back();
        }
        Slot& slot = m_Slots[m_Next++];
        slot.lastFrame = frame;
        return slot.buffer;
    }

    [[nodiscard]] std::size_t bufferCount() const noexcept
    {
        return m_Slots.size();
    }

    /// Doubles the pool's buffers can hold without allocating, summed over all of them.
    [[nodiscard]] std::size_t retainedCapacity() const noexcept
    {
        std::size_t total = 0;
        for (const Slot& slot : m_Slots)
        {
            total += slot.buffer.capacity();
        }
        return total;
    }

  private:
    struct Slot
    {
        std::vector<double> buffer;
        int lastFrame = 0; ///< Frame the buffer was last handed out in
    };

    /// Frees the trailing buffers not asked for within RELEASE_AFTER_FRAMES of @p frame. Called at the
    /// start of a frame, before anything is handed out in it, so no span of this frame is affected.
    /// A frame count that went backwards (a new ImGui context) counts as unused, too.
    void releaseUnused(int frame)
    {
        while (!m_Slots.empty())
        {
            const int lastFrame = m_Slots.back().lastFrame;
            if (frame >= lastFrame && (frame - lastFrame) <= RELEASE_AFTER_FRAMES)
            {
                break;
            }
            m_Slots.pop_back();
        }
    }

    std::vector<Slot> m_Slots;
    std::size_t m_Next = 0;
    int m_Frame = -1;
};

/// The time axis for a history chart (see fillTimeAxis()), in a buffer from this frame's
/// TimeAxisPool rather than a new vector. Valid for the rest of the ImGui frame only: never store the
/// span (e.g. in a member) and read it in a later frame, when its buffer may hold another chart's
/// axis or have been freed. Frame-keyed: call it only while a frame is being built, on the UI thread
/// (see "Frame-keyed caches" above, #1181).
///
/// Charts that share timestamps should share one axis: build it once with every timestamp and give
/// each chart tailAlignedSpan(axis, itsCount), rather than one call per chart (#1173).
namespace Detail
{
/// The process-wide pool behind frameTimeAxis() (see "Frame-keyed caches" above).
[[nodiscard]] inline TimeAxisPool& timeAxisPool()
{
    static TimeAxisPool pool;
    return pool;
}
} // namespace Detail

/// Per-frame upkeep of the frame-keyed caches that hold memory between frames: call once per frame,
/// right after ImGui::NewFrame() (UILayer::beginFrame()). Without it, the time-axis pool frees unused
/// buffers only when some chart asks for an axis, so leaving the chart tabs kept them forever (#1173).
inline void trimFrameCaches()
{
    Detail::timeAxisPool().beginFrame(ImGui::GetFrameCount());
}

[[nodiscard]] inline std::span<const double> frameTimeAxis(std::span<const double> timestamps, size_t desiredCount, double nowSeconds)
{
    Detail::assertWithinImGuiFrame();
    auto& buffer = Detail::timeAxisPool().acquire(ImGui::GetFrameCount());
    fillTimeAxis(buffer, timestamps, desiredCount, nowSeconds);
    return buffer;
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

/// The pixel width left for the name in a value-strip entry "<name><suffix>: <value>" on one row of a
/// strip as wide as its chart -- the available width less @p reservedWidth, what the layout takes beside
/// the chart (nowBarsReservedWidth()) -- less the entry's swatch, the suffix and room for a rate value,
/// so an entry built from an uncapped name fits one row. Defined in ChartLegend.cpp.
[[nodiscard]] float seriesNameBudget(std::string_view suffix, float reservedWidth = 0.0F);

/// @p name, cut short with an ellipsis at a UTF-8 code point boundary if @p measure (the pixel width
/// of a string) says it is wider than @p budget: the longest prefix whose "<prefix>…" fits, or just
/// "…" if none does. An OS network adapter's description, of any length, names its series in the
/// value strip, the tooltip and the bars, so it is fitted to the strip's width (#1275).
template<typename Measure> [[nodiscard]] std::string fitSeriesName(std::string_view name, float budget, const Measure& measure)
{
    if (measure(name) <= budget)
    {
        return std::string(name);
    }
    // Called every frame, so the search measures in one buffer rather than a string per probe.
    const auto isBoundary = [&](std::size_t i)
    {
        return i == name.size() || (static_cast<unsigned char>(name[i]) & 0xC0U) != 0x80U;
    };
    // The byte offset of the @p n-th code point boundary after the first code point (n from 0).
    const auto cutAt = [&](std::size_t n)
    {
        std::size_t i = 1;
        for (; !isBoundary(i) || n > 0; ++i)
        {
            if (isBoundary(i))
            {
                --n;
            }
        }
        return i;
    };
    std::size_t cutCount = 0;
    for (std::size_t i = 1; i <= name.size(); ++i)
    {
        cutCount += isBoundary(i) ? 1U : 0U;
    }
    constexpr std::string_view ELLIPSIS = "\u2026";
    std::string candidate;
    candidate.reserve(name.size() + ELLIPSIS.size());
    const auto withEllipsis = [&](std::size_t cut) -> std::string_view
    {
        candidate.assign(name.substr(0, cut)).append(ELLIPSIS);
        return candidate;
    };
    // Widths grow with the prefix: the largest cut that fits, by binary search.
    std::size_t lo = 0;
    std::size_t hi = cutCount;
    while (lo < hi)
    {
        const std::size_t mid = lo + ((hi - lo) / 2);
        if (measure(withEllipsis(cutAt(mid))) <= budget)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo == 0)
    {
        candidate.assign(ELLIPSIS);
    }
    else
    {
        static_cast<void>(withEllipsis(cutAt(lo - 1)));
    }
    return candidate;
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
///   compute theirs from the data and pass it to rateHistoryConfigWithUpper()).
/// yLimits empty → Y axis auto-fits the plotted data, including below zero. No chart does this:
///   plain auto-fit degenerates on an all-zero window into a +/-0.5 sliver, which renders an
///   impossible negative rate and a column of identical tick labels (#920).
struct HistoryChartConfig
{
    const char* id = "";
    double xMin = 0.0;
    double xMax = 0.0;
    ImPlotFormatter yFormatter = formatAxisLocalized;
    std::optional<std::pair<double, double>> yLimits;
    /// The time axis's tick labels ("5m ... now"). Off in a grid of small charts (CPU Cores, the
    /// per-disk grid), where every cell repeated them under the same axis (#1206); its gridlines and
    /// hover tooltip still place a sample in time.
    bool timeAxisLabels = true;
    float height = HISTORY_PLOT_HEIGHT_DEFAULT;
    ImPlotFlags flags = PLOT_FLAGS_DEFAULT;
    /// The generation of the data this chart draws (nextChartDataGeneration()), or 0 if the caller
    /// does not track one. When set, plotLineWithFill() series drawn in the chart keep their reduced
    /// points until it changes instead of reducing their whole history every frame (#1139), so it
    /// must change whenever anything the series are computed from does. See withDataGeneration().
    std::uint64_t dataGeneration = 0;
    /// Where the Y ticks stop, when that is below the axis's top (yLimits->second): a percent axis
    /// with headroom (percentHistoryConfigWithHeadroom()) labels up to 100 % only (#1300).
    std::optional<double> yTicksUpTo;
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

/// Top of a percent axis with headroom: a little above 100, so a series at 100 % -- a fully charged
/// battery -- draws as a line below the plot's top edge rather than along it (#1300). Its ticks still
/// stop at 100 %.
inline constexpr double PERCENT_AXIS_UPPER_WITH_HEADROOM = 104.0;

/// percentHistoryConfig() drawn to PERCENT_AXIS_UPPER_WITH_HEADROOM, ticks up to 100 %. For a series
/// that sits at 100 % for long stretches (battery charge), where the plain 0-100 axis hid it on the
/// top edge. A NowBar beside it is scaled to the same top (normalizeToUnitInterval()), so the bar
/// still meets its line (#1003).
[[nodiscard]] inline HistoryChartConfig percentHistoryConfigWithHeadroom(const char* id, double xMin, double xMax)
{
    HistoryChartConfig cfg = percentHistoryConfig(id, xMin, xMax);
    cfg.yLimits = std::pair{0.0, PERCENT_AXIS_UPPER_WITH_HEADROOM};
    cfg.yTicksUpTo = 100.0;
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
    return cfg;
}

/// The Y upper bound an eased chart draws this frame: its previous frame's bound eased toward
/// `target` (easeAxisUpperBound). Kept per chart, keyed by the chart's ImGui ID. A chart
/// that was not drawn last frame -- just opened, or its tab just shown -- starts at its target rather
/// than easing in from a stale value. Frame-keyed: see "Frame-keyed caches" above (#1181).
[[nodiscard]] inline double easedChartUpperBound(ImGuiID chartId, double target)
{
    Detail::assertWithinImGuiFrame();
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

/// The time axis's flags: without its labels (HistoryChartConfig::timeAxisLabels) it keeps its ticks
/// and gridlines but draws no tick labels (#1206).
[[nodiscard]] constexpr ImPlotAxisFlags historyChartXAxisFlags(bool timeAxisLabels) noexcept
{
    return timeAxisLabels ? X_AXIS_FLAGS_DEFAULT : (X_AXIS_FLAGS_DEFAULT | ImPlotAxisFlags_NoTickLabels);
}

/// The `BeginPlot` flags HistoryChart uses: the configured ones, never with ImPlot's legend. The value
/// strip above every chart is its key -- each series' colour, marker, name and current value -- and a
/// second key inside the chart only took the plot's height (#1198).
[[nodiscard]] constexpr ImPlotFlags historyChartBeginPlotFlags(ImPlotFlags configuredFlags) noexcept
{
    return configuredFlags | ImPlotFlags_NoLegend;
}

/// Set up a right-hand Y2 axis for a series with its own scale -- a rate drawn beside counts, say
/// (#1024) -- from 0 to `upperBound`. Pass easedRateAxisUpperBound() for it, the value its NowBar
/// is scaled to as well. Call right after constructing the HistoryChart, while it is active() and
/// before plotting; then plot that series between ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2) and
/// ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1).
///
/// The axis's tick marks are drawn in @p seriesColor and its tick labels in that colour made readable
/// as text (ColorContrast::readableTint()), the colour of the series on it, and that
/// series' label ends in " →" (pointing at this right-hand axis), so a reader can tell which scale a
/// line is read against (#1206). Its value-strip entry and tooltip rows show the mark after the value
/// (SECONDARY_AXIS_MARK, #1300).
///
/// With @p ticksUpTo the ticks stop there, below the axis's top: a percent axis drawn to
/// PERCENT_AXIS_UPPER_WITH_HEADROOM keeps its last label at 100 % (#1300).
inline void setupSecondaryRateAxis(double upperBound,
                                   ImPlotFormatter formatter,
                                   const ImVec4& seriesColor,
                                   std::optional<double> ticksUpTo = std::nullopt)
{
    // ImPlot reads an axis's colours from the style when the axis is set up (UpdateAxisColors), the
    // tick marks' apart from the labels', so both are pushed. The marks take the series colour as it
    // is; the labels are text, held to 4.5:1 on the frame they sit on, which a series colour (3:1)
    // need not reach, so they take it moved toward the theme's text colour as far as that needs.
    // ImPlot's frame colour, or ImGui's when the theme leaves it on auto (IMPLOT_AUTO_COL).
    const ImVec4 plotFrame = ImPlot::GetStyle().Colors[ImPlotCol_FrameBg];
    const ImVec4 frameColor = (plotFrame.w < 0.0F) ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBg) : plotFrame;
    const ImVec4 frameBg = ColorContrast::flattenOver(frameColor, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    const ImVec4 labelColor =
        ColorContrast::readableTint(seriesColor, Theme::get().scheme().textPrimary, frameBg, ColorContrast::TEXT_CONTRAST_MIN);
    ImPlot::PushStyleColor(ImPlotCol_AxisText, labelColor);
    ImPlot::PushStyleColor(ImPlotCol_AxisTick, seriesColor);
    // AuxDefault: no grid lines of its own, and Opposite, which puts its labels on the right.
    ImPlot::SetupAxis(ImAxis_Y2, nullptr, ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
    ImPlot::PopStyleColor(2);
    ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, upperBound, ImPlotCond_Always);
    // Round ticks like the primary axis, and no more of them (#1202).
    setupNiceAxisTicks(ImAxis_Y2, ticksUpTo.value_or(upperBound), formatter, activeChartDataScope().maxYTicks);
}

/// RAII scope around a stack of history charts drawn one above another in a view (#1206): ImPlot
/// gives every chart begun inside it the same axis padding on each side -- the widest Y tick labels
/// on the left, and on the right the gutter of any chart's second Y axis (setupSecondaryRateAxis())
/// -- so their plot areas share left and right edges and their time axes line up. A chart with a
/// second axis no longer has a shorter time axis than the charts above it. The charts must also
/// reserve the same NowBar column width (renderHistoryWithNowBars()'s minBarColumns).
///
/// Not nestable (ImPlot asserts), and nothing that is not part of the stack should be drawn inside.
class AlignedChartStack
{
  public:
    explicit AlignedChartStack(const char* id) : m_Active(ImPlot::BeginAlignedPlots(id))
    {}

    ~AlignedChartStack()
    {
        if (m_Active)
        {
            ImPlot::EndAlignedPlots();
        }
    }

    AlignedChartStack(const AlignedChartStack&) = delete;
    AlignedChartStack& operator=(const AlignedChartStack&) = delete;
    AlignedChartStack(AlignedChartStack&&) = delete;
    AlignedChartStack& operator=(AlignedChartStack&&) = delete;

  private:
    bool m_Active = false;
};

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
/// and applies the shared axis/format/limit setup so all charts look and behave
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
        Detail::seriesMarkers().clear();
        m_Active = ImPlot::BeginPlot(config.id, ImVec2(-1, config.height), historyChartBeginPlotFlags(config.flags));
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
        const int maxYTicks = axisMaxTicksForHeight(config.height, ImGui::GetTextLineHeight());
        Detail::g_ActiveChartDataScope = ChartDataScope{.generation = config.dataGeneration, .plotId = plotId, .maxYTicks = maxYTicks};
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

        // No x-axis title: the ticks say what they are ("5m ... now", setupTimeAxisTicks()), where
        // "Time (s)" over negative seconds needed one (#1202). Grid cells hide the tick labels (#1206).
        ImPlot::SetupAxes(
            nullptr, nullptr, historyChartXAxisFlags(config.timeAxisLabels), historyChartYAxisFlags(config.yLimits.has_value()));
        if (config.yLimits.has_value())
        {
            const double upper = config.yLimits->second;
            // This chart's bound was eased just before it began (easedRateAxisUpperBound()), and the chart
            // is visible (we're past BeginPlot): make its request here rather than leaving it pending for
            // the next chart or frame.
            if (shouldRequestEaseFrames(Detail::g_PendingEaseRequestFrame, ImGui::GetFrameCount(), true))
            {
                Core::AnimationRequest::request();
            }
            Detail::g_PendingEaseRequestFrame = -1;
            ImPlot::SetupAxisLimits(ImAxis_Y1, config.yLimits->first, upper, ImPlotCond_Always);
            // Round 1-2-5 ticks, at most maxYTicks of them (#1202). Every fixed-limit chart starts
            // at 0 (percent and rate configs); any other lower bound keeps ImPlot's own ticks.
            if (config.yLimits->first == 0.0)
            {
                setupNiceAxisTicks(ImAxis_Y1, std::min(upper, config.yTicksUpTo.value_or(upper)), config.yFormatter, maxYTicks);
            }
            else
            {
                ImPlot::SetupAxisFormat(ImAxis_Y1, config.yFormatter);
            }
        }
        else
        {
            ImPlot::SetupAxisFormat(ImAxis_Y1, config.yFormatter);
        }
        ImPlot::SetupAxisLimits(ImAxis_X1, config.xMin, config.xMax, ImPlotCond_Always);
        setupTimeAxisTicks(config.xMin, config.xMax, timeAxisMaxTicksForWidth(static_cast<float>(plotWidthPx), ImGui::GetFontSize()));
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
/// -- and `tail` in primary text, then for a right-hand-axis series (`rightAxis`) SECONDARY_AXIS_MARK
/// in the series' colour made readable as text, as that axis's labels are (#1300). With `wrap`, an
/// entry that does not fit the row starts a new line; without it the row runs on and the container
/// clips it.
/// The width drawValueStripEntry() gives an entry: swatch, `head` (with its colon), `tail` and the
/// right-axis mark.
[[nodiscard]] inline float valueStripEntryWidth(std::string_view head, std::string_view tail, bool rightAxis = false)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float side = std::floor(ImGui::GetTextLineHeight() * TOOLTIP_SWATCH_LINE_FRACTION);
    const bool addColon = !head.empty() && !head.ends_with(':');
    const float headWidth = head.empty() ? 0.0F
                                         : ImGui::CalcTextSize(head.data(), head.data() + head.size()).x +
                                               (addColon ? ImGui::CalcTextSize(":").x : 0.0F) + style.ItemInnerSpacing.x;
    const float markWidth =
        rightAxis ? ImGui::CalcTextSize(SECONDARY_AXIS_MARK.data(), SECONDARY_AXIS_MARK.data() + SECONDARY_AXIS_MARK.size()).x : 0.0F;
    return side + style.ItemInnerSpacing.x + headWidth + ImGui::CalcTextSize(tail.data(), tail.data() + tail.size()).x + markWidth;
}

inline void drawValueStripEntry(std::string_view head,
                                std::string_view tail,
                                const ImVec4& color,
                                bool first,
                                bool wrap,
                                float rowRight,
                                const ImVec4& muted,
                                ImPlotMarker marker = ImPlotMarker_None,
                                float slotWidth = 0.0F,
                                std::string_view seriesLabel = {},
                                bool rightAxis = false)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float side = std::floor(lineHeight * TOOLTIP_SWATCH_LINE_FRACTION);
    const float inset = std::floor((lineHeight - side) * 0.5F);
    const bool addColon = !head.empty() && !head.ends_with(':');
    const float naturalWidth = valueStripEntryWidth(head, tail, rightAxis);
    const float entryWidth = std::max(naturalWidth, slotWidth);
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
    // The series' marker cut out of the swatch: the shape its line carries on the chart. One the strip
    // does not know yet is cut in once the chart has drawn (drawPendingStripMarkers()).
    const float half = side * 0.5F;
    const ImVec2 swatchCentre(at.x + half, at.y + inset + half);
    const float glyphRadius = std::max(1.0F, half * 0.6F);
    if (marker != ImPlotMarker_None)
    {
        drawMarkerGlyph(*ImGui::GetWindowDrawList(), marker, swatchCentre, glyphRadius, ImGui::GetColorU32(ImGuiCol_WindowBg));
    }
    else if (!seriesLabel.empty())
    {
        pendingStripMarkers().push_back({.label = seriesLabel, .centre = swatchCentre, .radius = glyphRadius});
    }
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
    // The value right-aligned in the entry's slot (settleStripSlot()), so its digits stay put as
    // they change and the entries beside it do not move.
    ImGui::SameLine(0.0F, style.ItemInnerSpacing.x + (entryWidth - naturalWidth));
    ImGui::TextUnformatted(tail.data(), tail.data() + tail.size());
    if (rightAxis)
    {
        // After the value, not the name: it says which axis the value is read on (#1300). Tinted like
        // that axis's tick labels (setupSecondaryRateAxis()), held to text contrast on the window.
        const ImVec4 windowBg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        const ImVec4 markColor = ColorContrast::readableTint(
            withAlpha(color, 1.0F), Theme::get().scheme().textPrimary, windowBg, ColorContrast::TEXT_CONTRAST_MIN);
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, markColor);
        ImGui::TextUnformatted(SECONDARY_AXIS_MARK.data(), SECONDARY_AXIS_MARK.data() + SECONDARY_AXIS_MARK.size());
        ImGui::PopStyleColor();
    }
}
} // namespace Detail

/// How renderNowBarValueStrip() lays out its entries.
enum class ValueStripLayout : std::uint8_t
{
    Wrap,    ///< Each bar's tooltip text; entries that do not fit start a new line
    Compact, ///< One line of "label: valueText": for containers that budget exactly one line (grid
             ///< cells), where a longer tooltip text could run past the edge. The hover keeps it.
};

/// Each series' current value, readable without hovering (#1193), and the chart's only key (#1198):
/// per bar, a swatch in the bar's colour with its series' marker shape on it, and the same text its
/// tooltip shows -- its tooltipText when it has one (richer, e.g. bytes beside a percent), otherwise
/// the tooltip's own fallback "label: valueText" -- with the leading "label:" muted; then any `extras`,
/// series the chart draws without a bar. A bar's marker is its NowBar::marker, else the one its chart
/// records for its label this frame, cut in once the chart has drawn (Detail::drawPendingStripMarkers();
/// renderHistoryWithNowBars() does it after the chart). @p layoutId keys the entries' slots.
///
/// With @p chartReservedRight (what the layout takes beside its chart, nowBarsReservedWidth()), a Wrap
/// strip is placed like a heading's trailing status: on the line of the item just drawn -- the chart's
/// heading -- right-aligned to the chart's right edge (not its NowBars), or on a line of its own still
/// right-aligned when the heading leaves no room (placeTrailingBlock()). Only a strip wider than the
/// chart wraps, left-aligned within the chart's width. Bar strings are already built for the frame.
inline void renderNowBarValueStrip(std::span<const NowBar> bars,
                                   std::span<const ValueStripEntry> extras = {},
                                   ValueStripLayout layout = ValueStripLayout::Wrap,
                                   const char* layoutId = nullptr,
                                   float chartReservedRight = -1.0F)
{
    struct Entry
    {
        std::string_view head;
        std::string_view tail;
        ImVec4 color;
        ImPlotMarker marker = ImPlotMarker_None;
        float slotWidth = 0.0F;
        std::string_view seriesLabel;
        // The short form an entry falls back to when it is wider than the strip's row: a bar's own
        // "label: valueText", where head/tail may hold its richer tooltipText.
        std::string_view compactHead;
        std::string_view compactTail;
        bool rightAxis = false; // Drawn on the chart's right-hand axis: SECONDARY_AXIS_MARK after the value
    };
    static std::vector<Entry> entries; // UI thread only; reused
    entries.clear();
    Detail::pendingStripMarkers().clear(); // a strip not followed by its chart leaves none for another
    const bool wrap = layout == ValueStripLayout::Wrap;
    const ImGuiID slotsKey = (layoutId != nullptr) ? ImGui::GetID(layoutId) : 0;
    for (const NowBar& bar : bars)
    {
        // A right-hand-axis series is named without its " →", which follows its value (#1300).
        const SeriesLabelParts label = splitSecondaryAxisMark(bar.label);
        std::string_view head = label.name;
        std::string_view tail = bar.valueText;
        if (wrap && !bar.tooltipText.empty())
        {
            const std::string_view tip = bar.tooltipText.view();
            const StripTextParts parts = splitStripText(tip, bar.label);
            head = parts.head;
            tail = parts.tail;
        }
        entries.push_back({.head = head,
                           .tail = tail,
                           .color = bar.color,
                           .marker = bar.marker,
                           .seriesLabel = bar.label,
                           .compactHead = label.name,
                           .compactTail = bar.valueText,
                           .rightAxis = label.rightAxis});
    }
    for (const ValueStripEntry& entry : extras)
    {
        const SeriesLabelParts label = splitSecondaryAxisMark(entry.label);
        entries.push_back({.head = label.name,
                           .tail = entry.value,
                           .color = entry.color,
                           .seriesLabel = entry.label,
                           .compactHead = label.name,
                           .compactTail = entry.value,
                           .rightAxis = label.rightAxis});
    }
    if (entries.empty())
    {
        return;
    }

    // The row the strip lays out in: the content width, or with chartReservedRight the chart's.
    const float lineStartX = ImGui::GetCursorPosX();
    const float contentRight = lineStartX + ImGui::GetContentRegionAvail().x;
    const float chartRight = (wrap && chartReservedRight >= 0.0F) ? contentRight - chartReservedRight : contentRight;
    const float rowWidth = std::max(0.0F, chartRight - lineStartX);

    // An entry wider than the row on its own would run past it -- the strip wraps only between
    // entries -- and be clipped, and the strip is the chart's only key. Such an entry shows its short
    // "label: valueText" (the rich text stays in the bar's hover tooltip), and one still too wide has
    // its value cut short with an ellipsis (fitSeriesName()). Rare, so the cut copies are owned here.
    static std::vector<std::string> fittedTails; // UI thread only; reserved so the views below stay valid
    fittedTails.clear();
    fittedTails.reserve(entries.size());
    const auto textWidth = [](std::string_view text)
    {
        return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    };
    for (Entry& entry : entries)
    {
        if (Detail::valueStripEntryWidth(entry.head, entry.tail, entry.rightAxis) <= rowWidth)
        {
            continue;
        }
        entry.head = entry.compactHead;
        entry.tail = entry.compactTail;
        if (const float natural = Detail::valueStripEntryWidth(entry.head, entry.tail, entry.rightAxis); natural > rowWidth)
        {
            const float tailBudget = rowWidth - (natural - textWidth(entry.tail));
            fittedTails.push_back(fitSeriesName(entry.tail, tailBudget, textWidth));
            entry.tail = fittedTails.back();
        }
    }

    // Each entry's slot: its width, held for a while when its value narrows (settleStripSlot()), so
    // the strip -- right-aligned, where every entry moves with the ones after it -- stays still.
    // A strip with no layout to remember slots by uses each entry's own width.
    const double now = ImGui::GetTime();
    std::vector<Detail::StripSlot>* slots = nullptr;
    if (slotsKey != 0)
    {
        auto& byLayout = Detail::stripSlotsByLayout();
        Detail::pruneStaleStripSlots(byLayout, now);
        Detail::StripSlots& layoutSlots = byLayout[slotsKey];
        layoutSlots.lastUsed = now;
        slots = &layoutSlots.slots;
        if (slots->size() != entries.size())
        {
            slots->assign(entries.size(), Detail::StripSlot{});
        }
    }
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const float natural = Detail::valueStripEntryWidth(entries[i].head, entries[i].tail, entries[i].rightAxis);
        // Never wider than the row: a slot held from a wider value must not push the entry past it.
        entries[i].slotWidth =
            std::min(rowWidth,
                     (slots != nullptr) ? Detail::settleStripSlot((*slots)[i], natural, now, Detail::VALUE_STRIP_SLOT_SHRINK_DELAY_SECONDS)
                                        : natural);
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float entryGap = style.ItemSpacing.x * 2.0F;
    float rowRight = contentRight;
    bool rowWraps = wrap;
    if (wrap && chartReservedRight >= 0.0F)
    {
        float stripWidth = 0.0F;
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            stripWidth += entries[i].slotWidth + ((i > 0) ? entryGap : 0.0F);
        }
        if (stripWidth <= chartRight - lineStartX)
        {
            // Window-local X of the heading's right edge: the space SameLine() and SetCursorPosX() take.
            const float headingEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
            const auto placement = UI::LineLayout::placeTrailingBlock(lineStartX, headingEndX, chartRight, stripWidth, entryGap);
            if (placement.sameLine)
            {
                ImGui::SameLine(placement.x);
            }
            else
            {
                ImGui::SetCursorPosX(placement.x);
            }
            rowWraps = false;
        }
        rowRight = chartRight;
    }
    const ImVec4 muted = UI::Theme::get().scheme().textMuted;
    bool first = true;
    for (const Entry& entry : entries)
    {
        Detail::drawValueStripEntry(entry.head,
                                    entry.tail,
                                    entry.color,
                                    first,
                                    rowWraps,
                                    rowRight,
                                    muted,
                                    entry.marker,
                                    entry.slotWidth,
                                    entry.seriesLabel,
                                    entry.rightAxis);
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
/// Frame-keyed: see "Frame-keyed caches" above (#1181).
inline void requestNowBarMotion(ImGuiID barId, double value01, float heightPx)
{
    assertWithinImGuiFrame();
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

/// Width of renderHistoryWithNowBars()'s "Now" column for @p barColumnCount bars, in a row
/// @p availableWidth wide (fittedNowBarWidth(); not positive: uncapped, full-width bars).
[[nodiscard]] inline float nowBarColumnWidth(std::size_t barColumnCount, float availableWidth = -1.0F)
{
    const float count = UI::Format::toFloatNarrow(Domain::Numeric::toDouble(barColumnCount));
    const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
    const float spacing = (barColumnCount > 1) ? itemSpacing * (count - 1.0F) : 0.0F;
    return (fittedNowBarWidth(ImGui::GetFontSize(), barColumnCount, itemSpacing, availableWidth) * count) + spacing;
}

/// Width renderHistoryWithNowBars() takes from the available width beside its chart: the "Now" column
/// for max(@p barCount, @p minBarColumns) bars and, unless @p compactSpacing, the cell padding on each
/// side of the boundary between the two columns. Errs on the wide side: without @p availableWidth
/// (the row renderHistoryWithNowBars() lays out in) it is the uncapped column.
[[nodiscard]] inline float
nowBarsReservedWidth(std::size_t barCount, std::size_t minBarColumns, bool compactSpacing, float availableWidth = -1.0F)
{
    const float cellPadding = compactSpacing ? 0.0F : 2.0F * ImGui::GetStyle().CellPadding.x;
    return nowBarColumnWidth(std::max(barCount, minBarColumns), availableWidth) + cellPadding;
}

/// @p plotFn draws the chart; it is called before this returns, and never stored. A template parameter
/// rather than a std::function: the charts' capturing lambdas are larger than any standard library's
/// std::function small buffer, so wrapping one allocated once per chart per frame (#1171).
template<typename PlotFn>
inline void renderHistoryWithNowBars(const char* tableId,
                                     float plotHeight,
                                     const PlotFn& plotFn,
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

    // The row the chart and its bars share: the bars' column is capped to a share of it, so a narrow
    // pane keeps most of its width for the chart (fittedNowBarWidth(), #1300). Charts stacked in one
    // pane share the row width and minBarColumns, so their columns stay equal.
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const std::size_t barColumns = std::max(bars.size(), minBarColumns);

    if (values == NowBarValues::Strip)
    {
        // stripExtras: series the chart draws without a bar (a peak line), so the strip lists every
        // series its tooltip does. On the heading's line, right-aligned to the chart.
        renderNowBarValueStrip(
            bars, stripExtras, ValueStripLayout::Wrap, tableId, nowBarsReservedWidth(bars.size(), minBarColumns, compactSpacing, rowWidth));
    }

    if (barsOnly)
    {
        // No chart is drawn on this path, so an axis eased for it can't be visible: drop its pending
        // full-rate request rather than leave it for the next chart (Detail::g_PendingEaseRequestFrame).
        // The other paths all construct the chart through plotFn -- the table path inside the table,
        // the clipped-table and bar-less paths directly -- and HistoryChart consumes it there.
        Detail::g_PendingEaseRequestFrame = -1;
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
    const float barWidth = fittedNowBarWidth(ImGui::GetFontSize(), barColumns, style.ItemSpacing.x, rowWidth);
    const float columnWidth = nowBarColumnWidth(barColumns, rowWidth);

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
    // The strip above the chart -- drawn here, or by the caller just before -- gets the shapes of the
    // markers the chart has just recorded, outside the layout table so they draw in the window.
    Detail::drawPendingStripMarkers(Detail::seriesMarkers());

    if (pushedVars > 0)
    {
        ImGui::PopStyleVar(pushedVars);
    }
}

/// renderHistoryWithNowBars() for bars listed in place, e.g. `{readBar, writeBar}`: the list's backing
/// array lives on the stack, where a braced std::vector argument allocated every frame (#1018).
template<typename PlotFn>
inline void renderHistoryWithNowBars(const char* tableId,
                                     float plotHeight,
                                     const PlotFn& plotFn,
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
