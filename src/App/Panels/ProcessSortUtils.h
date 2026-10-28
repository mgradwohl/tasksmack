#pragma once

#include "App/ProcessColumnConfig.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/SchedPolicy.h"

#include <algorithm>
#include <cctype>
#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace App::ProcessSortUtils
{

/// A value as a sort key that orders one the probe could not read (#1110) below every reading, 0
/// included (std::optional's ordering, as the GDI column already sorts): sorted by Handles without
/// root, other users' processes gather at one end instead of mixing in with real zeros.
template<typename T> [[nodiscard]] constexpr std::optional<T> readingSortKey(bool available, T value) noexcept
{
    return available ? std::optional<T>(value) : std::nullopt;
}

/// An engine list as the GPU Engine and NPU Engine columns sort it: by the number of engines, then by
/// the first engine's name. Equal lists, and two empty ones, compare false both ways.
[[nodiscard]] inline bool compareEngines(const std::vector<std::string>& a, const std::vector<std::string>& b, bool ascending)
{
    if (a.size() != b.size())
    {
        return ascending ? (a.size() < b.size()) : (b.size() < a.size());
    }
    if (!a.empty() && !b.empty())
    {
        return ascending ? (a[0] < b[0]) : (b[0] < a[0]);
    }
    return false;
}

/// The Services column's text as it sorts (#1880): A to Z ignoring case (Windows names services in
/// mixed case: "Appinfo", "BFE", "bits"), Z to A descending; a process hosting none sorts after every
/// one hosting some in either direction, so the hosts stay together at the top. Equal texts, and two
/// empty ones, compare false both ways.
[[nodiscard]] inline bool compareServicesText(std::string_view a, std::string_view b, bool ascending)
{
    if (a.empty() != b.empty())
    {
        return !a.empty();
    }
    const auto lessIgnoringCase = [](std::string_view lhs, std::string_view rhs)
    {
        return std::ranges::lexicographical_compare(
            lhs, rhs, [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    };
    return ascending ? lessIgnoringCase(a, b) : lessIgnoringCase(b, a);
}

/// The column's own key comparison, with no tie-breaker: equal keys compare false both ways.
/// @return true if `a`'s key for `column` sorts before `b`'s in the given direction.
[[nodiscard]] inline bool
compareColumnKey(const Domain::ProcessSnapshot& a, const Domain::ProcessSnapshot& b, ProcessColumn column, bool ascending)
{
    const auto compare = [ascending](const auto& lhs, const auto& rhs) -> bool
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
    case ProcessColumn::Private:
        return compare(a.privateBytes, b.privateBytes);
    case ProcessColumn::Swap:
        return compare(a.swapBytes, b.swapBytes);
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
        // By class, then nice: on Windows, Realtime sorts above High (#1280).
        return compare(Domain::Priority::prioritySortKey(a.priorityClass, a.nice),
                       Domain::Priority::prioritySortKey(b.priorityClass, b.nice));
    case ProcessColumn::Threads:
        return compare(a.threadCount, b.threadCount);
    case ProcessColumn::Handles:
        return compare(readingSortKey(a.handleCountAvailable, a.handleCount), readingSortKey(b.handleCountAvailable, b.handleCount));
    case ProcessColumn::PageFaults:
        return compare(a.pageFaults, b.pageFaults);
    case ProcessColumn::Affinity:
        return compare(a.cpuAffinity, b.cpuAffinity);
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
        return compareEngines(a.gpuEngines, b.gpuEngines, ascending);
    case ProcessColumn::GpuDevice:
        return compare(a.gpuDevices, b.gpuDevices);
    case ProcessColumn::NpuPercent:
        return compare(a.npuUtilPercent, b.npuUtilPercent);
    case ProcessColumn::NpuMemory:
        return compare(a.npuMemoryBytes, b.npuMemoryBytes);
    case ProcessColumn::NpuEngine:
        return compareEngines(a.npuEngines, b.npuEngines, ascending);
    case ProcessColumn::Publisher:
        return compare(a.publisher, b.publisher);
    case ProcessColumn::Container:
        return compare(a.container, b.container);
    case ProcessColumn::Services:
        return compareServicesText(a.services, b.services, ascending);
    case ProcessColumn::Type:
        return compare(a.processType, b.processType);
    case ProcessColumn::Architecture:
        return compare(a.architecture, b.architecture);
    case ProcessColumn::Policy: // By policy (Idle .. FIFO), then real-time priority (#3086)
    {
        const std::strong_ordering order = Platform::compareSchedPolicy(a.schedPolicy, a.rtPriority, b.schedPolicy, b.rtPriority);
        return ascending ? order < 0 : order > 0;
    }
    case ProcessColumn::GdiObjects:
        return compare(a.gdiObjectCount, b.gdiObjectCount);
    default:
        return false;
    }
}

/// Pure comparison logic for sorting the process table, extracted from
/// ProcessesPanel's ImGui table-sort handling so it can be unit-tested without an
/// ImGui context. Mirrors the column -> field mapping ProcessesPanel uses when
/// building the table (see ProcessColumnConfig.h for the column list).
///
/// Rows whose keys tie (most processes sit at 0.0% CPU) are ordered by PID, then by unique key,
/// in the same direction, so the order is total: std::ranges::sort is not stable, and without a
/// tie-breaker the tied rows reshuffled on every refresh (#1174).
/// @return true if `a` should sort before `b` for the given column and direction.
[[nodiscard]] inline bool
compareByColumn(const Domain::ProcessSnapshot& a, const Domain::ProcessSnapshot& b, ProcessColumn column, bool ascending)
{
    if (column >= ProcessColumn::Count)
    {
        return false; // Not a real column: no order at all, not even by PID.
    }
    if (compareColumnKey(a, b, column, ascending))
    {
        return true;
    }
    if (compareColumnKey(b, a, column, ascending))
    {
        return false;
    }
    if (a.pid != b.pid)
    {
        return ascending ? (a.pid < b.pid) : (b.pid < a.pid);
    }
    return ascending ? (a.uniqueKey < b.uniqueKey) : (b.uniqueKey < a.uniqueKey);
}

} // namespace App::ProcessSortUtils
