#pragma once

// The Processes table's Group by (#3068): processes gathered under group header rows, by systemd unit,
// container, app or user, each header showing its group's totals marked with Σ, so a rollup never reads
// as one process's figure (#838). The group itself and its totals are in ProcessGroupTotals.h; by user,
// the account matching is UserGrouping's (#3377). Pure, so the keys, totals and order are tested without
// ImGui (test_ProcessGrouping.cpp, test_UserGrouping.cpp).

#include "App/Panels/ProcessGroupTotals.h"
#include "App/Panels/UserGrouping.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/ProcessTypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
    App,  ///< Its app: Linux, the desktop app, Flatpak or Snap its unit was launched as; Windows, its package or AppUserModelID (#3457)
    User, ///< The account it runs as (domain and user on Windows, #3377): per-user totals (#3076)
};

/// The label of the group for processes in none.
inline constexpr std::string_view UNGROUPED_LABEL = "Ungrouped";

/// Whether a process's unit comes from the service list: on Windows it is the first service the list says
/// the process hosts; on Linux it is the systemd unit its own cgroup names, so the list adds nothing (#3386).
#ifdef _WIN32
inline constexpr bool UNITS_FROM_SERVICE_LIST = true;
#else
inline constexpr bool UNITS_FROM_SERVICE_LIST = false;
#endif

/// Whether the service list is needed in the background, beside the Services tab (#1880, #3386): while
/// the Processes table shows its Services column (@p servicesColumnVisible), Process Details' Services
/// section is open (@p servicesSectionOpen), or the table is grouped by Unit (@p groupBy) where units
/// come from the list (@p unitsFromServiceList).
[[nodiscard]] constexpr bool
servicesBackgroundDemand(bool servicesColumnVisible, bool servicesSectionOpen, GroupBy groupBy, bool unitsFromServiceList) noexcept
{
    return servicesColumnVisible || servicesSectionOpen || (unitsFromServiceList && groupBy == GroupBy::Unit);
}

/// Whether Group: User shows each account's signed-in sessions under its header (#3377). Not yet: the fold
/// that adds the session rows turns it on, and until then the Processes table reads no sessions.
inline constexpr bool USER_GROUPS_SHOW_SESSIONS = false;

/// Whether the signed-in sessions are read (App::UserSessionsFeed, #3377): only while a view that shows them
/// is shown, the Users tab (@p usersTabShown), or the Processes tab (@p processesTabShown) grouped by User
/// (@p groupBy) once its groups show sessions (@p userGroupsShowSessions). At the Settings interval meanwhile.
[[nodiscard]] constexpr bool
sessionsDemand(bool usersTabShown, bool processesTabShown, GroupBy groupBy, bool userGroupsShowSessions) noexcept
{
    return usersTabShown || (userGroupsShowSessions && processesTabShown && groupBy == GroupBy::User);
}

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

/// The app @p proc belongs to by its own identity (#3457), or empty: its package family (a packaged app's, on
/// Windows), else its AppUserModelID (a packaged app's "<family>!<app>" is its family, so a process whose
/// package couldn't be read joins the rest of its package), else the app its systemd unit was launched as
/// (Linux, appFromUnit()).
[[nodiscard]] inline std::string ownAppKey(const Domain::ProcessSnapshot& proc)
{
    if (!proc.packageFamily.empty())
    {
        return proc.packageFamily;
    }
    if (!proc.appUserModelId.empty())
    {
        const std::string_view id = proc.appUserModelId;
        const std::size_t bang = id.find('!');
        return std::string((bang != std::string_view::npos && bang > 0) ? id.substr(0, bang) : id);
    }
    return proc.serviceNames.empty() ? std::string{} : appFromUnit(proc.serviceNames[0]);
}

/// Whether @p proc's own app (ownAppKey()) is a package: its family, or a packaged app's AppUserModelID.
[[nodiscard]] inline bool ownAppIsPackage(const Domain::ProcessSnapshot& proc)
{
    return !proc.packageFamily.empty() || proc.appUserModelId.contains('!');
}

/// What an App group's header calls the app @p proc names by its own identity @p key (#3457): the name the
/// Start menu gives it, else a package's name without its publisher ID ("Microsoft.WindowsTerminal"), else
/// the key itself.
[[nodiscard]] inline std::string appGroupLabel(const Domain::ProcessSnapshot& proc, const std::string& key)
{
    if (!proc.appName.empty())
    {
        return proc.appName;
    }
    if (ownAppIsPackage(proc))
    {
        const std::size_t underscore = key.rfind('_');
        return (underscore != std::string::npos && underscore > 0) ? key.substr(0, underscore) : key;
    }
    return key;
}

/// Process Details' App row (#3457): the app as its App group's header names it, empty for a process with no
/// package or AppUserModelID of its own (Linux's app is in its Services row).
[[nodiscard]] inline std::string appDetailsValue(const Domain::ProcessSnapshot& proc)
{
    if (proc.packageFamily.empty() && proc.appUserModelId.empty())
    {
        return {};
    }
    return appGroupLabel(proc, ownAppKey(proc));
}

/// The App row's tooltip: the package family and AppUserModelID the app is read from.
[[nodiscard]] inline std::string appDetailsTooltip(const Domain::ProcessSnapshot& proc)
{
    std::string tooltip;
    if (!proc.packageFamily.empty())
    {
        tooltip = "Package family: " + proc.packageFamily;
    }
    if (!proc.appUserModelId.empty())
    {
        tooltip += (tooltip.empty() ? "" : "\n") + std::string("AppUserModelID: ") + proc.appUserModelId;
    }
    return tooltip;
}

/// Each process's App group (#3457), by index into @p snapshots: its own (ownAppKey()), else the one it
/// follows from its parent, as Task Manager puts an app's helpers under it. A process with no app of its
/// own follows a parent whose app is a package (a packaged app's shells and helpers, unpackaged
/// themselves), or a parent running the same program (a browser's renderers under the browser). Anything
/// else, such as a program Explorer starts, is its own. A parent is the process with the parent PID that
/// started no later than the child, so a reused PID isn't taken for one.
[[nodiscard]] inline std::vector<std::string> appKeys(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    struct Resolved
    {
        std::string key;
        bool fromPackage = false;
        bool done = false;
    };
    std::vector<Resolved> resolved(snapshots.size());
    std::unordered_map<std::int32_t, std::size_t> byPid;
    byPid.reserve(snapshots.size());
    for (std::size_t i = 0; i < snapshots.size(); ++i)
    {
        byPid.try_emplace(snapshots[i].pid, i);
    }
    const auto parentOf = [&snapshots, &byPid](std::size_t child) -> std::optional<std::size_t>
    {
        const Domain::ProcessSnapshot& proc = snapshots[child];
        const auto it = byPid.find(proc.parentPid);
        if (proc.parentPid == proc.pid || it == byPid.end() || it->second == child)
        {
            return std::nullopt;
        }
        const Domain::ProcessSnapshot& parent = snapshots[it->second];
        if (parent.startTimeTicks != 0 && proc.startTimeTicks != 0 && parent.startTimeTicks > proc.startTimeTicks)
        {
            return std::nullopt; // Started after the child: a process that reused the parent's PID
        }
        return it->second;
    };
    std::vector<std::size_t> chain;
    std::vector<bool> onChain(snapshots.size(), false); // The walk's processes, so a PID loop ends it
    for (std::size_t start = 0; start < snapshots.size(); ++start)
    {
        // Up through the processes with no app of their own, to one already resolved, one with an app, one with
        // no parent listed, or one whose parent is already on the walk (a PID loop). However deep the tree and in
        // whatever order it is listed, every process above the start is resolved by its own ancestors alone.
        for (const std::size_t index : chain)
        {
            onChain[index] = false;
        }
        chain.clear();
        std::size_t current = start;
        while (!resolved[current].done && ownAppKey(snapshots[current]).empty())
        {
            const std::optional<std::size_t> parent = parentOf(current);
            if (!parent.has_value() || *parent == current || onChain[*parent])
            {
                break;
            }
            chain.push_back(current);
            onChain[current] = true;
            current = *parent;
        }
        if (!resolved[current].done)
        {
            resolved[current] = Resolved{
                .key = ownAppKey(snapshots[current]),
                .fromPackage = ownAppIsPackage(snapshots[current]),
                .done = true,
            };
        }
        // Back down the chain: each follows the process above it where the rule lets it.
        for (std::size_t step = chain.size(); step-- > 0;)
        {
            const std::size_t child = chain[step];
            const std::size_t above = (step + 1 < chain.size()) ? chain[step + 1] : current;
            Resolved& r = resolved[child];
            r.done = true;
            const Resolved& parent = resolved[above];
            if (!parent.key.empty() && (parent.fromPackage || snapshots[child].name == snapshots[above].name))
            {
                r.key = parent.key;
                r.fromPackage = parent.fromPackage;
            }
        }
    }
    std::vector<std::string> keys;
    keys.reserve(resolved.size());
    for (Resolved& r : resolved)
    {
        keys.push_back(std::move(r.key));
    }
    return keys;
}

/// What each App group's header says (#3457), by key: named by a process in @p snapshots that is the app itself,
/// not one following it (appGroupLabel()), the first with the Start menu's name, else the first that is the app.
/// Read from every process, not only those a filter shows, so a group filtered down to a terminal's shell is
/// still named for the terminal.
[[nodiscard]] inline std::unordered_map<std::string, std::string> appGroupLabels(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    std::unordered_map<std::string, std::string> labels;
    std::unordered_map<std::string, bool> named; // Whether the label is the Start menu's name, which nothing replaces
    for (const Domain::ProcessSnapshot& proc : snapshots)
    {
        std::string key = ownAppKey(proc);
        if (key.empty())
        {
            continue;
        }
        auto [isNamed, added] = named.try_emplace(key, false);
        if (added || (!isNamed->second && !proc.appName.empty()))
        {
            isNamed->second = !proc.appName.empty();
            std::string label = appGroupLabel(proc, key);
            labels.insert_or_assign(std::move(key), std::move(label));
        }
    }
    return labels;
}

/// The group @p proc belongs to under @p mode, or empty for none (Ungrouped). By App it is its own app
/// (ownAppKey()); buildGroups() then puts a process with none under the app it follows (appKeys()). By user it is the account the
/// process names (UserGrouping::processAccountKey()); buildGroups() then gives a Windows process that names
/// no domain to the one account with its name.
[[nodiscard]] inline std::string groupKey(const Domain::ProcessSnapshot& proc, GroupBy mode)
{
    switch (mode)
    {
    case GroupBy::Unit:
        return proc.serviceNames.empty() ? std::string{} : proc.serviceNames[0];
    case GroupBy::Container:
        return proc.container;
    case GroupBy::App:
        return ownAppKey(proc);
    case GroupBy::User:
        return UserGrouping::processAccountKey(proc);
    case GroupBy::None:
        break;
    }
    return {};
}

/// The groups of @p order (indices into @p snapshots, already filtered and sorted) under @p mode. A
/// member at or past @p liveCount is an exited row (#3060): listed, not counted. Groups are ordered as
/// sortGroups() orders them. Group: User matches accounts across processes, so UserGrouping builds it
/// (buildUserGroups(), #3377).
[[nodiscard]] inline std::vector<Group> buildGroups(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                                    const std::vector<std::size_t>& order,
                                                    GroupBy mode,
                                                    std::size_t liveCount,
                                                    ProcessColumn sortColumn,
                                                    bool ascending)
{
    if (mode == GroupBy::User)
    {
        return UserGrouping::buildUserGroups(snapshots, order, liveCount, sortColumn, ascending);
    }
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> byKey;
    const bool byApp = mode == GroupBy::App;
    const std::vector<std::string> followedApps = byApp ? appKeys(snapshots) : std::vector<std::string>{};
    const std::unordered_map<std::string, std::string> appLabels =
        byApp ? appGroupLabels(snapshots) : std::unordered_map<std::string, std::string>{};
    for (const std::size_t index : order)
    {
        if (index >= snapshots.size())
        {
            continue;
        }
        const Domain::ProcessSnapshot& proc = snapshots[index];
        std::string key = byApp ? followedApps[index] : groupKey(proc, mode);
        auto [slot, added] = byKey.try_emplace(std::move(key), groups.size());
        if (added)
        {
            const std::string& newKey = slot->first;
            const auto named = appLabels.find(newKey);
            groups.push_back(Group{
                .key = newKey,
                .label = (named != appLabels.end()) ? named->second : newKey,
                .totals = {},
                .members = {},
            });
        }
        Group& group = groups[slot->second];
        group.members.push_back(index);
        if (index < liveCount)
        {
            group.totals.add(proc);
        }
    }
    sortGroups(groups, sortColumn, ascending);
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

/// Every mode, in the Group control's order.
inline constexpr std::array<GroupBy, 5> ALL_GROUP_MODES = {GroupBy::None, GroupBy::Unit, GroupBy::Container, GroupBy::App, GroupBy::User};

/// Whether the Group control offers @p mode on a system whose probe reports @p caps (#3428): Container
/// needs the container (Linux cgroups), App the systemd unit an app was launched in (Linux) or the package
/// and AppUserModelID (Windows, #3457); offered anywhere else, every process would land under Ungrouped. None, Unit and User are offered
/// everywhere (Unit's data on Windows comes from the service list, which grouping by Unit asks for, #3386).
[[nodiscard]] constexpr bool groupModeOffered(GroupBy mode, const Platform::ProcessCapabilities& caps) noexcept
{
    switch (mode)
    {
    case GroupBy::Container:
        return caps.hasContainer;
    case GroupBy::App:
        return caps.hasAppUnits || caps.hasAppIds;
    case GroupBy::None:
    case GroupBy::Unit:
    case GroupBy::User:
        break;
    }
    return true;
}

/// The modes the Group control offers under @p caps, in its order.
[[nodiscard]] inline std::vector<GroupBy> offeredGroupModes(const Platform::ProcessCapabilities& caps)
{
    std::vector<GroupBy> modes;
    modes.reserve(ALL_GROUP_MODES.size());
    std::ranges::copy_if(ALL_GROUP_MODES, std::back_inserter(modes), [&caps](GroupBy mode) { return groupModeOffered(mode, caps); });
    return modes;
}

/// The mode the table is grouped by when @p chosen is the saved choice (#3428): itself where it is
/// offered, else None. The saved choice is kept, so a config shared with a Linux machine, or read before
/// the probe's capabilities are known, still groups by Container or App once they are.
[[nodiscard]] constexpr GroupBy effectiveGroupBy(GroupBy chosen, const Platform::ProcessCapabilities& caps) noexcept
{
    return groupModeOffered(chosen, caps) ? chosen : GroupBy::None;
}

} // namespace App::ProcessGrouping
