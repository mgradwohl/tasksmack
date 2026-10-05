#pragma once

#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"

#include <optional>

namespace App::ProcessSortUtils
{

/// A value as a sort key that orders one the probe could not read (#1110) below every reading, 0
/// included (std::optional's ordering, as the GDI column already sorts): sorted by Handles without
/// root, other users' processes gather at one end instead of mixing in with real zeros.
template<typename T> [[nodiscard]] constexpr std::optional<T> readingSortKey(bool available, T value) noexcept
{
    return available ? std::optional<T>(value) : std::nullopt;
}

/// Pure comparison logic for sorting the process table, extracted from
/// ProcessesPanel's ImGui table-sort handling so it can be unit-tested without an
/// ImGui context. Mirrors the column -> field mapping ProcessesPanel uses when
/// building the table (see ProcessColumnConfig.h for the column list).
/// @return true if `a` should sort before `b` for the given column and direction.
[[nodiscard]] inline bool
compareByColumn(const Domain::ProcessSnapshot& a, const Domain::ProcessSnapshot& b, ProcessColumn column, bool ascending)
{
    auto compare = [ascending](const auto& lhs, const auto& rhs) -> bool
    {
        return ascending ? (lhs < rhs) : (rhs < lhs);
    };

    switch (column)
    {
    case ProcessColumn::PID:
        return compare(a.pid, b.pid);
    case ProcessColumn::User:
        return compare(a.user, b.user);
    case ProcessColumn::CpuPercent:
        return compare(a.cpuPercent, b.cpuPercent);
    case ProcessColumn::MemPercent:
        return compare(a.memoryPercent, b.memoryPercent);
    case ProcessColumn::Virtual:
        return compare(a.virtualBytes, b.virtualBytes);
    case ProcessColumn::Resident:
        return compare(a.memoryBytes, b.memoryBytes);
    case ProcessColumn::PeakResident:
        return compare(a.peakMemoryBytes, b.peakMemoryBytes);
    case ProcessColumn::Shared:
        return compare(a.sharedBytes, b.sharedBytes);
    case ProcessColumn::CpuTime:
        return compare(a.cpuTimeSeconds, b.cpuTimeSeconds);
    case ProcessColumn::StartTime:
        return compare(a.startTimeEpoch, b.startTimeEpoch);
    case ProcessColumn::State:
        return compare(a.displayState, b.displayState);
    case ProcessColumn::Status:
        return compare(a.status, b.status);
    case ProcessColumn::Name:
        return compare(a.name, b.name);
    case ProcessColumn::PPID:
        return compare(a.parentPid, b.parentPid);
    case ProcessColumn::Priority:
        return compare(a.nice, b.nice);
    case ProcessColumn::Threads:
        return compare(a.threadCount, b.threadCount);
    case ProcessColumn::Handles:
        return compare(readingSortKey(a.handleCountAvailable, a.handleCount), readingSortKey(b.handleCountAvailable, b.handleCount));
    case ProcessColumn::PageFaults:
        return compare(a.pageFaults, b.pageFaults);
    case ProcessColumn::Affinity:
        return compare(a.cpuAffinityMask, b.cpuAffinityMask);
    case ProcessColumn::Command:
        return compare(a.command, b.command);
    case ProcessColumn::IoRead:
        return compare(readingSortKey(a.ioAvailable, a.ioReadBytesPerSec), readingSortKey(b.ioAvailable, b.ioReadBytesPerSec));
    case ProcessColumn::IoWrite:
        return compare(readingSortKey(a.ioAvailable, a.ioWriteBytesPerSec), readingSortKey(b.ioAvailable, b.ioWriteBytesPerSec));
    case ProcessColumn::NetSent:
        return compare(readingSortKey(a.networkAvailable, a.netSentBytesPerSec), readingSortKey(b.networkAvailable, b.netSentBytesPerSec));
    case ProcessColumn::NetReceived:
        return compare(readingSortKey(a.networkAvailable, a.netReceivedBytesPerSec),
                       readingSortKey(b.networkAvailable, b.netReceivedBytesPerSec));
    case ProcessColumn::Power:
        return compare(a.powerWatts, b.powerWatts);
    case ProcessColumn::GpuPercent:
        return compare(a.gpuUtilPercent, b.gpuUtilPercent);
    case ProcessColumn::GpuMemory:
        return compare(a.gpuMemoryBytes, b.gpuMemoryBytes);
    case ProcessColumn::GpuEngine:
    {
        // Sort by number of engines, then by first engine name
        if (a.gpuEngines.size() != b.gpuEngines.size())
        {
            return compare(a.gpuEngines.size(), b.gpuEngines.size());
        }
        if (!a.gpuEngines.empty() && !b.gpuEngines.empty())
        {
            return compare(a.gpuEngines[0], b.gpuEngines[0]);
        }
        return false;
    }
    case ProcessColumn::GpuDevice:
        return compare(a.gpuDevices, b.gpuDevices);
    case ProcessColumn::Publisher:
        return compare(a.publisher, b.publisher);
    case ProcessColumn::Type:
        return compare(a.processType, b.processType);
    case ProcessColumn::GdiObjects:
        return compare(a.gdiObjectCount, b.gdiObjectCount);
    default:
        return false;
    }
}

} // namespace App::ProcessSortUtils
