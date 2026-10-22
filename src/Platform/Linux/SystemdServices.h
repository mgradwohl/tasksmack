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
//
// A loaded service's User, ExecStart and Type (#2590) fill the Account, Command and Type the Windows
// list has; a unit file that isn't loaded has no object to ask, so those are left empty.

#include "Platform/IServiceProbe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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
    std::string objectPath;  ///< The unit's object, for its MainPID and configuration
};

/// A loaded service's configuration (#2590): the org.freedesktop.systemd1.Service properties User,
/// ExecStart and Type, raw. A property that couldn't be read is left empty (user: nullopt).
struct UnitConfig
{
    std::optional<std::string> user;                 ///< "www-data"; empty when the unit runs as root
    bool dynamicUser = false;                        ///< DynamicUser=yes: without User=, a user named after the unit
    std::vector<std::vector<std::string>> execStart; ///< Each ExecStart= line's argv
    std::string type;                                ///< "simple", "exec", "forking", "oneshot", "dbus", "notify"...

    friend bool operator==(const UnitConfig&, const UnitConfig&) = default;
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
    std::map<std::string, UnitConfig> configs; ///< A loaded service's User, ExecStart and Type, by unit name (#2590)
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

/// The account the service @p name runs as, as the Account column shows it (#2590): its User; when User
/// was read and is empty, the unit's name without ".service" for a DynamicUser= service (systemd names
/// the user it allocates so), else "root" -- systemd runs a system service without User= as root, as
/// Windows' Account shows "LocalSystem". Empty when User couldn't be read.
[[nodiscard]] inline std::string accountFrom(std::string_view name, const UnitConfig& config)
{
    if (!config.user.has_value())
    {
        return {};
    }
    if (!config.user->empty())
    {
        return *config.user;
    }
    if (config.dynamicUser && name.ends_with(SERVICE_SUFFIX))
    {
        return std::string(name.substr(0, name.size() - SERVICE_SUFFIX.size()));
    }
    return "root";
}

/// The command a service runs, as the Command line shows it (#2590): each ExecStart= line's argv joined
/// with spaces, as Windows shows the program and its arguments; a service with several (a oneshot may
/// have more than one) has them in order, separated by "; ". Empty without any.
[[nodiscard]] inline std::string commandLineFrom(const std::vector<std::vector<std::string>>& execStart)
{
    std::string text;
    for (const std::vector<std::string>& argv : execStart)
    {
        if (argv.empty())
        {
            continue;
        }
        if (!text.empty())
        {
            text += "; ";
        }
        for (std::size_t i = 0; i < argv.size(); ++i)
        {
            if (i > 0)
            {
                text += ' ';
            }
            text += argv[i];
        }
    }
    return text;
}

/// A service's Type= as a short label, capitalized as Windows' type labels are ("Own process") (#2590):
/// "simple" -> "Simple", "dbus" -> "D-Bus", "notify-reload" -> "Notify (reload)". A type this doesn't
/// know is shown as systemd gave it; an empty one stays empty.
[[nodiscard]] inline std::string serviceTypeText(std::string_view type)
{
    if (type == "dbus")
    {
        return "D-Bus";
    }
    if (type == "notify-reload")
    {
        return "Notify (reload)";
    }
    if (type == "simple" || type == "exec" || type == "forking" || type == "oneshot" || type == "notify" || type == "idle")
    {
        std::string text(type);
        text.front() = static_cast<char>(text.front() - 'a' + 'A');
        return text;
    }
    return std::string(type);
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
        if (const auto config = listing.configs.find(unit.name); config != listing.configs.end())
        {
            info.account = accountFrom(unit.name, config->second);
            info.binaryPath = commandLineFrom(config->second.execStart);
            info.serviceType = serviceTypeText(config->second.type);
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
