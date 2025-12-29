#pragma once

#include "UI/Numeric.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <string>
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
    const double baseIntervalMs = UI::Numeric::toDouble(refreshInterval.count());
    const double tauMs = std::clamp(baseIntervalMs * SMOOTH_FACTOR, TAU_MS_MIN, TAU_MS_MAX);
    const double dtMs = (deltaTimeSeconds > 0.0) ? deltaTimeSeconds * 1000.0 : baseIntervalMs;
    return std::clamp(1.0 - std::exp(-dtMs / std::max(1.0, tauMs)), 0.0, 1.0);
}

inline double computeAlpha(float deltaTimeSeconds, std::chrono::milliseconds refreshInterval)
{
    return computeAlpha(UI::Numeric::toDouble(deltaTimeSeconds), refreshInterval); // Explicit: float seconds -> double smoothing math
}

inline double smoothTowards(double current, double target, double alpha)
{
    return current + (alpha * (target - current));
}

inline std::string formatAgeSeconds(double relativeSeconds)
{
    const double ageSeconds = std::abs(relativeSeconds);
    return std::format("Age: {:.1f}s", ageSeconds);
}

template<typename TX, typename TY>
inline void plotLineWithFill(const char* label,
                             const TX* xData,
                             const TY* yData,
                             int count,
                             const ImVec4& lineColor,
                             std::optional<ImVec4> fillColor = std::nullopt,
                             float lineThickness = 2.0F)
{
    if (count <= 0)
    {
        return;
    }

    const ImVec4 fill = fillColor.value_or(ImVec4{lineColor.x, lineColor.y, lineColor.z, lineColor.w * 0.35F});
    const std::string shadedLabel = std::format("##{}Fill", label);

    ImPlot::SetNextFillStyle(fill);
    ImPlot::PlotShaded(shadedLabel.c_str(), xData, yData, count, 0.0);

    ImPlot::SetNextLineStyle(lineColor, lineThickness);
    ImPlot::PlotLine(label, xData, yData, count);
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
    double value01 = 0.0;
    ImVec4 color;
};

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
        timeData[i] = UI::Numeric::toFloatNarrow(timestamps[offset + i] - nowSeconds);
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

    const float x = UI::Numeric::toFloatNarrow(mouseX);
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

            drawVerticalBarWithValue(
                "##NowBar", bars[i].value01, bars[i].color, plotHeight, widthPerBar, "", "", bars[i].valueText.c_str());
            ImGui::PopID();
        }
        ImGui::EndGroup();
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const size_t barColumnCount = std::max(bars.size(), minBarColumns);
    const float barColumnCountF = UI::Numeric::toFloatNarrow(UI::Numeric::toDouble(barColumnCount));
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
            drawVerticalBarWithValue(
                "##NowBar", bars[i].value01, bars[i].color, plotHeight, widthPerBar, "", "", bars[i].valueText.c_str());
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
