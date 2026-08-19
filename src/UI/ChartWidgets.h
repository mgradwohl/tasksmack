#pragma once

#include "Domain/Numeric.h"
#include "UI/Format.h"
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
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace UI::Widgets
{

inline constexpr ImPlotFlags PLOT_FLAGS_DEFAULT = ImPlotFlags_NoMenus;
inline constexpr ImPlotAxisFlags X_AXIS_FLAGS_DEFAULT = ImPlotAxisFlags_NoHighlight;
inline constexpr ImPlotAxisFlags Y_AXIS_FLAGS_DEFAULT = ImPlotAxisFlags_NoHighlight;
inline constexpr float HISTORY_PLOT_HEIGHT_DEFAULT = 180.0F;
inline constexpr float BAR_WIDTH = 24.0F;
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

/// Compute dynamic max for NowBar and Y-axis scaling.
/// Returns max of all values in history plus current, with a minimum floor of 1.0.
/// Used to keep NowBar height consistent with chart Y-axis.
[[nodiscard]] inline double seriesMax(const std::vector<double>& values, double current)
{
    if (values.empty())
    {
        return std::max(current, 1.0);
    }
    const auto it = std::ranges::max_element(values);
    const double historyMax = (it != values.end()) ? *it : 1.0;
    return std::max({historyMax, current, 1.0});
}

template<typename TX, typename TY>
inline void plotLineWithFill(const char* label,
                             const TX* xData,
                             const TY* yData,
                             int count,
                             const ImVec4& lineColor,
                             std::optional<ImVec4> fillColor = std::nullopt,
                             float lineThickness = 2.0F,
                             bool drawFill = true,
                             int maxPointCount = 0)
{
    if (count <= 0)
    {
        return;
    }

    const auto renderSeries = [&](const TX* plotXData, const TY* plotYData, int plotCount)
    {
        if (drawFill)
        {
            const ImVec4 fill = fillColor.value_or(ImVec4{lineColor.x, lineColor.y, lineColor.z, lineColor.w * 0.35F});
            // Render fill with same label as line so ImPlot treats them as one series.
            // When user clicks legend to hide the series, both fill and line hide together.
            // Render fill first so line appears on top.
            ImPlot::PlotShaded(label, plotXData, plotYData, plotCount, 0.0, {ImPlotProp_FillColor, fill});
        }

        ImPlot::PlotLine(label, plotXData, plotYData, plotCount, {ImPlotProp_LineColor, lineColor, ImPlotProp_LineWeight, lineThickness});
    };

    // Clamp effective max so stride math and buffer capacity stay in sync.
    const int effectiveMax = (maxPointCount > 1) ? std::min(maxPointCount, static_cast<int>(LINE_PLOT_MAX_POINTS_DENSE)) : maxPointCount;
    if ((effectiveMax > 1) && (count > effectiveMax))
    {
        std::array<TX, LINE_PLOT_MAX_POINTS_DENSE> reducedXData;
        std::array<TY, LINE_PLOT_MAX_POINTS_DENSE> reducedYData;
        for (int resultIdx = 0; resultIdx < effectiveMax; ++resultIdx)
        {
            const std::size_t numerator = static_cast<std::size_t>(resultIdx) * static_cast<std::size_t>(count - 1);
            const std::size_t denominator = static_cast<std::size_t>(effectiveMax - 1);
            const int sourceIdx = static_cast<int>(numerator / denominator);

            reducedXData[static_cast<std::size_t>(resultIdx)] = xData[sourceIdx];
            reducedYData[static_cast<std::size_t>(resultIdx)] = yData[sourceIdx];
        }

        renderSeries(reducedXData.data(), reducedYData.data(), effectiveMax);
        return;
    }

    renderSeries(xData, yData, count);
}

/// Helper for line-only rendering with stride downsampling to LINE_PLOT_MAX_POINTS_DENSE points.
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

/// Format values as bytes/s with appropriate unit scaling (B/s, KB/s, MB/s, GB/s)
/// Use with ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytesPerSec)
inline int formatAxisBytesPerSec(double value, char* buff, int size, void* /*userData*/)
{
    // Clamp tiny values to zero to avoid "-0B/s" display
    if (std::abs(value) < 0.5)
    {
        value = 0.0;
    }

    const double absValue = std::abs(value);
    std::string str;

    if (absValue >= 1024.0 * 1024.0 * 1024.0)
    {
        str = std::format("{:.1f}GB/s", value / (1024.0 * 1024.0 * 1024.0));
    }
    else if (absValue >= 1024.0 * 1024.0)
    {
        str = std::format("{:.1f}MB/s", value / (1024.0 * 1024.0));
    }
    else if (absValue >= 1024.0)
    {
        str = std::format("{:.1f}KB/s", value / 1024.0);
    }
    else
    {
        str = std::format("{:.1f}B/s", value);
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
    std::string tooltipText; // Rich tooltip text shown on bar hover (e.g., "CPU Total: 45%");
                             // falls back to "label: valueText", then label, then valueText when empty
    double value01 = 0.0;
    ImVec4 color;
};

[[nodiscard]] inline double normalizeToUnitInterval(double value, double maxValue)
{
    if (maxValue <= 0.0)
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

template<typename T> [[nodiscard]] inline TailAlignedSpan<T> tailAlignedSpan(const std::vector<T>& data, std::size_t count)
{
    const std::size_t clampedCount = std::min(count, data.size());
    const std::size_t offset = data.size() - clampedCount;
    if (clampedCount == 0)
    {
        return {std::span<const T>{}, offset};
    }
    return {std::span<const T>(data.data() + offset, clampedCount), offset};
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

inline TimeAxisConfig makeTimeAxisConfig(const std::vector<double>& timestamps, double maxHistorySeconds, double desiredOffsetSeconds)
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

inline std::vector<float> buildTimeAxis(const std::vector<double>& timestamps, size_t desiredCount, double nowSeconds)
{
    const size_t n = std::min(desiredCount, timestamps.size());
    std::vector<float> timeData(n);
    const size_t offset = timestamps.size() - n;
    if (n == 0)
    {
        return timeData;
    }
    for (size_t i = 0; i < n; ++i)
    {
        timeData[i] = UI::Format::toFloatNarrow(timestamps[offset + i] - nowSeconds);
    }
    return timeData;
}

inline std::vector<double> buildTimeAxisDoubles(const std::vector<double>& timestamps, size_t desiredCount, double nowSeconds)
{
    const size_t n = std::min(desiredCount, timestamps.size());
    std::vector<double> timeData(n);
    const size_t offset = timestamps.size() - n;
    if (n == 0)
    {
        return timeData;
    }
    for (size_t i = 0; i < n; ++i)
    {
        timeData[i] = timestamps[offset + i] - nowSeconds;
    }
    return timeData;
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
    const size_t upperIdx = static_cast<size_t>(upperDist); // Safe: checked by std::in_range
    const size_t lowerIdx = upperIdx - 1;

    const float distLower = std::abs(timeData[lowerIdx] - x);
    const float distUpper = std::abs(timeData[upperIdx] - x);

    return (distUpper < distLower) ? upperIdx : lowerIdx;
}

inline auto hoveredIndexFromPlotX(const std::vector<double>& timeData, double mouseX) -> std::optional<size_t>
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
    const size_t upperIdx = static_cast<size_t>(upperDist); // Safe: checked by std::in_range
    const size_t lowerIdx = upperIdx - 1;

    const double distLower = std::abs(timeData[lowerIdx] - mouseX);
    const double distUpper = std::abs(timeData[upperIdx] - mouseX);

    return (distUpper < distLower) ? upperIdx : lowerIdx;
}

inline void setupLegendDefault()
{
    ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_NoHighlightItem);
}

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
        const float widthPerBar = 24.0F;
        const ImGuiStyle& style = ImGui::GetStyle();

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
    const float columnWidth = (BAR_WIDTH * barColumnCountF) + spacing;

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

        const float widthPerBar = BAR_WIDTH;

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

// ============================================================================
// Chart Data Utilities
// ============================================================================

/// Crop the front of a vector to reach a target size.
/// Used to align history buffers when they have different lengths for charting.
/// @param data The vector to crop (modified in place)
/// @param targetSize The desired size after cropping
template<typename T> void cropFrontToSize(std::vector<T>& data, std::size_t targetSize)
{
    if (data.size() > targetSize)
    {
        const std::size_t removeCount = data.size() - targetSize;
        using Diff = std::vector<T>::difference_type;
        constexpr Diff maxDiff = std::numeric_limits<Diff>::max();
        const Diff removeCountDiff = Domain::Numeric::narrowOr<Diff>(removeCount, maxDiff);
        if (removeCountDiff == maxDiff)
        {
            data.clear();
            return;
        }

        data.erase(data.begin(), data.begin() + removeCountDiff);
    }
}

} // namespace UI::Widgets
