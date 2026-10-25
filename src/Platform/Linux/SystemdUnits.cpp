// systemd over D-Bus: its units (services, start/stop, start types) and the Manager's boot timestamps (#2006, #1525). Split out of
// SystemdBus.cpp (#2171).

#include "Platform/IServiceActions.h"
#include "Platform/Linux/SystemdActions.h"
#include "Platform/Linux/SystemdBus.h"
#include "Platform/Linux/SystemdServices.h"

#include <cstdint>
#include <optional>
#include <string>

#ifdef TASKSMACK_HAS_SDBUS
#include "Platform/IServiceProbe.h"
#include "Platform/Linux/SdBusCall.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <map>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
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

namespace
{

constexpr const char* SYSTEMD_UNIT = "org.freedesktop.systemd1.Unit";
constexpr const char* SYSTEMD_JOB = "org.freedesktop.systemd1.Job";

/// A failed call on @p bus as a ServiceActionResult, in the user's terms (SystemdActions::errorText()).
/// Whether @p bus allowed a password prompt decides how a polkit refusal reads, and whether a retry
/// that allows one is worth offering (#2304).
[[nodiscard]] ServiceActionResult actionFailure(sd_bus* bus, const BusError& error, int result)
{
    const sd_bus_error& raw = error.raw();
    const std::string_view name = raw.name != nullptr ? std::string_view(raw.name) : std::string_view{};
    const std::string message = (raw.message != nullptr)
                                  ? std::string(raw.message)
                                  : std::string(std::strerror(-result)); // NOLINT(concurrency-mt-unsafe) - one worker thread
    const bool interactive = sd_bus_get_allow_interactive_authorization(bus) > 0;
    ServiceActionResult failure = ServiceActionResult::failed(SystemdActions::errorText(name, message, timedOut(raw, result), interactive));
    const bool authorizationRequired = name == SystemdActions::INTERACTIVE_AUTHORIZATION_REQUIRED;
    failure.needsAuthorization = authorizationRequired && !interactive;
    failure.authorizationUnavailable = authorizationRequired && interactive;
    return failure;
}

/// The system bus for an action, or why not. Unless @p allowInteractive, polkit may never prompt:
/// unprivileged, the call is refused with InteractiveAuthorizationRequired, which reads as "Requires
/// administrator" (#2006). @p allowInteractive is the user's "Authenticate and retry" (#2304), the one
/// case where polkit's agent may ask for a password.
[[nodiscard]] std::optional<std::string> openForAction(Bus& bus, bool allowInteractive)
{
    if (const int result = openSystemBus(bus); result < 0)
    {
        return std::string("The system bus couldn't be reached: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe)
    }
    (void) sd_bus_set_allow_interactive_authorization(bus.get(), allowInteractive ? 1 : 0);
    return std::nullopt;
}

/// How often callAction() looks at its stop token while it waits (#2402).
constexpr std::uint64_t ACTION_STOP_POLL_USEC = 50'000;

/// What an action call's answer handed callAction().
struct PendingReply
{
    bool done = false;
    int result = 0;
    BusError* error = nullptr;
    Message* reply = nullptr;
};

int onActionReply(sd_bus_message* message, void* userdata, sd_bus_error* /*retError*/)
{
    auto* pending = static_cast<PendingReply*>(userdata);
    pending->done = true;
    if (const sd_bus_error* error = sd_bus_message_get_error(message); error != nullptr)
    {
        (void) sd_bus_error_copy(pending->error->get(), error);
        const int errorNumber = sd_bus_message_get_errno(message);
        pending->result = errorNumber > 0 ? -errorNumber : -EIO;
        return 0;
    }
    *pending->reply->out() = sd_bus_message_ref(message);
    pending->result = 1;
    return 0;
}

/// Sends @p call on an action's @p bus and waits for its answer, as sd_bus_call() does, or for
/// @p stopToken: -ECANCELED then (#2402). A bus allowed to prompt gets actionCallTimeoutUsec()'s long
/// bound, since systemd answers only once the user has answered polkit's agent; the wait goes in short
/// slices so closing the panel or exiting still ends it at once, as runJob()'s own wait does.
[[nodiscard]] int callAction(sd_bus* bus, sd_bus_message* call, const std::stop_token& stopToken, BusError& error, Message& reply)
{
    PendingReply pending{.error = &error, .reply = &reply};
    sd_bus_slot* slot = nullptr;
    const std::uint64_t timeout = actionCallTimeoutUsec(sd_bus_get_allow_interactive_authorization(bus) > 0);
    if (const int result = sd_bus_call_async(bus, &slot, call, onActionReply, &pending, timeout); result < 0)
    {
        return result;
    }
    int failure = 0;
    while (!pending.done && failure == 0)
    {
        if (stopToken.stop_requested())
        {
            failure = -ECANCELED;
            break;
        }
        const int processed = sd_bus_process(bus, nullptr);
        if (processed < 0)
        {
            failure = processed;
        }
        else if (processed == 0)
        {
            if (const int waited = sd_bus_wait(bus, ACTION_STOP_POLL_USEC); waited < 0 && waited != -EINTR)
            {
                failure = waited;
            }
        }
    }
    (void) sd_bus_slot_unref(slot); // no answer may reach `pending` once it is gone
    return pending.done ? pending.result : failure;
}

/// Manager.@p method's call message on @p bus, into @p call.
[[nodiscard]] int newManagerCall(sd_bus* bus, Message& call, const char* method)
{
    return sd_bus_message_new_method_call(bus, call.out(), SYSTEMD, SYSTEMD_PATH, SYSTEMD_MANAGER, method);
}

/// @p name's unit object (Manager.LoadUnit, which loads an installed unit that isn't), into @p path.
[[nodiscard]] std::optional<ServiceActionResult> unitPath(sd_bus* bus, std::string_view name, std::string& path)
{
    BusError error;
    Message reply;
    const std::string unit(name);
    if (const int result =
            sd_bus_call_method(bus, SYSTEMD, SYSTEMD_PATH, SYSTEMD_MANAGER, "LoadUnit", error.get(), reply.out(), "s", unit.c_str());
        result < 0)
    {
        return actionFailure(bus, error, result);
    }
    const char* object = nullptr;
    if (const int result = sd_bus_message_read(reply.get(), "o", &object); result < 0 || object == nullptr)
    {
        return ServiceActionResult::failed("systemd's answer couldn't be read");
    }
    path = object;
    return std::nullopt;
}

/// A unit's ActiveState, or empty when it can't be read.
[[nodiscard]] std::string activeStateOf(sd_bus* bus, const std::string& unitPath)
{
    BusError error;
    char* state = nullptr;
    if (sd_bus_get_property_string(bus, SYSTEMD, unitPath.c_str(), SYSTEMD_UNIT, "ActiveState", error.get(), &state) < 0 ||
        state == nullptr)
    {
        return {};
    }
    std::string text(state);
    std::free(state); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - sd-bus hands back malloc'd strings
    return text;
}

/// A string property of @p unitPath's Service interface (Type, Result); empty when it can't be read.
[[nodiscard]] std::string serviceStringOf(sd_bus* bus, const std::string& unitPath, const char* property)
{
    BusError error;
    char* value = nullptr;
    std::string text;
    if (sd_bus_get_property_string(bus, SYSTEMD, unitPath.c_str(), SYSTEMD_SERVICE, property, error.get(), &value) >= 0 && value != nullptr)
    {
        text = value;
    }
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - sd-bus hands back malloc'd strings
    return text;
}

/// When @p unitPath's service main process last started (ExecMainStartTimestampMonotonic, in µs); 0 when
/// it never has or can't be read.
[[nodiscard]] std::uint64_t execMainStartOf(sd_bus* bus, const std::string& unitPath)
{
    BusError error;
    std::uint64_t started = 0;
    if (sd_bus_get_property_trivial(
            bus, SYSTEMD, unitPath.c_str(), SYSTEMD_SERVICE, "ExecMainStartTimestampMonotonic", error.get(), 't', &started) < 0)
    {
        return 0;
    }
    return started;
}

/// Whether the job at @p jobPath has finished: systemd removes a job's object once it is done.
[[nodiscard]] bool jobGone(sd_bus* bus, const std::string& jobPath)
{
    BusError error;
    char* state = nullptr;
    const int result = sd_bus_get_property_string(bus, SYSTEMD, jobPath.c_str(), SYSTEMD_JOB, "State", error.get(), &state);
    std::free(state); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - as above
    return result < 0;
}

/// Sends Manager.@p method (StartUnit or StopUnit) for @p name in "replace" mode and waits for @p goal.
[[nodiscard]] ServiceActionResult runJob(sd_bus* bus,
                                         std::string_view name,
                                         const std::string& unitObject,
                                         const char* method,
                                         SystemdActions::Goal goal,
                                         const std::stop_token& stopToken,
                                         UnitWait wait)
{
    if (stopToken.stop_requested())
    {
        return ServiceActionResult::stopRequested();
    }
    // A oneshot service is back to inactive once it has run: when its main process last started tells
    // a run apart from a start that never happened (#2123).
    const std::uint64_t execStartBefore = goal == SystemdActions::Goal::Running ? execMainStartOf(bus, unitObject) : 0;
    std::string jobPath;
    {
        BusError error;
        Message call;
        Message reply;
        const std::string unit(name);
        int result = newManagerCall(bus, call, method);
        if (result >= 0)
        {
            result = sd_bus_message_append(call.get(), "ss", unit.c_str(), "replace");
        }
        if (result >= 0)
        {
            result = callAction(bus, call.get(), stopToken, error, reply);
        }
        if (result == -ECANCELED && stopToken.stop_requested())
        {
            return ServiceActionResult::stopRequested();
        }
        if (result < 0)
        {
            return actionFailure(bus, error, result);
        }
        const char* job = nullptr;
        if (sd_bus_message_read(reply.get(), "o", &job) >= 0 && job != nullptr)
        {
            jobPath = job;
        }
    }
    const auto giveUpAt = std::chrono::steady_clock::now() + wait.timeout;
    for (;;)
    {
        const std::string state = activeStateOf(bus, unitObject);
        const bool gone = jobPath.empty() || jobGone(bus, jobPath);
        switch (SystemdActions::waitStep(goal, state, gone))
        {
        case SystemdActions::WaitStep::Done:
            return ServiceActionResult::succeeded();
        case SystemdActions::WaitStep::Failed:
            if (goal == SystemdActions::Goal::Running && state == "inactive" &&
                SystemdActions::oneshotRanSuccessfully(serviceStringOf(bus, unitObject, "Type"),
                                                       serviceStringOf(bus, unitObject, "Result"),
                                                       execStartBefore,
                                                       execMainStartOf(bus, unitObject)))
            {
                return ServiceActionResult::succeeded();
            }
            return ServiceActionResult::failed(goal == SystemdActions::Goal::Running
                                                   ? "It didn't start (" + (state.empty() ? std::string("state unknown") : state) + ")"
                                                   : "It didn't stop (" + (state.empty() ? std::string("state unknown") : state) + ")");
        case SystemdActions::WaitStep::Wait:
            break;
        }
        if (stopToken.stop_requested())
        {
            return ServiceActionResult::stopRequested();
        }
        if (std::chrono::steady_clock::now() >= giveUpAt)
        {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(wait.timeout).count();
            return ServiceActionResult::failed(goal == SystemdActions::Goal::Running ? std::format("It didn't start within {} s", seconds)
                                                                                     : std::format("It didn't stop within {} s", seconds));
        }
        std::this_thread::sleep_for(wait.poll);
    }
}

/// The running units a stop of @p unitObject would take down: those that Requires= it (RequiredBy),
/// BindsTo= it (BoundBy), are PartOf= it (ConsistsOf) or are Requisite= of it (RequisiteOf, #2124).
[[nodiscard]] std::vector<std::string> runningDependents(sd_bus* bus, const std::string& unitObject)
{
    std::vector<std::string> running;
    for (const char* property : {"RequiredBy", "BoundBy", "ConsistsOf", "RequisiteOf"})
    {
        BusError error;
        char** names = nullptr;
        if (sd_bus_get_property_strv(bus, SYSTEMD, unitObject.c_str(), SYSTEMD_UNIT, property, error.get(), &names) < 0 || names == nullptr)
        {
            continue;
        }
        for (char* const* entry = names; *entry != nullptr; ++entry) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - a C strv
        {
            const std::string dependent(*entry);
            std::free(*entry); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - strv entries are malloc'd
            std::string dependentObject;
            if (!unitPath(bus, dependent, dependentObject).has_value() && SystemdActions::isRunning(activeStateOf(bus, dependentObject)) &&
                std::ranges::find(running, dependent) == running.end())
            {
                running.push_back(dependent);
            }
        }
        std::free(static_cast<void*>(names)); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - as above
    }
    return running;
}

/// Manager.@p method over the one unit file @p name: UnmaskUnitFiles/DisableUnitFiles (as, b runtime),
/// MaskUnitFiles (as, b runtime, b force) or EnableUnitFiles (as, b runtime, b force). The changes it
/// answers with aren't needed, but EnableUnitFiles' carries_install_info is: false means the unit file
/// has no [Install] section, so nothing was enabled and it would never start at boot (#2146).
[[nodiscard]] std::optional<ServiceActionResult>
changeUnitFile(sd_bus* bus, const char* method, std::string_view name, bool withForce, const std::stop_token& stopToken)
{
    BusError error;
    Message call;
    Message reply;
    const std::string unit(name);
    int result = newManagerCall(bus, call, method);
    if (result >= 0)
    {
        result = sd_bus_message_append(call.get(), "as", 1, unit.c_str());
    }
    if (result >= 0)
    {
        result = withForce ? sd_bus_message_append(call.get(), "bb", 0, 0) : sd_bus_message_append(call.get(), "b", 0);
    }
    if (result >= 0)
    {
        result = callAction(bus, call.get(), stopToken, error, reply);
    }
    if (result == -ECANCELED && stopToken.stop_requested())
    {
        return ServiceActionResult::stopRequested();
    }
    if (result < 0)
    {
        return actionFailure(bus, error, result);
    }
    if (std::string_view(method) == "EnableUnitFiles")
    {
        int carriesInstallInfo = 1;
        if (sd_bus_message_read(reply.get(), "b", &carriesInstallInfo) >= 0 && carriesInstallInfo == 0)
        {
            return ServiceActionResult::failed(SystemdActions::NO_INSTALL_SECTION_MESSAGE);
        }
    }
    return std::nullopt;
}

} // namespace

ServiceActionResult startUnit(std::string_view name, const std::stop_token& stopToken, UnitWait wait, bool allowInteractive)
{
    Bus bus;
    if (const auto failed = openForAction(bus, allowInteractive))
    {
        return ServiceActionResult::failed(*failed);
    }
    std::string unitObject;
    if (auto failed = unitPath(bus.get(), name, unitObject))
    {
        return std::move(*failed);
    }
    return runJob(bus.get(), name, unitObject, "StartUnit", SystemdActions::Goal::Running, stopToken, wait);
}

ServiceActionResult stopUnit(std::string_view name, const std::stop_token& stopToken, UnitWait wait, bool allowInteractive)
{
    Bus bus;
    if (const auto failed = openForAction(bus, allowInteractive))
    {
        return ServiceActionResult::failed(*failed);
    }
    std::string unitObject;
    if (auto failed = unitPath(bus.get(), name, unitObject))
    {
        return std::move(*failed);
    }
    // Never stop running dependents with it (#2006): systemd would take every unit that Requires= it.
    if (const std::vector<std::string> dependents = runningDependents(bus.get(), unitObject); !dependents.empty())
    {
        return ServiceActionResult::failed(SystemdActions::dependentsText(dependents));
    }
    return runJob(bus.get(), name, unitObject, "StopUnit", SystemdActions::Goal::Stopped, stopToken, wait);
}

ServiceActionResult
setUnitStartType(std::string_view name, ServiceStartType startType, bool allowInteractive, const std::stop_token& stopToken)
{
    const std::optional<SystemdActions::StartTypeSteps> steps = SystemdActions::startTypeSteps(startType);
    if (!steps.has_value())
    {
        return ServiceActionResult::failed("systemd services have no such startup type");
    }
    Bus bus;
    if (const auto failed = openForAction(bus, allowInteractive))
    {
        return ServiceActionResult::failed(*failed);
    }
    if (steps->unmaskFirst)
    {
        if (auto failed = changeUnitFile(bus.get(), "UnmaskUnitFiles", name, false, stopToken))
        {
            return std::move(*failed);
        }
    }
    const char* method = "DisableUnitFiles";
    bool withForce = false;
    switch (steps->change)
    {
    case SystemdActions::FileChange::Enable:
        method = "EnableUnitFiles";
        withForce = true;
        break;
    case SystemdActions::FileChange::Mask:
        method = "MaskUnitFiles";
        withForce = true;
        break;
    case SystemdActions::FileChange::Disable:
        break;
    }
    // As systemctl does after changing unit files, so systemd sees the new state.
    const auto reload = [&bus, &stopToken] -> ServiceActionResult
    {
        BusError error;
        Message call;
        Message reply;
        int result = newManagerCall(bus.get(), call, "Reload");
        if (result >= 0)
        {
            result = callAction(bus.get(), call.get(), stopToken, error, reply);
        }
        if (result == -ECANCELED && stopToken.stop_requested())
        {
            return ServiceActionResult::stopRequested();
        }
        if (result < 0)
        {
            return actionFailure(bus.get(), error, result);
        }
        return ServiceActionResult::succeeded();
    };
    if (auto failed = changeUnitFile(bus.get(), method, name, withForce, stopToken))
    {
        // The unmask already changed the unit files (#2426): systemd is still told, or it keeps the unit
        // masked in memory while the list reads it from disk. The step's own failure is what's reported.
        if (steps->unmaskFirst && !failed->cancelled)
        {
            (void) reload();
        }
        return std::move(*failed);
    }
    return reload();
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

ServiceActionResult startUnit(std::string_view /*name*/, const std::stop_token& /*stopToken*/, UnitWait /*wait*/, bool /*allowInteractive*/)
{
    return ServiceActionResult::failed("This build has no systemd support (it was built without libsystemd)");
}

ServiceActionResult stopUnit(std::string_view /*name*/, const std::stop_token& /*stopToken*/, UnitWait /*wait*/, bool /*allowInteractive*/)
{
    return ServiceActionResult::failed("This build has no systemd support (it was built without libsystemd)");
}

ServiceActionResult
setUnitStartType(std::string_view /*name*/, ServiceStartType /*startType*/, bool /*allowInteractive*/, const std::stop_token& /*stopToken*/)
{
    return ServiceActionResult::failed("This build has no systemd support (it was built without libsystemd)");
}

#endif

} // namespace Platform::SystemdBus
