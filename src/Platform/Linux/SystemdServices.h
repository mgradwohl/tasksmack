#pragma once

// systemd's services as the Services tab lists them (#2006): pure, so it is unit-tested on fixture rows;
// SystemdBus::readServices() fills the Listing from org.freedesktop.systemd1.Manager, and
// LinuxServiceProbe turns it into ServiceInfos here.
//
// ListUnits returns the units systemd has loaded, with their state; ListUnitFiles every installed unit
// file and whether it is enabled. A disabled service that nothing has started is a unit file only, so
// both are read: the loaded units with their state, then the installed services that aren't loaded, as
// Stopped -- the Windows list shows every installed service, running or not, and so does this one.
// Template unit files ("getty@.service") are not services themselves; their instances
// ("getty@tty1.service") are, and take the template's enablement.

#include "Platform/IServiceProbe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::SystemdServices
{

/// One loaded unit, as ListUnits reports it.
struct Unit
{
    std::string name;        ///< "cron.service"
    std::string description; ///< "Regular background program processing daemon"
    std::string activeState; ///< "active", "inactive", "activating", "deactivating", "failed", "reloading"...
    std::string subState;    ///< "running", "exited", "dead"...
    std::string objectPath;  ///< The unit's object, for its MainPID
};

/// What SystemdBus::readServices() read.
struct Listing
{
    std::vector<Unit> units;                                    ///< The loaded services
    std::vector<std::pair<std::string, std::string>> unitFiles; ///< Every installed service file: name, state ("enabled"...)
    std::map<std::string, std::uint32_t> mainPids;              ///< A running service's main process, by unit name
    /// A loaded template instance's own unit file state, by unit name (#2425): ListUnitFiles lists only
    /// the template, and an instance can be enabled or not on its own (getty@tty2 under an enabled getty@).
    std::map<std::string, std::string> instanceFileStates;
};

inline constexpr std::string_view SERVICE_SUFFIX = ".service";

/// Whether @p name is a service unit's: "*.service", and not a template ("*@.service").
[[nodiscard]] constexpr bool isService(std::string_view name) noexcept
{
    return name.size() > SERVICE_SUFFIX.size() && name.ends_with(SERVICE_SUFFIX) && !name.ends_with("@.service");
}

/// An instance's template ("getty@tty1.service" -> "getty@.service"), or @p name itself.
[[nodiscard]] inline std::string templateOf(std::string_view name)
{
    const std::size_t at = name.find('@');
    if (at == std::string_view::npos || !name.ends_with(SERVICE_SUFFIX))
    {
        return std::string(name);
    }
    return std::string(name.substr(0, at + 1)) + std::string(SERVICE_SUFFIX);
}

/// A unit's ActiveState as a ServiceState: active (and reloading) is running, a oneshot that has run
/// ("active (exited)") included, as systemctl shows it; activating and deactivating are pending; inactive
/// and failed are stopped.
[[nodiscard]] constexpr ServiceState stateFrom(std::string_view activeState) noexcept
{
    if (activeState == "active" || activeState == "reloading" || activeState == "refreshing")
    {
        return ServiceState::Running;
    }
    if (activeState == "activating")
    {
        return ServiceState::StartPending;
    }
    if (activeState == "deactivating")
    {
        return ServiceState::StopPending;
    }
    if (activeState == "inactive" || activeState == "failed" || activeState == "maintenance")
    {
        return ServiceState::Stopped;
    }
    return ServiceState::Unknown;
}

/// A unit file's state as a start type, matching what SystemdActions sets (#2006, #2114): enabled starts
/// at boot (Automatic); masked can never start (Disabled); disabled, static, indirect, generated and the
/// like start only when something asks (Manual) -- a disabled unit can still be started by hand, which is
/// Windows' Manual, and Manual is what DisableUnitFiles writes.
[[nodiscard]] constexpr ServiceStartType startTypeFrom(std::string_view unitFileState) noexcept
{
    if (unitFileState == "enabled" || unitFileState == "enabled-runtime")
    {
        return ServiceStartType::Automatic;
    }
    if (unitFileState == "masked" || unitFileState == "masked-runtime")
    {
        return ServiceStartType::Disabled;
    }
    if (unitFileState == "disabled" || unitFileState == "static" || unitFileState == "indirect" || unitFileState == "generated" ||
        unitFileState == "transient" || unitFileState == "alias" || unitFileState == "linked" || unitFileState == "linked-runtime")
    {
        return ServiceStartType::Manual;
    }
    return ServiceStartType::Unknown;
}

/// The services in @p listing, sorted by name: every loaded service with its state, enablement and main
/// process, then every installed service file not loaded, as Stopped. Alias files aren't services of
/// their own: the unit they name is listed under its real name.
[[nodiscard]] inline std::vector<ServiceInfo> services(const Listing& listing)
{
    std::map<std::string, std::string, std::less<>> fileStates;
    for (const auto& [name, state] : listing.unitFiles)
    {
        fileStates.emplace(name, state);
    }
    const auto startTypeOf = [&fileStates, &listing](const std::string& name)
    {
        // An instance's own state first (#2425), then its file's, then its template's.
        if (const auto own = listing.instanceFileStates.find(name); own != listing.instanceFileStates.end())
        {
            if (const ServiceStartType type = startTypeFrom(own->second); type != ServiceStartType::Unknown)
            {
                return type;
            }
        }
        auto it = fileStates.find(name);
        if (it == fileStates.end())
        {
            it = fileStates.find(templateOf(name));
        }
        return it == fileStates.end() ? ServiceStartType::Unknown : startTypeFrom(it->second);
    };

    std::vector<ServiceInfo> result;
    std::set<std::string, std::less<>> listed;
    for (const Unit& unit : listing.units)
    {
        if (!isService(unit.name) || !listed.insert(unit.name).second)
        {
            continue;
        }
        ServiceInfo info;
        info.name = unit.name;
        info.displayName = unit.description.empty() ? unit.name : unit.description;
        info.state = stateFrom(unit.activeState);
        info.startType = startTypeOf(unit.name);
        if (const auto pid = listing.mainPids.find(unit.name); pid != listing.mainPids.end())
        {
            info.pid = pid->second;
        }
        result.push_back(std::move(info));
    }
    for (const auto& [name, state] : listing.unitFiles)
    {
        // An alias (syslog.service -> rsyslog.service) is another name for a unit listed under its own:
        // ListUnits never reports alias names, so it would show as a second, always-Stopped row (#2424).
        if (!isService(name) || state == "alias" || listed.contains(name))
        {
            continue;
        }
        listed.insert(name);
        ServiceInfo info;
        info.name = name;
        info.displayName = name; // Not loaded: systemd hasn't read its Description
        info.state = ServiceState::Stopped;
        info.startType = startTypeFrom(state);
        result.push_back(std::move(info));
    }
    std::ranges::sort(result, {}, &ServiceInfo::name);
    return result;
}

} // namespace Platform::SystemdServices
