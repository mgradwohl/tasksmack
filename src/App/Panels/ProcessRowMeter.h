#pragma once

// Inline meters in the Processes table's rows (#1528): a bar behind a resource column's text,
// proportional to the value, in a heat colour that deepens with it, as Task Manager shades its cells.
//
// A column's meter can instead be a sparkline of the process's recent values (#1612), drawn from the
// rings Domain::ProcessRowHistory keeps while any column shows one.
//
// This header is the ImGui-free part, unit-tested on its own: which columns can carry a meter, how a
// value becomes a fraction of the cell, the per-column styles saved in config.toml, and the column
// maxima the absolute columns are scaled against. ProcessRowMeterView.h draws the bar and the line.

#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessRowHistory.h"
#include "Domain/ProcessSnapshot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace App::ProcessRowMeter
{

/// How a column's value is turned into a fraction of its cell.
enum class Scale : std::uint8_t
{
    None,     ///< No meter
    Percent,  ///< 0-100%, clamped
    Absolute, ///< Bytes or bytes/s, against the column's largest value in the snapshot
};

/// The scale of @p col's meter, or Scale::None for a column without one.
[[nodiscard]] constexpr Scale scaleOf(ProcessColumn col) noexcept
{
    switch (col)
    {
    case ProcessColumn::CpuPercent:
    case ProcessColumn::MemPercent:
    case ProcessColumn::GpuPercent:
        return Scale::Percent;
    case ProcessColumn::Resident:
    case ProcessColumn::IoRead:
    case ProcessColumn::IoWrite:
    case ProcessColumn::NetSent:
    case ProcessColumn::NetReceived:
    case ProcessColumn::GpuMemory:
        return Scale::Absolute;
    default:
        return Scale::None;
    }
}

/// The columns that can carry a meter, in the Columns menu's order: CPU, memory, disk, network, GPU.
inline constexpr auto METER_COLUMNS = std::to_array<ProcessColumn>({
    ProcessColumn::CpuPercent,
    ProcessColumn::MemPercent,
    ProcessColumn::Resident,
    ProcessColumn::IoRead,
    ProcessColumn::IoWrite,
    ProcessColumn::NetSent,
    ProcessColumn::NetReceived,
    ProcessColumn::GpuPercent,
    ProcessColumn::GpuMemory,
});

/// Whether @p col's meter is on by default: CPU % only. It is the column people scan for a busy
/// process, and its 0-100% scale reads the same on every machine; the absolute columns are scaled
/// against whatever the largest process happens to be, which is noisier, so they are opt-in.
[[nodiscard]] constexpr bool defaultOn(ProcessColumn col) noexcept
{
    return col == ProcessColumn::CpuPercent;
}

/// @p percent (0-100) as a fraction of the cell, clamped to [0, 1]; 0 for anything not finite.
[[nodiscard]] inline float percentFraction(double percent) noexcept
{
    if (!std::isfinite(percent) || percent <= 0.0)
    {
        return 0.0F;
    }
    return static_cast<float>(std::min(percent / 100.0, 1.0));
}

/// @p value against @p max as a fraction of the cell, clamped to [0, 1]; 0 when either is not
/// finite or not positive, so a column whose largest value is 0 draws no bars.
[[nodiscard]] inline float absoluteFraction(double value, double max) noexcept
{
    if (!std::isfinite(value) || !std::isfinite(max) || value <= 0.0 || max <= 0.0)
    {
        return 0.0F;
    }
    return static_cast<float>(std::min(value / max, 1.0));
}

/// The per-process history metric that follows @p col, or none for a column without a meter.
[[nodiscard]] constexpr std::optional<Domain::ProcessRowHistory::Metric> metricOf(ProcessColumn col) noexcept
{
    using Domain::ProcessRowHistory::Metric;
    switch (col)
    {
    case ProcessColumn::CpuPercent:
        return Metric::CpuPercent;
    case ProcessColumn::MemPercent:
        return Metric::MemPercent;
    case ProcessColumn::Resident:
        return Metric::Resident;
    case ProcessColumn::IoRead:
        return Metric::IoRead;
    case ProcessColumn::IoWrite:
        return Metric::IoWrite;
    case ProcessColumn::NetSent:
        return Metric::NetSent;
    case ProcessColumn::NetReceived:
        return Metric::NetReceived;
    case ProcessColumn::GpuPercent:
        return Metric::GpuPercent;
    case ProcessColumn::GpuMemory:
        return Metric::GpuMemory;
    default:
        return std::nullopt;
    }
}

/// The value @p col shows for @p proc, as the meter scales it; 0 for a column without a meter or a
/// value the probe could not read (the cell then shows a dash, and gets no bar).
[[nodiscard]] inline double valueOf(const Domain::ProcessSnapshot& proc, ProcessColumn col) noexcept
{
    const auto metric = metricOf(col);
    return metric.has_value() ? Domain::ProcessRowHistory::valueOf(proc, *metric) : 0.0;
}

/// What a meter column draws behind its text: nothing, a bar (#1528) or a sparkline (#1612).
enum class Style : std::uint8_t
{
    Off,
    Bar,
    Sparkline,
};

/// @p style as [process_meters] saves it.
[[nodiscard]] constexpr std::string_view styleKey(Style style) noexcept
{
    switch (style)
    {
    case Style::Bar:
        return "bar";
    case Style::Sparkline:
        return "sparkline";
    case Style::Off:
        break;
    }
    return "off";
}

/// The style a [process_meters] string names, or none for anything else.
[[nodiscard]] constexpr std::optional<Style> parseStyle(std::string_view key) noexcept
{
    for (const Style style : {Style::Off, Style::Bar, Style::Sparkline})
    {
        if (key == styleKey(style))
        {
            return style;
        }
    }
    return std::nullopt;
}

/// The style a [process_meters] boolean, as saved before sparklines (#1528), stands for.
[[nodiscard]] constexpr Style styleFromLegacyBool(bool on) noexcept
{
    return on ? Style::Bar : Style::Off;
}

/// What each column draws behind its text, saved as [process_meters] in config.toml.
struct Settings
{
    std::array<Style, processColumnCount()> styles{};

    Settings() noexcept
    {
        for (const ProcessColumn col : METER_COLUMNS)
        {
            styles[toIndex(col)] = defaultOn(col) ? Style::Bar : Style::Off;
        }
    }

    /// What @p col draws; always Style::Off for a column that cannot have a meter.
    [[nodiscard]] Style style(ProcessColumn col) const noexcept
    {
        return styles[toIndex(col)];
    }

    /// Sets what @p col draws; ignored for a column that cannot have a meter.
    void setStyle(ProcessColumn col, Style style) noexcept
    {
        if (scaleOf(col) != Scale::None)
        {
            styles[toIndex(col)] = style;
        }
    }

    /// Whether @p col draws anything, a bar or a sparkline.
    [[nodiscard]] bool isOn(ProcessColumn col) const noexcept
    {
        return style(col) != Style::Off;
    }

    /// Turns @p col's bar on or off, as the [process_meters] booleans did; ignored for a column that
    /// cannot have a meter.
    void set(ProcessColumn col, bool enabled) noexcept
    {
        setStyle(col, styleFromLegacyBool(enabled));
    }

    /// Whether any column draws anything, so a frame with none skips the per-cell work.
    [[nodiscard]] bool anyOn() const noexcept
    {
        return std::ranges::any_of(styles, [](Style style) { return style != Style::Off; });
    }

    /// The histories the sparkline columns need kept (Domain::ProcessModel::setRowHistoryMetrics()):
    /// none, and so no history at all, while no column shows a sparkline.
    [[nodiscard]] Domain::ProcessRowHistory::MetricSet sparklineMetrics() const noexcept
    {
        Domain::ProcessRowHistory::MetricSet metrics = 0;
        for (const ProcessColumn col : METER_COLUMNS)
        {
            if (const auto metric = metricOf(col); metric.has_value() && style(col) == Style::Sparkline)
            {
                metrics = static_cast<Domain::ProcessRowHistory::MetricSet>(metrics | Domain::ProcessRowHistory::bitOf(*metric));
            }
        }
        return metrics;
    }

    friend bool operator==(const Settings&, const Settings&) = default;
};

/// The largest value of each absolute column in one snapshot generation, which its meters are scaled
/// against. Rebuilt once per generation (O(processes x 6)), not per frame.
struct ColumnMaxima
{
    std::array<double, processColumnCount()> max{};

    void rebuild(const std::vector<Domain::ProcessSnapshot>& snapshots) noexcept
    {
        max.fill(0.0);
        for (const Domain::ProcessSnapshot& proc : snapshots)
        {
            for (const ProcessColumn col : METER_COLUMNS)
            {
                if (scaleOf(col) == Scale::Absolute)
                {
                    const double value = valueOf(proc, col);
                    if (std::isfinite(value))
                    {
                        max[toIndex(col)] = std::max(max[toIndex(col)], value);
                    }
                }
            }
        }
    }
};

/// The fraction of @p col's cell the meter fills for @p proc: 0 for a column without a meter.
[[nodiscard]] inline float fractionOf(const Domain::ProcessSnapshot& proc, ProcessColumn col, const ColumnMaxima& maxima) noexcept
{
    switch (scaleOf(col))
    {
    case Scale::Percent:
        return percentFraction(valueOf(proc, col));
    case Scale::Absolute:
        return absoluteFraction(valueOf(proc, col), maxima.max[toIndex(col)]);
    case Scale::None:
        break;
    }
    return 0.0F;
}

} // namespace App::ProcessRowMeter
