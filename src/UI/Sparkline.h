#pragma once

// A row-height history line with no axes, legend or frame (#1522): the live Sensors section's per-sensor
// chart. It is drawn with the history charts' own series code (plotLineWithFill(): min/max reduction to
// the plot's width, NaN as a gap, the last reading held to now) on the same "seconds before now" time
// axis, so it moves with every other chart; only the decorations are left out.

#include "UI/ChartWidgets.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace UI::Widgets
{

/// The Y range a sparkline draws @p values in: their finite extremes with a little headroom either
/// side, or a band of 1 around a flat line; nullopt when no value is finite.
[[nodiscard]] inline std::optional<std::pair<double, double>> sparklineYRange(std::span<const float> values) noexcept
{
    double low = std::numeric_limits<double>::infinity();
    double high = -std::numeric_limits<double>::infinity();
    for (const float value : values)
    {
        if (std::isfinite(value))
        {
            low = std::min(low, static_cast<double>(value));
            high = std::max(high, static_cast<double>(value));
        }
    }
    if (low > high)
    {
        return std::nullopt;
    }
    const double pad = (high - low > 0.0) ? (high - low) * 0.1 : 1.0;
    return std::pair{low - pad, high + pad};
}

/// Draws @p values, aligned with @p timestamps (steady_clock seconds, as the models stamp them), over the
/// last @p windowSeconds into a @p size box. Takes the box whatever it draws, so rows stay aligned; an
/// empty or all-NaN series leaves it blank. Call while a frame is being built, on the UI thread.
inline void renderSparkline(const char* id,
                            std::span<const double> timestamps,
                            std::span<const float> values,
                            double windowSeconds,
                            ImVec2 size,
                            const ImVec4& color)
{
    const std::size_t count = std::min(timestamps.size(), values.size());
    const std::optional<std::pair<double, double>> yRange = sparklineYRange(values.first(count));
    if (count == 0 || !yRange.has_value())
    {
        ImGui::Dummy(size);
        return;
    }
    // The newest `count` of each, so a series and its axis line up even if one is longer.
    const std::span<const double> x = frameTimeAxis(timestamps.last(count), count, historyFrameNowSeconds());
    const std::span<const float> y = values.last(count);

    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0.0F, 0.0F));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, 0.0F);
    if (ImPlot::BeginPlot(id, size, ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs | ImPlotFlags_NoFrame))
    {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxisLimits(ImAxis_X1, -windowSeconds, 0.0, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, yRange->first, yRange->second, ImPlotCond_Always);
        plotLineWithFill("##sparkline", x.data(), y.data(), static_cast<int>(x.size()), color, std::nullopt, 1.5F);
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar(2);
}

} // namespace UI::Widgets
