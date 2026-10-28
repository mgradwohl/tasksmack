#pragma once

// The Processes table's Group by (#3068): processes gathered under group header rows, by systemd unit,
// container or app, each header showing its group's totals marked with Σ, so a rollup never reads as
// one process's figure (#838). Pure, so the keys, totals and order are tested without ImGui
// (test_ProcessGrouping.cpp).

#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/AsciiCase.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App::ProcessGrouping
{

/// What the table is grouped by.
enum class GroupBy : std::uint8_t
{
    None,
    Unit,      ///< The systemd unit the process's cgroup names (Linux); the first service it hosts (Windows)
    Container, ///< Its container ("docker:1a2b3c4d5e6f")
    App,       ///< The app its unit was launched as: a desktop app, a Flatpak or a Snap
    User,      ///< The user it runs as: per-user totals, the Users view's read-only first part (#3076)
};

/// The label of the group for processes in none.
inline constexpr std::string_view UNGROUPED_LABEL = "Ungrouped";

/// The app a systemd unit was launched as, or empty when it isn't an app's:
/// - a desktop or Flatpak app's scope: "app-<launcher>-<app id>-<n>.scope" ("app-gnome-firefox-4521.scope"
///   is "firefox"; "app-flatpak-org.mozilla.firefox-7731.scope" is "org.mozilla.firefox");
/// - an app's service, as systemd's XDG autostart and newer launchers start them:
///   "app-<launcher>-<app id>@<instance>.service" or "app-<app id>@<instance>.service";
/// - a Snap: "snap.<name>.<app>-<uuid>.scope" or "snap.<name>.<app>.service" is "<name>".
[[nodiscard]] inline std::string appFromUnit(std::string_view unit)
{
    const auto stripSuffix = [](std::string_view text, std::string_view suffix) -> std::optional<std::string_view>
    {
        if (!text.ends_with(suffix))
        {
            return std::nullopt;
        }
        text.remove_suffix(suffix.size());
        return text;
    };
    if (unit.starts_with("snap."))
    {
        const std::string_view rest = unit.substr(5);
        const std::size_t dot = rest.find('.');
        return dot == std::string_view::npos || dot == 0 ? std::string{} : std::string(rest.substr(0, dot));
    }
    if (!unit.starts_with("app-"))
    {
        return {};
    }
    const std::string_view rest = unit.substr(4);
    if (const auto scope = stripSuffix(rest, ".scope"))
    {
        // "<launcher>-<app id>-<n>": the number after the last '-' is the instance; the launcher before
        // the first '-' (gnome, kde, flatpak, ...). App ids may hold '-' themselves.
        std::string_view body = *scope;
        const std::size_t lastDash = body.rfind('-');
        if (lastDash != std::string_view::npos &&
            std::ranges::all_of(body.substr(lastDash + 1), [](char c) { return c >= '0' && c <= '9'; }) && lastDash + 1 < body.size())
        {
            body = body.substr(0, lastDash);
        }
        const std::size_t firstDash = body.find('-');
        return firstDash == std::string_view::npos ? std::string(body) : std::string(body.substr(firstDash + 1));
    }
    if (const auto service = stripSuffix(rest, ".service"))
    {
        std::string_view body = *service;
        if (const std::size_t at = body.find('@'); at != std::string_view::npos)
        {
            body = body.substr(0, at);
        }
        // "<launcher>-<app id>" when the part before the first '-' has no '.', else the app id itself.
        if (const std::size_t firstDash = body.find('-'); firstDash != std::string_view::npos && !body.substr(0, firstDash).contains('.'))
        {
            body = body.substr(firstDash + 1);
        }
        return std::string(body);
    }
    return {};
}

/// The group @p proc belongs to under @p mode, or empty for none (Ungrouped).
[[nodiscard]] inline std::string groupKey(const Domain::ProcessSnapshot& proc, GroupBy mode)
{
    switch (mode)
    {
    case GroupBy::Unit:
        return proc.serviceNames.empty() ? std::string{} : proc.serviceNames[0];
    case GroupBy::Container:
        return proc.container;
    case GroupBy::App:
        return proc.serviceNames.empty() ? std::string{} : appFromUnit(proc.serviceNames[0]);
    case GroupBy::User:
        return proc.user;
    case GroupBy::None:
        break;
    }
    return {};
}

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

/// One group: its key (empty: Ungrouped), totals and members in the table's order.
struct Group
{
    std::string key;
    Totals totals;
    std::vector<std::size_t> members; ///< Indices into the snapshots, in the order given
};

/// The groups of @p order (indices into @p snapshots, already filtered and sorted) under @p mode. A
/// member at or past @p liveCount is an exited row (#3060): listed, not counted. Groups are ordered by
/// their totals in @p sortColumn, ascending or not as @p ascending says, and by name A to Z between equal
/// totals; by name in the sort's direction for a column without totals. Ungrouped comes last whatever
/// the order.
[[nodiscard]] inline std::vector<Group> buildGroups(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                                    const std::vector<std::size_t>& order,
                                                    GroupBy mode,
                                                    std::size_t liveCount,
                                                    ProcessColumn sortColumn,
                                                    bool ascending)
{
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> byKey;
    for (const std::size_t index : order)
    {
        if (index >= snapshots.size())
        {
            continue;
        }
        const Domain::ProcessSnapshot& proc = snapshots[index];
        std::string key = groupKey(proc, mode);
        auto [slot, added] = byKey.try_emplace(key, groups.size());
        if (added)
        {
            groups.push_back(Group{.key = std::move(key), .totals = {}, .members = {}});
        }
        Group& group = groups[slot->second];
        group.members.push_back(index);
        if (index < liveCount)
        {
            group.totals.add(proc);
        }
    }
    std::ranges::stable_sort(groups,
                             [sortColumn, ascending](const Group& a, const Group& b)
                             {
                                 if (a.key.empty() != b.key.empty())
                                 {
                                     return b.key.empty(); // Ungrouped last
                                 }
                                 const std::optional<double> ta = totalFor(a.totals, sortColumn);
                                 const std::optional<double> tb = totalFor(b.totals, sortColumn);
                                 if (ta.has_value() && tb.has_value() && *ta != *tb)
                                 {
                                     return ascending ? *ta < *tb : *ta > *tb;
                                 }
                                 // By name, A to Z, between equal totals; a column without totals sorts by name in its direction.
                                 const auto byName = Platform::compareIgnoreCase(a.key, b.key);
                                 return (ascending || ta.has_value()) ? byName < 0 : byName > 0;
                             });
    return groups;
}

/// The group mode's label in the Group control.
[[nodiscard]] constexpr std::string_view groupByLabel(GroupBy mode) noexcept
{
    switch (mode)
    {
    case GroupBy::Unit:
        return "Unit";
    case GroupBy::Container:
        return "Container";
    case GroupBy::App:
        return "App";
    case GroupBy::User:
        return "User";
    case GroupBy::None:
        break;
    }
    return "None";
}

/// The config value for @p mode, and back (anything else: None).
[[nodiscard]] constexpr std::string_view groupByConfigValue(GroupBy mode) noexcept
{
    switch (mode)
    {
    case GroupBy::Unit:
        return "unit";
    case GroupBy::Container:
        return "container";
    case GroupBy::App:
        return "app";
    case GroupBy::User:
        return "user";
    case GroupBy::None:
        break;
    }
    return "none";
}

[[nodiscard]] constexpr GroupBy groupByFromConfig(std::string_view value) noexcept
{
    for (const GroupBy mode : {GroupBy::Unit, GroupBy::Container, GroupBy::App, GroupBy::User})
    {
        if (value == groupByConfigValue(mode))
        {
            return mode;
        }
    }
    return GroupBy::None;
}

} // namespace App::ProcessGrouping
