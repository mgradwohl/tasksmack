#pragma once

// Which Processes table columns this system's process probe can fill, and what a column's header
// tooltip says about the values it does not have (#1210). Pure (no ImGui calls), so it is
// unit-testable without a live context, following CONTRIBUTING.md's "extract the pure decision
// logic into a small header" pattern.

#include "App/Panels/ProcessRowFormat.h"
#include "App/ProcessColumnConfig.h"
#include "Platform/ProcessTypes.h"

#include <string_view>

namespace App::ProcessColumnAvailability
{

/// Whether the process probe can fill `col` at all on this system. A column it cannot shows
/// ProcessRowFormat::UNAVAILABLE_CELL_TEXT on every row, and its header says why.
///
/// Peak Memory is not gated on ProcessCapabilities::hasPeakRss: where the OS reports no peak, the
/// process model tracks one itself.
[[nodiscard]] constexpr bool isSupported(ProcessColumn col, const Platform::ProcessCapabilities& caps) noexcept
{
    switch (col)
    {
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

/// The row formatter's view of the same capabilities.
[[nodiscard]] constexpr ProcessRowFormat::RowFormatOptions rowFormatOptions(const Platform::ProcessCapabilities& caps) noexcept
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
[[nodiscard]] constexpr std::string_view unavailableValuesNote(ProcessColumn col, const Platform::ProcessCapabilities& caps) noexcept
{
    if (!isSupported(col, caps))
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
