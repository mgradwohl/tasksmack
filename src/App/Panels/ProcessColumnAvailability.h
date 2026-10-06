#pragma once

// Which Processes table columns this system's process probe can fill, and what a column's header
// tooltip says about the values it does not have (#1210). Pure (no ImGui calls), so it is
// unit-testable without a live context, following CONTRIBUTING.md's "extract the pure decision
// logic into a small header" pattern.

#include "App/Panels/ProcessRowFormat.h"
#include "App/ProcessColumnConfig.h"
#include "Platform/ProcessTypes.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace App::ProcessColumnAvailability
{

/// What per-process GPU data this system's GPU probe supplies (#1210). Unknown counts as supported,
/// so the GPU columns do not flicker away while the probe is still starting.
struct GpuSupport
{
    /// Per-process GPU memory, devices and engines: a GPU model exists and its probe has per-process
    /// metrics (Platform::GPUCapabilities::hasPerProcessMetrics is false on DRM- or ROCm-only Linux
    /// and with DXGI alone).
    bool perProcess = true;
    /// Per-process GPU utilization as well (hasPerProcessUtilization): NVML's running-process lists
    /// give a process's memory and engines but not its utilization, which then reads 0 everywhere.
    bool utilization = true;

    friend constexpr bool operator==(const GpuSupport&, const GpuSupport&) = default;
};

/// GpuSupport from the GPU model's "known unsupported" flags (Domain::GPUModel::
/// perProcessMetricsKnownUnsupported() / perProcessUtilizationKnownUnsupported()).
[[nodiscard]] constexpr GpuSupport
gpuSupport(bool hasGpuModel, bool perProcessKnownUnsupported, bool perProcessUtilizationKnownUnsupported) noexcept
{
    const bool perProcess = hasGpuModel && !perProcessKnownUnsupported;
    return {.perProcess = perProcess, .utilization = perProcess && !perProcessUtilizationKnownUnsupported};
}

/// GpuSupport for formatting one snapshot generation's cells, from the support published with it
/// (Domain::ProcessModel::GpuSupport): its GPU fields were read under that, which can differ from the
/// GPU model's current state while GPU merges are throttled (#1210). No per-process data means no
/// utilization either.
[[nodiscard]] constexpr GpuSupport gpuSupportOfGeneration(bool perProcess, bool utilization) noexcept
{
    return {.perProcess = perProcess, .utilization = perProcess && utilization};
}

/// Whether the process probe can fill `col` at all on this system. A column it cannot shows
/// ProcessRowFormat::UNAVAILABLE_CELL_TEXT on every row, and its header says why. The GPU columns
/// depend on the GPU probe instead (`gpu`): GPU % on per-process utilization, the others on
/// per-process metrics at all.
///
/// Peak Memory is not gated on ProcessCapabilities::hasPeakRss: where the OS reports no peak, the
/// process model tracks one itself.
[[nodiscard]] constexpr bool isSupported(ProcessColumn col, const Platform::ProcessCapabilities& caps, GpuSupport gpu = {}) noexcept
{
    switch (col)
    {
    case ProcessColumn::GpuPercent:
        return gpu.utilization;
    case ProcessColumn::GpuMemory:
    case ProcessColumn::GpuEngine:
    case ProcessColumn::GpuDevice:
        return gpu.perProcess;
    case ProcessColumn::Shared:
        return caps.hasSharedMemory;
    case ProcessColumn::Power:
        return caps.hasPowerUsage;
    case ProcessColumn::IoRead:
    case ProcessColumn::IoWrite:
        return caps.hasIoCounters;
    case ProcessColumn::NetSent:
    case ProcessColumn::NetReceived:
        return caps.hasNetworkCounters;
    case ProcessColumn::Threads:
        return caps.hasThreadCount;
    case ProcessColumn::Handles:
        return caps.hasHandleCount;
    case ProcessColumn::PageFaults:
        return caps.hasPageFaults;
    case ProcessColumn::Affinity:
        return caps.hasCpuAffinity;
    case ProcessColumn::GdiObjects:
        return caps.hasGdiObjects;
    case ProcessColumn::Status:
        return caps.hasStatus;
    case ProcessColumn::Publisher:
        return caps.hasPublisher;
    case ProcessColumn::Type:
        return caps.hasProcessType;
    default:
        return true;
    }
}

/// The default columns on this system (#1210): getColumnInfo()'s defaults, less any column the probe
/// cannot fill, so no column shown by default is all dashes -- Power without RAPL on Linux, say. It is
/// what "Reset columns" restores. A hidden column can still be shown from the Columns menu, where it
/// says it is not available on this system.
[[nodiscard]] inline ProcessColumnSettings defaultColumns(const Platform::ProcessCapabilities& caps, GpuSupport gpu = {})
{
    ProcessColumnSettings settings;
    for (const ProcessColumn col : allProcessColumns())
    {
        if (!isSupported(col, caps, gpu))
        {
            settings.setDefaultVisible(col, false);
        }
    }
    return settings;
}

/// Gives every column whose visibility was not chosen -- by the user, or in the config file -- this
/// system's default (defaultColumns()). A chosen one is left as it is.
inline void applyCapabilityDefaults(ProcessColumnSettings& settings, const Platform::ProcessCapabilities& caps, GpuSupport gpu = {})
{
    const ProcessColumnSettings defaults = defaultColumns(caps, gpu);
    for (const ProcessColumn col : allProcessColumns())
    {
        settings.setDefaultVisible(col, defaults.isVisible(col));
    }
}

/// The columns after the probe's capabilities change to `caps` (#1210): a probe can withdraw one
/// mid-run (Windows' network counters after the first EStats sample, #1254), or gain one. Every
/// column whose visibility was not chosen takes the new default; a chosen one is left alone. Empty
/// when no column's visibility would change, so the caller queues nothing.
[[nodiscard]] inline std::optional<ProcessColumnSettings>
capabilityDefaultChanges(const ProcessColumnSettings& settings, const Platform::ProcessCapabilities& caps, GpuSupport gpu = {})
{
    ProcessColumnSettings updated = settings;
    applyCapabilityDefaults(updated, caps, gpu);
    if (updated.visible == settings.visible)
    {
        return std::nullopt;
    }
    return updated;
}

/// Whether `settings` shows exactly this system's default columns, i.e. "Reset columns" would change
/// no column's visibility.
[[nodiscard]] inline bool
hasDefaultColumns(const ProcessColumnSettings& settings, const Platform::ProcessCapabilities& caps, GpuSupport gpu = {})
{
    return settings.visible == defaultColumns(caps, gpu).visible;
}

/// What a free-text cell (Status, Publisher) shows.
enum class TextCell : std::uint8_t
{
    Text,        ///< The value
    Blank,       ///< Nothing: the column is filled on this system, and this process has no value
    Unavailable, ///< ProcessRowFormat::UNAVAILABLE_CELL_TEXT: this system cannot fill the column
};

/// A free-text cell's content. An empty value in a supported column is blank, not unavailable: the
/// probe reports no separate "could not read" for these (Windows' publisher lookup returns empty both
/// for an executable without a CompanyName and for one it could not read), so the dash would claim
/// more than is known (#1210).
[[nodiscard]] constexpr TextCell textCell(bool columnSupported, bool hasValue) noexcept
{
    if (!columnSupported)
    {
        return TextCell::Unavailable;
    }
    return hasValue ? TextCell::Text : TextCell::Blank;
}

/// The row formatter's view of the same capabilities.
[[nodiscard]] constexpr ProcessRowFormat::RowFormatOptions rowFormatOptions(const Platform::ProcessCapabilities& caps,
                                                                            GpuSupport gpu = {}) noexcept
{
    ProcessRowFormat::RowFormatOptions options;
    options.hasPowerUsage = caps.hasPowerUsage;
    options.hasSharedMemory = caps.hasSharedMemory;
    options.hasIoCounters = caps.hasIoCounters;
    options.hasNetworkCounters = caps.hasNetworkCounters;
    options.hasThreadCount = caps.hasThreadCount;
    options.hasHandleCount = caps.hasHandleCount;
    options.hasPageFaults = caps.hasPageFaults;
    options.hasCpuAffinity = caps.hasCpuAffinity;
    options.hasGdiObjects = caps.hasGdiObjects;
    options.hasPerProcessGpu = gpu.perProcess;
    options.hasPerProcessGpuUtilization = gpu.utilization;
    options.hasStatus = caps.hasStatus;
    options.hasPublisher = caps.hasPublisher;
    options.hasProcessType = caps.hasProcessType;
    return options;
}

/// Header tooltip line for a column this system cannot fill.
inline constexpr std::string_view UNSUPPORTED_COLUMN_NOTE = "Not available on this system: every row shows \xE2\x80\x94.";

/// Header tooltip line for a column whose value can be unreadable for some processes (#1110).
inline constexpr std::string_view UNREADABLE_VALUES_NOTE =
    "\xE2\x80\x94 means TaskSmack could not read the value for that process, usually for lack of privileges; "
    "0 is a measured zero.";

/// What a column's header tooltip adds about the values it does not have, or empty: that the system
/// cannot fill it at all, or that "—" marks a value unreadable for one process -- distinct from a
/// measured 0 (#1210).
[[nodiscard]] constexpr std::string_view
unavailableValuesNote(ProcessColumn col, const Platform::ProcessCapabilities& caps, GpuSupport gpu = {}) noexcept
{
    if (!isSupported(col, caps, gpu))
    {
        return UNSUPPORTED_COLUMN_NOTE;
    }
    switch (col)
    {
    case ProcessColumn::IoRead:
    case ProcessColumn::IoWrite:
    case ProcessColumn::NetSent:
    case ProcessColumn::NetReceived:
    case ProcessColumn::Handles:
    case ProcessColumn::GdiObjects:
    case ProcessColumn::Threads:
        return UNREADABLE_VALUES_NOTE;
    default:
        return {};
    }
}

} // namespace App::ProcessColumnAvailability
