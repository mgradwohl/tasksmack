#pragma once

// Which Processes table columns this system's process probe can fill, and what a column's header
// tooltip says about the values it does not have (#1210). Pure (no ImGui calls), so it is
// unit-testable without a live context, following CONTRIBUTING.md's "extract the pure decision
// logic into a small header" pattern.

#include "App/Panels/ProcessRowFormat.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessModel.h"
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
    /// metrics (Platform::GPUCapabilities::hasPerProcessMetrics; false with DXGI alone, and on Linux
    /// for a GPU whose driver reports no DRM clients, #2589).
    bool perProcess = true;
    /// Per-process GPU utilization as well (hasPerProcessUtilization): without it a process's memory
    /// and engines are known but not its utilization (NVML without nvmlDeviceGetProcessUtilization),
    /// which then reads 0 everywhere.
    bool utilization = true;
    /// Supported, but reading the per-process GPU data failed for the generation being drawn: its GPU
    /// cells are unreadable for that sample, not "not available on this system" (#1210).
    bool readFailed = false;
    /// Per-process NPU / compute-accelerator data (Platform::perProcessNpuMetricsSupported(), #1827),
    /// apart from the GPU's: on Linux an NPU's per-process memory comes from its own fdinfo while the
    /// iGPU beside it may have none, so neither may switch on the other's columns (#1210).
    bool npuPerProcess = true;
    bool npuUtilization = true; ///< ...and per-process NPU utilization among it
    /// An NPU or other compute accelerator is listed (Domain::GPUPublication::acceleratorInfo, #1799):
    /// the NPU columns need one as well as per-process NPU data.
    bool accelerator = true;
    /// An NPU or compute accelerator is known to be listed: the GPU model has published its devices and
    /// one is an accelerator. Unlike `accelerator`, unknown counts as none, so NPU % -- shown by default
    /// only with one (defaultVisible()) -- is not shown while the probe starts and then withdrawn on a
    /// system without one; it appears once the accelerator is known (#1870).
    bool acceleratorListed = false;

    friend constexpr bool operator==(const GpuSupport&, const GpuSupport&) = default;
};

/// The GPU model's "known unsupported" flags (Domain::GPUModel::perProcessMetricsKnownUnsupported()
/// and the rest), as gpuSupport() takes them.
struct KnownUnsupported
{
    bool perProcess = false;
    bool utilization = false;
    bool npuPerProcess = false;  ///< perProcessNpuMetricsKnownUnsupported() (#1827)
    bool npuUtilization = false; ///< perProcessNpuUtilizationKnownUnsupported()
};

/// GpuSupport from the GPU model's "known unsupported" flags (@p known) and whether it lists an NPU or
/// compute accelerator: @p hasAccelerator with unknown counted as listed (#1799), @p acceleratorListed
/// only once one is known to be (#1870).
[[nodiscard]] constexpr GpuSupport
gpuSupport(bool hasGpuModel, KnownUnsupported known, bool hasAccelerator = true, bool acceleratorListed = false) noexcept
{
    const bool perProcess = hasGpuModel && !known.perProcess;
    const bool npuPerProcess = hasGpuModel && !known.npuPerProcess;
    return {
        .perProcess = perProcess,
        .utilization = perProcess && !known.utilization,
        .npuPerProcess = npuPerProcess,
        .npuUtilization = npuPerProcess && !known.npuUtilization,
        .accelerator = hasAccelerator, // Needed with npuPerProcess, which a missing model already lacks
        .acceleratorListed = hasAccelerator && acceleratorListed,
    };
}

/// GpuSupport for formatting one snapshot generation's cells, from the support published with it
/// (Domain::ProcessModel::GpuSupport): its GPU fields were read under that, which can differ from the
/// GPU model's current state, which moves on with each GPU sample (#1210, #1417). No per-process data means no
/// utilization either.
/// Whether an accelerator is listed is not published with a generation, so @p accelerator is the GPU
/// model's current word on it (GpuSupport::accelerator).
[[nodiscard]] constexpr GpuSupport gpuSupportOfGeneration(const Domain::ProcessModel::GpuSupport& generation,
                                                          bool accelerator = true) noexcept
{
    return {
        .perProcess = generation.perProcess,
        .utilization = generation.perProcess && generation.utilization,
        .readFailed = (generation.perProcess || generation.npuPerProcess) && generation.readFailed,
        .npuPerProcess = generation.npuPerProcess,
        .npuUtilization = generation.npuPerProcess && generation.npuUtilization,
        .accelerator = accelerator,
    };
}

/// Whether the NPU columns can be filled (#1799): per-process NPU data (#1827) on a system with an NPU
/// or compute accelerator, and for NPU %, per-process NPU utilization too.
[[nodiscard]] constexpr bool npuSupported(GpuSupport gpu) noexcept
{
    return gpu.npuPerProcess && gpu.accelerator;
}

/// Whether the process probe can fill `col` at all on this system. A column it cannot shows
/// ProcessRowFormat::UNAVAILABLE_CELL_TEXT on every row, and its header says why. The GPU columns
/// depend on the GPU probe instead (`gpu`): GPU % on per-process utilization, the others on
/// per-process metrics at all. The NPU columns need an NPU or compute accelerator as well (#1799).
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
    case ProcessColumn::NpuPercent:
        return npuSupported(gpu) && gpu.npuUtilization;
    case ProcessColumn::NpuMemory:
    case ProcessColumn::NpuEngine:
        return npuSupported(gpu);
    case ProcessColumn::Shared:
        return caps.hasSharedMemory;
    case ProcessColumn::Private:
        return caps.hasPrivateBytes;
    case ProcessColumn::Swap:
        return caps.hasSwapBytes;
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
    case ProcessColumn::Container:
        return caps.hasContainer;
    case ProcessColumn::Services:
        return caps.hasServices;
    case ProcessColumn::Type:
        return caps.hasProcessType;
    case ProcessColumn::Architecture:
        return caps.hasArchitecture;
    default:
        return true;
    }
}

/// Whether `col` is shown by default on this system (#1210): getColumnInfo()'s default, unless the probe
/// cannot fill it, so no column shown by default is all dashes -- Power without RAPL on Linux, say.
/// NPU % is the exception the other way: off in getColumnInfo(), it is shown by default once an NPU
/// or compute accelerator is known to be listed (GpuSupport::acceleratorListed, #1870), so a Copilot+
/// PC shows NPU activity without the user going looking, and a machine without one spends no width on it.
[[nodiscard]] constexpr bool defaultVisible(ProcessColumn col, const Platform::ProcessCapabilities& caps, GpuSupport gpu = {}) noexcept
{
    if (!isSupported(col, caps, gpu))
    {
        return false;
    }
    if (col == ProcessColumn::NpuPercent)
    {
        return gpu.acceleratorListed;
    }
    return getColumnInfo(col).defaultVisible;
}

/// The default columns on this system (defaultVisible()). It is what "Reset columns" restores. A
/// hidden column can still be shown from the Columns menu, which says when it is not available on
/// this system.
[[nodiscard]] inline ProcessColumnSettings defaultColumns(const Platform::ProcessCapabilities& caps, GpuSupport gpu = {})
{
    ProcessColumnSettings settings;
    for (const ProcessColumn col : allProcessColumns())
    {
        settings.setDefaultVisible(col, defaultVisible(col, caps, gpu));
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

/// Whether `settings` is exactly this system's default columns, i.e. "Reset columns" would change
/// nothing: no column's visibility, and no column left chosen. A chosen column can match its default
/// today and still differ later -- Power shown then hidden again stays hidden when the system gains
/// power readings -- so Reset stays available to clear the choice.
[[nodiscard]] inline bool
hasDefaultColumns(const ProcessColumnSettings& settings, const Platform::ProcessCapabilities& caps, GpuSupport gpu = {})
{
    return settings == defaultColumns(caps, gpu);
}

/// What a free-text cell (Status, Publisher, Container, Services) shows.
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
    options.hasPrivateBytes = caps.hasPrivateBytes;
    options.hasSwapBytes = caps.hasSwapBytes;
    options.hasIoCounters = caps.hasIoCounters;
    options.hasNetworkCounters = caps.hasNetworkCounters;
    options.hasThreadCount = caps.hasThreadCount;
    options.hasHandleCount = caps.hasHandleCount;
    options.hasPageFaults = caps.hasPageFaults;
    options.hasCpuAffinity = caps.hasCpuAffinity;
    options.hasGdiObjects = caps.hasGdiObjects;
    options.hasPerProcessGpu = gpu.perProcess;
    options.hasPerProcessGpuUtilization = gpu.utilization;
    options.gpuReadFailed = gpu.readFailed;
    options.hasPerProcessNpu = npuSupported(gpu);
    options.hasPerProcessNpuUtilization = gpu.npuUtilization;
    options.hasStatus = caps.hasStatus;
    options.hasPublisher = caps.hasPublisher;
    options.hasContainer = caps.hasContainer;
    options.hasServices = caps.hasServices;
    options.hasProcessType = caps.hasProcessType;
    options.hasArchitecture = caps.hasArchitecture;
    return options;
}

/// Header tooltip line for a column this system cannot fill.
inline constexpr std::string_view UNSUPPORTED_COLUMN_NOTE = "Not available on this system: every row shows \xE2\x80\x94.";

/// Header tooltip line for the Private column where the probe doesn't fill it (#1879): Windows' private
/// bytes are the commit charge, which Virtual already shows.
inline constexpr std::string_view PRIVATE_UNSUPPORTED_COLUMN_NOTE =
    "Not available on this system: every row shows \xE2\x80\x94. Private bytes on Windows are the commit size, shown in the "
    "Virtual column.";

/// Header tooltip line for the Swap column where the probe has no per-process swap figure (#1879).
inline constexpr std::string_view SWAP_UNSUPPORTED_COLUMN_NOTE =
    "Not available on this system: every row shows \xE2\x80\x94. Windows doesn't count how much of each process is in the "
    "pagefile; Virtual, the commit size, includes what is paged out.";

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
        switch (col)
        {
        case ProcessColumn::Private:
            return PRIVATE_UNSUPPORTED_COLUMN_NOTE;
        case ProcessColumn::Swap:
            return SWAP_UNSUPPORTED_COLUMN_NOTE;
        default:
            return UNSUPPORTED_COLUMN_NOTE;
        }
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
