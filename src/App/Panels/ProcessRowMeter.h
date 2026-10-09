#pragma once

// Inline meters in the Processes table's rows (#1528): a bar behind a resource column's text,
// proportional to the value, in a heat colour that deepens with it, as Task Manager shades its cells.
//
// This header is the ImGui-free part, unit-tested on its own: which columns can carry a meter, how a
// value becomes a fraction of the cell, the per-column toggles saved in config.toml, and the column
// maxima the absolute columns are scaled against. ProcessRowMeterView.h draws the bar.

#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

/// The value @p col shows for @p proc, as the meter scales it; 0 for a column without a meter or a
/// value the probe could not read (the cell then shows a dash, and gets no bar).
[[nodiscard]] inline double valueOf(const Domain::ProcessSnapshot& proc, ProcessColumn col) noexcept
{
    switch (col)
    {
    case ProcessColumn::CpuPercent:
        return proc.cpuPercent;
    case ProcessColumn::MemPercent:
        return proc.memoryPercent;
    case ProcessColumn::Resident:
        return static_cast<double>(proc.memoryBytes);
    case ProcessColumn::IoRead:
        return proc.ioAvailable ? proc.ioReadBytesPerSec : 0.0;
    case ProcessColumn::IoWrite:
        return proc.ioAvailable ? proc.ioWriteBytesPerSec : 0.0;
    case ProcessColumn::NetSent:
        return proc.networkAvailable ? proc.netSentBytesPerSec : 0.0;
    case ProcessColumn::NetReceived:
        return proc.networkAvailable ? proc.netReceivedBytesPerSec : 0.0;
    case ProcessColumn::GpuPercent:
        return proc.gpuFieldsRead ? proc.gpuUtilPercent : 0.0;
    case ProcessColumn::GpuMemory:
        return proc.gpuFieldsRead ? static_cast<double>(proc.gpuMemoryBytes) : 0.0;
    default:
        return 0.0;
    }
}

/// Which columns show a meter, saved as [process_meters] in config.toml.
struct Settings
{
    std::array<bool, processColumnCount()> on{};

    Settings() noexcept
    {
        for (const ProcessColumn col : METER_COLUMNS)
        {
            on[toIndex(col)] = defaultOn(col);
        }
    }

    /// Whether @p col shows a meter; always false for a column that cannot have one.
    [[nodiscard]] bool isOn(ProcessColumn col) const noexcept
    {
        return on[toIndex(col)];
    }

    /// Turns @p col's meter on or off; ignored for a column that cannot have one.
    void set(ProcessColumn col, bool enabled) noexcept
    {
        if (scaleOf(col) != Scale::None)
        {
            on[toIndex(col)] = enabled;
        }
    }

    /// Whether any column shows a meter, so a frame with none skips the per-cell work.
    [[nodiscard]] bool anyOn() const noexcept
    {
        return std::ranges::any_of(on, [](bool enabled) { return enabled; });
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
