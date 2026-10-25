#pragma once

// Draws a Processes row's inline meter (#1528) -- a bar, or a sparkline of the process's recent values
// (#1612) -- behind the current table cell's text. See ProcessRowMeter.h for the scaling and the
// settings; this is the ImGui half.

#include "App/Panels/ProcessRowMeter.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessRowHistory.h"
#include "Domain/ProcessSnapshot.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace App::ProcessRowMeter
{

/// The heat gradient's ends, from the theme ([process_meter] low and high).
struct Colors
{
    ImVec4 low;
    ImVec4 high;
};

/// Bars narrower than this many pixels are not drawn: they would not be seen.
inline constexpr float MIN_BAR_WIDTH_PX = 1.0F;

/// A sparkline's stroke, in pixels.
inline constexpr float SPARKLINE_THICKNESS_PX = 1.0F;

/// The heat colour at @p fraction (0-1) of the gradient. Never more opaque than the gradient's ends,
/// which the theme keeps faint enough for the cell's text to stay readable over them.
[[nodiscard]] inline ImU32 heatColor(const Colors& colors, float fraction)
{
    const ImVec4 tint(colors.low.x + ((colors.high.x - colors.low.x) * fraction),
                      colors.low.y + ((colors.high.y - colors.low.y) * fraction),
                      colors.low.z + ((colors.high.z - colors.low.z) * fraction),
                      colors.low.w + ((colors.high.w - colors.low.w) * fraction));
    return ImGui::ColorConvertFloat4ToU32(tint);
}

/// The current cell's meter box: its content box, grown by half the cell padding above and below so
/// the meters of one column read as a continuous strip with a hairline between rows.
struct CellBox
{
    ImVec2 min;
    ImVec2 max;
};

[[nodiscard]] inline CellBox currentCellBox(float width)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float padY = ImGui::GetStyle().CellPadding.y * 0.5F;
    return {.min = ImVec2(pos.x, pos.y - padY), .max = ImVec2(pos.x + width, pos.y + ImGui::GetTextLineHeight() + padY)};
}

/// Draws @p col's bar for @p proc behind the current table cell's content, if @p settings gives it
/// one; call after ImGui::TableSetColumnIndex() and before the cell's text, so the text lands on top.
/// One AddRectFilled at most: no widget, no ID, no allocation. Returns whether it drew a bar.
inline bool renderCellMeter(
    const Settings& settings, ProcessColumn col, const Domain::ProcessSnapshot& proc, const ColumnMaxima& maxima, const Colors& colors)
{
    if (settings.style(col) != Style::Bar)
    {
        return false;
    }
    const float fraction = fractionOf(proc, col, maxima);
    const float width = ImGui::GetContentRegionAvail().x * fraction;
    if (!(width >= MIN_BAR_WIDTH_PX))
    {
        return false;
    }
    const CellBox box = currentCellBox(width);
    ImGui::GetWindowDrawList()->AddRectFilled(box.min, box.max, heatColor(colors, fraction));
    return true;
}

/// The fraction of the cell's height each of @p ring's values reaches, oldest first, into @p out;
/// returns how many. Percent columns are drawn against 0-100%, as their bars; the absolute columns
/// against the ring's own largest value, so a row's line shows its process's trend, not its share
/// of the column (a line at the bottom of every row but the largest says nothing).
[[nodiscard]] inline std::size_t sparklineFractions(ProcessColumn col,
                                                    const Domain::ProcessRowHistory::Ring& ring,
                                                    std::array<float, Domain::ProcessRowHistory::RING_CAPACITY>& out) noexcept
{
    const std::size_t count = std::min(ring.size(), out.size());
    const Scale scale = scaleOf(col);
    double max = 0.0;
    if (scale == Scale::Absolute)
    {
        std::size_t i = 0;
        while (i < count)
        {
            if (const auto value = static_cast<double>(ring.at(i)); std::isfinite(value))
            {
                max = std::max(max, value);
            }
            ++i;
        }
    }
    std::size_t i = 0;
    while (i < count)
    {
        const auto value = static_cast<double>(ring.at(i));
        // A value not read stays NaN: the line breaks there (#2158).
        if (!std::isfinite(value))
        {
            out[i] = std::numeric_limits<float>::quiet_NaN();
        }
        else
        {
            out[i] = (scale == Scale::Percent) ? percentFraction(value) : absoluteFraction(value, max);
        }
        ++i;
    }
    return count;
}

/// Draws @p col's sparkline for row @p row behind the current table cell's content, if @p settings
/// gives it one and @p history keeps that row's ring with two values or more; call where
/// renderCellMeter() is called. The newest value sits at the cell's right edge, one step per
/// refresh to the left, so a process younger than the ring's capacity has a shorter line. Its
/// colour is the bar's heat colour at the newest value. One AddPolyline of at most RING_CAPACITY
/// points from a stack buffer: no widget, no ID, no allocation. Returns whether it drew a line.
inline bool renderCellSparkline(const Settings& settings,
                                ProcessColumn col,
                                const Domain::ProcessRowHistory::Generation* history,
                                std::size_t row,
                                const Colors& colors)
{
    if (history == nullptr || settings.style(col) != Style::Sparkline)
    {
        return false;
    }
    const auto metric = metricOf(col);
    const Domain::ProcessRowHistory::Ring* ring = metric.has_value() ? history->ring(row, *metric) : nullptr;
    if (ring == nullptr || ring->size() < 2)
    {
        return false;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    if (!(width >= MIN_BAR_WIDTH_PX))
    {
        return false;
    }

    std::array<float, Domain::ProcessRowHistory::RING_CAPACITY> fractions{};
    const std::size_t count = sparklineFractions(col, *ring, fractions);
    const CellBox box = currentCellBox(width);
    // Inset by half the stroke, so a line at 0 or at full scale is not cut by the row's edge.
    const float inset = SPARKLINE_THICKNESS_PX * 0.5F;
    const float bottom = box.max.y - inset;
    const float height = std::max(0.0F, (box.max.y - box.min.y) - SPARKLINE_THICKNESS_PX);
    const float step = width / static_cast<float>(Domain::ProcessRowHistory::RING_CAPACITY - 1U);

    // The colour is the newest value read; with none read, there is nothing to draw.
    std::size_t newest = count;
    while (newest > 0 && std::isnan(fractions[newest - 1U]))
    {
        --newest;
    }
    if (newest == 0)
    {
        return false;
    }
    const ImU32 color = heatColor(colors, fractions[newest - 1U]);

    // One polyline per run of values read: a value not read (NaN) leaves a gap (#2158).
    std::array<ImVec2, Domain::ProcessRowHistory::RING_CAPACITY> points{};
    bool drew = false;
    std::size_t i = 0;
    while (i < count)
    {
        std::size_t run = 0;
        while (i < count && !std::isnan(fractions[i]))
        {
            const auto stepsBack = static_cast<float>(count - 1U - i);
            points[run] = ImVec2(box.max.x - (stepsBack * step), bottom - (fractions[i] * height));
            ++run;
            ++i;
        }
        if (run >= 2)
        {
            ImGui::GetWindowDrawList()->AddPolyline(points.data(), static_cast<int>(run), color, ImDrawFlags_None, SPARKLINE_THICKNESS_PX);
            drew = true;
        }
        ++i; // past the gap
    }
    return drew;
}

} // namespace App::ProcessRowMeter
