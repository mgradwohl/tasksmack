// systemd over D-Bus: its units (the services and their unit files) and the Manager's boot timestamps (#2006, #1525). Split
// out of SystemdBus.cpp (#2171); starting, stopping and start types are in SystemdUnitActions.cpp (#2728).

#include "Platform/Linux/SystemdBus.h"
#include "Platform/Linux/SystemdServices.h"

#include <cstdint>
#include <optional>
#include <string>

#ifdef TASKSMACK_HAS_SDBUS
#include "Platform/IServiceProbe.h"
#include "Platform/Linux/SdBusCall.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

// Newer libsystemd (259 here) declares sd_bus and sd_bus_error in sd-bus-protocol.h; older releases
// (Ubuntu 24.04's, in CI) declare them in sd-bus.h, so include-cleaner wants this line on one and not the other.
#include <systemd/sd-bus-protocol.h> // NOLINT(misc-include-cleaner) - see above
#include <systemd/sd-bus.h>
#endif

namespace Platform::SystemdBus
{

bool built() noexcept
{
#ifdef TASKSMACK_HAS_SDBUS
    return true;
#else
    return false;
#endif
}

#ifdef TASKSMACK_HAS_SDBUS

using Detail::Bus;
using Detail::BusError;
using Detail::Message;
using Detail::openSystemBus;
using Detail::timedOut;

namespace
{

/// The Manager's unsigned 64-bit property @p name into @p value; the error text when it fails.
[[nodiscard]] std::string readManagerUint64(sd_bus* bus, const char* name, std::uint64_t& value)
{
    sd_bus_error error{}; // SD_BUS_ERROR_NULL: all null, written as C++ (the macro is a C compound literal)
    const int result = sd_bus_get_property_trivial(
        bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", name, &error, 't', &value);
    std::string text;
    if (result < 0 && timedOut(error, result))
    {
        text = "systemd didn't answer in time";
    }
    else if (result < 0)
    {
        text = (error.message != nullptr) ? error.message
                                          : std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - read on one worker thread
    }
    sd_bus_error_free(&error);
    return text;
}

} // namespace

BootTimestampsRead readBootTimestamps()
{
    BootTimestampsRead read;
    Bus bus;
    if (const int result = openSystemBus(bus); result < 0)
    {
        read.error =
            std::string("The system bus couldn't be reached: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - as above
        return read;
    }
    struct Property
    {
        const char* name;
        std::uint64_t* value;
    };
    const std::array properties{
        Property{.name = "FirmwareTimestampMonotonic", .value = &read.firmware},
        Property{.name = "LoaderTimestampMonotonic", .value = &read.loader},
        Property{.name = "InitRDTimestampMonotonic", .value = &read.initrd},
        Property{.name = "UserspaceTimestampMonotonic", .value = &read.userspace},
        Property{.name = "FinishTimestampMonotonic", .value = &read.finish},
    };
    for (const Property& property : properties)
    {
        if (std::string error = readManagerUint64(bus.get(), property.name, *property.value); !error.empty())
        {
            read.error = std::move(error);
            return read;
        }
    }
    read.ok = true;
    return read;
}

namespace
{

constexpr const char* SYSTEMD = "org.freedesktop.systemd1";
constexpr const char* SYSTEMD_PATH = "/org/freedesktop/systemd1";
constexpr const char* SYSTEMD_MANAGER = "org.freedesktop.systemd1.Manager";
constexpr const char* SYSTEMD_SERVICE = "org.freedesktop.systemd1.Service";

/// Why systemd couldn't answer, in the user's terms.
[[nodiscard]] std::string systemdError(const BusError& error, int result)
{
    if (timedOut(error.raw(), result))
    {
        return "systemd didn't answer in time";
    }
    if (error.is("org.freedesktop.DBus.Error.ServiceUnknown") || error.is("org.freedesktop.DBus.Error.NameHasNoOwner"))
    {
        return "systemd isn't running";
    }
    return "systemd couldn't be asked: " + error.text(result);
}

/// The loaded services from ListUnits' a(ssssssouso) reply into @p units; a negative sd-bus result when it is malformed.
[[nodiscard]] int readUnits(sd_bus_message* reply, std::vector<SystemdServices::Unit>& units)
{
    int result = sd_bus_message_enter_container(reply, 'a', "(ssssssouso)");
    if (result < 0)
    {
        return result;
    }
    for (;;)
    {
        const char* name = nullptr;
        const char* description = nullptr;
        const char* loadState = nullptr;
        const char* activeState = nullptr;
        const char* subState = nullptr;
        const char* following = nullptr;
        const char* path = nullptr;
        std::uint32_t jobId = 0;
        const char* jobType = nullptr;
        const char* jobPath = nullptr;
        result = sd_bus_message_read(
            reply, "(ssssssouso)", &name, &description, &loadState, &activeState, &subState, &following, &path, &jobId, &jobType, &jobPath);
        if (result <= 0)
        {
            break;
        }
        if (name != nullptr && SystemdServices::isService(name))
        {
            units.push_back({.name = name,
                             .description = (description != nullptr) ? description : "",
                             .loadState = (loadState != nullptr) ? loadState : "",
                             .activeState = (activeState != nullptr) ? activeState : "",
                             .subState = (subState != nullptr) ? subState : "",
                             .objectPath = (path != nullptr) ? path : ""});
        }
    }
    if (result < 0)
    {
        return result;
    }
    return sd_bus_message_exit_container(reply);
}

/// The installed service files from ListUnitFiles' a(ss) reply (path, state) into @p files, by file name.
[[nodiscard]] int readUnitFiles(sd_bus_message* reply, std::vector<std::pair<std::string, std::string>>& files)
{
    int result = sd_bus_message_enter_container(reply, 'a', "(ss)");
    if (result < 0)
    {
        return result;
    }
    for (;;)
    {
        const char* path = nullptr;
        const char* state = nullptr;
        result = sd_bus_message_read(reply, "(ss)", &path, &state);
        if (result <= 0)
        {
            break;
        }
        const std::string_view full = (path != nullptr) ? path : "";
        const std::string_view name = full.substr(full.rfind('/') == std::string_view::npos ? 0 : full.rfind('/') + 1);
        // Templates ("getty@.service") too: their instances take their enablement (SystemdServices::services()).
        if (name.ends_with(SystemdServices::SERVICE_SUFFIX))
        {
            files.emplace_back(std::string(name), (state != nullptr) ? state : "");
        }
    }
    if (result < 0)
    {
        return result;
    }
    return sd_bus_message_exit_container(reply);
}

/// One of a service's string properties (User, Type) into @p value: false only when systemd didn't
/// answer in time. Any other failure (an unknown property, say) leaves @p value empty.
[[nodiscard]] bool readServiceString(sd_bus* bus, const char* object, const char* name, std::optional<std::string>& value)
{
    BusError error;
    char* text = nullptr;
    const int result = sd_bus_get_property_string(bus, SYSTEMD, object, SYSTEMD_SERVICE, name, error.get(), &text);
    if (result >= 0)
    {
        value = (text != nullptr) ? std::string(text) : std::string{};
    }
    std::free(text); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - sd-bus hands back malloc'd strings
    return result >= 0 || !timedOut(error.raw(), result);
}

/// The service's ExecStart, a(sasbttttuii) (#2590): each ExecStart= line's binary path, argv,
/// ignore-errors flag, start and exit times, PID and exit status. Each line's argv into @p commands
/// (its path, when argv is empty); the rest is skipped. False only when systemd didn't answer in time;
/// any other failure, a malformed reply included, leaves @p commands as it was.
[[nodiscard]] bool readExecStart(sd_bus* bus, const char* object, std::vector<std::vector<std::string>>& commands)
{
    BusError error;
    Message reply;
    int result = sd_bus_get_property(bus, SYSTEMD, object, SYSTEMD_SERVICE, "ExecStart", error.get(), reply.out(), "a(sasbttttuii)");
    if (result < 0)
    {
        return !timedOut(error.raw(), result);
    }
    std::vector<std::vector<std::string>> lines;
    result = sd_bus_message_enter_container(reply.get(), 'a', "(sasbttttuii)");
    while (result >= 0 && (result = sd_bus_message_enter_container(reply.get(), 'r', "sasbttttuii")) > 0)
    {
        const char* path = nullptr;
        std::vector<std::string> argv;
        result = sd_bus_message_read(reply.get(), "s", &path);
        if (result >= 0)
        {
            result = sd_bus_message_enter_container(reply.get(), 'a', "s");
        }
        while (result >= 0)
        {
            const char* argument = nullptr;
            result = sd_bus_message_read(reply.get(), "s", &argument);
            if (result <= 0)
            {
                break;
            }
            argv.emplace_back(argument != nullptr ? argument : "");
        }
        if (result >= 0)
        {
            result = sd_bus_message_exit_container(reply.get()); // the argv
        }
        if (result >= 0)
        {
            result = sd_bus_message_skip(reply.get(), "bttttuii");
        }
        if (result >= 0)
        {
            result = sd_bus_message_exit_container(reply.get()); // the line
        }
        if (result >= 0)
        {
            if (argv.empty() && path != nullptr && *path != '\0')
            {
                argv.emplace_back(path);
            }
            lines.push_back(std::move(argv));
        }
    }
    if (result >= 0)
    {
        result = sd_bus_message_exit_container(reply.get());
    }
    if (result >= 0)
    {
        commands = std::move(lines);
    }
    return true;
}

/// Whether the service runs as a user systemd allocates for it (DynamicUser=yes) into @p dynamic: false
/// only when systemd didn't answer in time.
[[nodiscard]] bool readDynamicUser(sd_bus* bus, const char* object, bool& dynamic)
{
    BusError error;
    int value = 0;
    const int result = sd_bus_get_property_trivial(bus, SYSTEMD, object, SYSTEMD_SERVICE, "DynamicUser", error.get(), 'b', &value);
    dynamic = result >= 0 && value != 0;
    return result >= 0 || !timedOut(error.raw(), result);
}

/// A loaded service's User, ExecStart and Type into @p config (#2590): three property reads, and a
/// fourth, DynamicUser, only when User is empty. False when systemd stopped answering partway; a
/// property that couldn't be read otherwise is left empty.
[[nodiscard]] bool readUnitConfig(sd_bus* bus, const char* object, SystemdServices::UnitConfig& config)
{
    if (!readServiceString(bus, object, "User", config.user))
    {
        return false;
    }
    if (config.user.has_value() && config.user->empty() && !readDynamicUser(bus, object, config.dynamicUser))
    {
        return false;
    }
    std::optional<std::string> type;
    const bool answered = readExecStart(bus, object, config.execStart) && readServiceString(bus, object, "Type", type);
    config.type = type.value_or(std::string{});
    return answered;
}

} // namespace

ServicesRead readServices(UnitFileCache* cache, std::chrono::steady_clock::time_point now)
{
    ServicesRead read;
    Bus bus;
    if (const int result = openSystemBus(bus); result < 0)
    {
        read.error = std::string("The system bus couldn't be reached: ") +
                     std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - one worker thread
        return read;
    }
    {
        BusError error;
        Message reply;
        int result = sd_bus_call_method(bus.get(), SYSTEMD, SYSTEMD_PATH, SYSTEMD_MANAGER, "ListUnits", error.get(), reply.out(), "");
        if (result < 0)
        {
            read.error = systemdError(error, result);
            return read;
        }
        if (result = readUnits(reply.get(), read.listing.units); result < 0)
        {
            read.error = std::string("systemd's unit list couldn't be read: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe)
            return read;
        }
    }
    if (cache != nullptr && cache->readAt.has_value() && now - *cache->readAt < UNIT_FILES_TTL)
    {
        read.listing.unitFiles = cache->unitFiles;
    }
    else
    {
        BusError error;
        Message reply;
        int result = sd_bus_call_method(bus.get(), SYSTEMD, SYSTEMD_PATH, SYSTEMD_MANAGER, "ListUnitFiles", error.get(), reply.out(), "");
        if (result < 0)
        {
            read.error = systemdError(error, result);
            return read;
        }
        if (result = readUnitFiles(reply.get(), read.listing.unitFiles); result < 0)
        {
            read.error = std::string("systemd's unit files couldn't be read: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe)
            return read;
        }
        if (cache != nullptr)
        {
            cache->unitFiles = read.listing.unitFiles;
            cache->readAt = now;
        }
    }
    // False once a property read below times out: systemd has stopped answering, so the rest of this call
    // asks it nothing more, whichever loop it was in. A stalled systemd costs one call timeout per call
    // (#2125, #2773), not one per loop.
    bool answering = true;
    // Each loaded template instance's own enablement (#2425): ListUnitFiles has only its template's, which
    // an instance needn't share. One property per instance, and there are few.
    for (const SystemdServices::Unit& unit : read.listing.units)
    {
        if (unit.objectPath.empty() || SystemdServices::templateOf(unit.name) == unit.name)
        {
            continue;
        }
        BusError error;
        char* state = nullptr;
        const int result = sd_bus_get_property_string(
            bus.get(), SYSTEMD, unit.objectPath.c_str(), "org.freedesktop.systemd1.Unit", "UnitFileState", error.get(), &state);
        std::string text = result >= 0 && state != nullptr ? std::string(state) : std::string{};
        std::free(state); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - sd-bus hands back malloc'd strings
        if (result < 0 && timedOut(error.raw(), result))
        {
            answering = false;
            break; // the template's state for the rest, as before
        }
        if (!text.empty())
        {
            read.listing.instanceFileStates.emplace(unit.name, std::move(text));
        }
    }
    // Each running service's main process: one property per service, so only for those that have one.
    for (const SystemdServices::Unit& unit : read.listing.units)
    {
        if (!answering)
        {
            break; // systemd stopped answering above: the list without PIDs
        }
        if (unit.objectPath.empty() || SystemdServices::stateFrom(unit.activeState) == ServiceState::Stopped)
        {
            continue;
        }
        BusError error;
        std::uint32_t pid = 0;
        const int result =
            sd_bus_get_property_trivial(bus.get(), SYSTEMD, unit.objectPath.c_str(), SYSTEMD_SERVICE, "MainPID", error.get(), 'u', &pid);
        if (result < 0 && timedOut(error.raw(), result))
        {
            answering = false;
            break; // the list without the rest of the PIDs, not a call timeout per service
        }
        if (result >= 0 && pid != 0)
        {
            read.listing.mainPids.emplace(unit.name, pid);
        }
    }
    // Each loaded service's User, ExecStart and Type (#2590): three or four properties per service, so
    // with a cache a service's are read again only when its state or main process changed or
    // UNIT_CONFIG_TTL passed -- once warm, a refresh asks only about the services that started, stopped
    // or restarted. After a timeout here or above, each is what was read before: none is read again in a
    // call whose MainPIDs are missing, which would cache them as having no main process.
    for (const SystemdServices::Unit& unit : read.listing.units)
    {
        // A unit that isn't loaded -- referenced but not installed (not-found), masked, or failing to
        // parse -- still answers the Service interface, with an empty User, Type and ExecStart that would
        // read as "runs as root" (#2750). It has no configuration to show, so none is asked for.
        if (unit.objectPath.empty() || unit.loadState != "loaded")
        {
            continue;
        }
        const auto pid = read.listing.mainPids.find(unit.name);
        const std::uint32_t mainPid = pid != read.listing.mainPids.end() ? pid->second : 0;
        CachedUnitConfig* cached = nullptr;
        if (cache != nullptr)
        {
            if (const auto found = cache->configs.find(unit.name); found != cache->configs.end())
            {
                cached = &found->second;
            }
        }
        const bool fresh = cached != nullptr && cached->activeState == unit.activeState && cached->subState == unit.subState &&
                           cached->mainPid == mainPid && now - cached->readAt < UNIT_CONFIG_TTL;
        if (!fresh && answering)
        {
            SystemdServices::UnitConfig config;
            answering = readUnitConfig(bus.get(), unit.objectPath.c_str(), config);
            if (answering)
            {
                if (cache != nullptr)
                {
                    cache->configs.insert_or_assign(unit.name,
                                                    CachedUnitConfig{.config = config,
                                                                     .activeState = unit.activeState,
                                                                     .subState = unit.subState,
                                                                     .mainPid = mainPid,
                                                                     .readAt = now});
                }
                read.listing.configs.emplace(unit.name, std::move(config));
                continue;
            }
            // systemd isn't answering: what was read before for the rest, not a call timeout per service
        }
        if (cached != nullptr)
        {
            read.listing.configs.emplace(unit.name, cached->config);
        }
    }
    if (cache != nullptr)
    {
        // A service no longer loaded needs no configuration kept.
        std::erase_if(cache->configs, [&read](const auto& entry) { return !read.listing.configs.contains(entry.first); });
    }
    read.ok = true;
    return read;
}

#else

BootTimestampsRead readBootTimestamps()
{
    BootTimestampsRead read;
    read.error = "This build has no systemd support (it was built without libsystemd)";
    return read;
}

ServicesRead readServices(UnitFileCache* /*cache*/, std::chrono::steady_clock::time_point /*now*/)
{
    ServicesRead read;
    read.error = "This build has no systemd support (it was built without libsystemd)";
    return read;
}

#endif

} // namespace Platform::SystemdBus
