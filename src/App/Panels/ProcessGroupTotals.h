#pragma once

// A Group by group (#3068): its key, label, members and the totals its header shows with Σ, Group: User's
// sign-in sessions (#3377), and the order groups sort in. Split from ProcessGrouping.h so UserGrouping.h, which ProcessGrouping.h's Group:
// User calls (#3377), can build groups without the two headers including each other. Pure; tested in test_ProcessGrouping.cpp and
// test_UserGrouping.cpp.

#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/AsciiCase.h"
#include "Platform/IUserSessions.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace App::ProcessGrouping
{

/// A group's totals over its running members (an exited row, #3060, isn't counted).
struct Totals
{
    std::size_t processes = 0;
    double cpuPercent = 0.0;
    double memoryPercent = 0.0;
    std::uint64_t resident = 0;
    std::uint64_t privateBytes = 0;
    std::uint64_t swap = 0;
    std::int64_t threads = 0;
    std::int64_t handles = 0;
    double cpuTimeSeconds = 0.0;
    double ioRead = 0.0;
    double ioWrite = 0.0;
    double netSent = 0.0;
    double netReceived = 0.0;
    double powerWatts = 0.0;
    double gpuPercent = 0.0;
    std::uint64_t gpuMemory = 0;
    double npuPercent = 0.0;
    std::uint64_t npuMemory = 0;

    void add(const Domain::ProcessSnapshot& proc) noexcept
    {
        ++processes;
        cpuPercent += proc.cpuPercent;
        memoryPercent += proc.memoryPercent;
        resident += proc.memoryBytes;
        privateBytes += proc.privateBytes;
        swap += proc.swapBytes;
        threads += proc.threadCount;
        handles += proc.handleCount;
        cpuTimeSeconds += proc.cpuTimeSeconds;
        ioRead += proc.ioReadBytesPerSec;
        ioWrite += proc.ioWriteBytesPerSec;
        netSent += proc.netSentBytesPerSec;
        netReceived += proc.netReceivedBytesPerSec;
        powerWatts += proc.powerWatts;
        gpuPercent += proc.gpuUtilPercent;
        gpuMemory += proc.gpuMemoryBytes;
        npuPercent += proc.npuUtilPercent;
        npuMemory += proc.npuMemoryBytes;
    }
};

/// @p column's total in @p totals, for sorting and drawing; nullopt for a column whose values don't add up
/// to anything meaningful (names, states, virtual and shared memory, priority, start time, ...).
[[nodiscard]] constexpr std::optional<double> totalFor(const Totals& totals, ProcessColumn column) noexcept
{
    switch (column)
    {
    case ProcessColumn::CpuPercent:
        return totals.cpuPercent;
    case ProcessColumn::MemPercent:
        return totals.memoryPercent;
    case ProcessColumn::Resident:
        return static_cast<double>(totals.resident);
    case ProcessColumn::Private:
        return static_cast<double>(totals.privateBytes);
    case ProcessColumn::Swap:
        return static_cast<double>(totals.swap);
    case ProcessColumn::Threads:
        return static_cast<double>(totals.threads);
    case ProcessColumn::Handles:
        return static_cast<double>(totals.handles);
    case ProcessColumn::CpuTime:
        return totals.cpuTimeSeconds;
    case ProcessColumn::IoRead:
        return totals.ioRead;
    case ProcessColumn::IoWrite:
        return totals.ioWrite;
    case ProcessColumn::NetSent:
        return totals.netSent;
    case ProcessColumn::NetReceived:
        return totals.netReceived;
    case ProcessColumn::Power:
        return totals.powerWatts;
    case ProcessColumn::GpuPercent:
        return totals.gpuPercent;
    case ProcessColumn::GpuMemory:
        return static_cast<double>(totals.gpuMemory);
    case ProcessColumn::NpuPercent:
        return totals.npuPercent;
    case ProcessColumn::NpuMemory:
        return static_cast<double>(totals.npuMemory);
    default:
        return std::nullopt;
    }
}

/// One group: its key (empty: Ungrouped), what its header says, its totals and its members in the table's order.
struct Group
{
    std::string key;   ///< What it groups by, and what keeps it collapsed; empty for Ungrouped
    std::string label; ///< What its header says: the key, except under Group: User (UserGrouping::buildUserGroups)
    Totals totals;
    std::vector<std::size_t> members; ///< Indices into the snapshots, in the order given
    /// Group: User (#3377): the account's sign-in sessions, in the publication's order, drawn under its header;
    /// none in the other modes, and none for an account that isn't signed in (root, SYSTEM, a service's account).
    std::vector<Platform::UserSession> sessions;
    bool current = false;     ///< One of `sessions` is the one TaskSmack runs in
    bool notReadable = false; ///< The sessions whose user couldn't be read (Windows), one group before Ungrouped
};

/// Where @p group sorts before its totals are compared (#3377): Group: User's signed-in accounts first, then
/// every other group, then the sessions whose user couldn't be read, then Ungrouped. The other modes have no
/// sessions, so only Ungrouped moves there, last as before.
[[nodiscard]] inline int groupTier(const Group& group) noexcept
{
    if (group.key.empty())
    {
        return 3;
    }
    if (group.notReadable)
    {
        return 2;
    }
    return group.sessions.empty() ? 1 : 0;
}

/// Orders @p groups by their totals in @p sortColumn, ascending or not as @p ascending says, and by label A
/// to Z between equal totals; by label in the sort's direction for a column without totals. Each tier
/// (groupTier()) is ordered so on its own: signed-in accounts come first and Ungrouped last whatever the order.
inline void sortGroups(std::vector<Group>& groups, ProcessColumn sortColumn, bool ascending)
{
    std::ranges::stable_sort(groups,
                             [sortColumn, ascending](const Group& a, const Group& b)
                             {
                                 const int tierA = groupTier(a);
                                 const int tierB = groupTier(b);
                                 if (tierA != tierB)
                                 {
                                     return tierA < tierB; // signed in first, Ungrouped last
                                 }
                                 const std::optional<double> ta = totalFor(a.totals, sortColumn);
                                 const std::optional<double> tb = totalFor(b.totals, sortColumn);
                                 if (ta.has_value() && tb.has_value() && *ta != *tb)
                                 {
                                     return ascending ? *ta < *tb : *ta > *tb;
                                 }
                                 // By name, A to Z, between equal totals; a column without totals sorts by name in its direction.
                                 const auto byName = Platform::compareIgnoreCase(a.label, b.label);
                                 return (ascending || ta.has_value()) ? byName < 0 : byName > 0;
                             });
}

} // namespace App::ProcessGrouping
