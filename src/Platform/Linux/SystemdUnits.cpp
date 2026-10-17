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
    // Each running service's main process: one property per service, so only for those that have one.
    for (const SystemdServices::Unit& unit : read.listing.units)
    {
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
            break; // systemd isn't answering: the list without the rest of the PIDs, not a call timeout per service
        }
        if (result >= 0 && pid != 0)
        {
            read.listing.mainPids.emplace(unit.name, pid);
        }
    }
    read.ok = true;
    return read;
}

namespace
{

constexpr const char* SYSTEMD_UNIT = "org.freedesktop.systemd1.Unit";
constexpr const char* SYSTEMD_JOB = "org.freedesktop.systemd1.Job";

/// A failed call as a ServiceActionResult, in the user's terms (SystemdActions::errorText()).
[[nodiscard]] ServiceActionResult actionFailure(const BusError& error, int result)
{
    const sd_bus_error& raw = error.raw();
    const std::string_view name = raw.name != nullptr ? std::string_view(raw.name) : std::string_view{};
    const std::string message = (raw.message != nullptr)
                                  ? std::string(raw.message)
                                  : std::string(std::strerror(-result)); // NOLINT(concurrency-mt-unsafe) - one worker thread
    return ServiceActionResult::failed(SystemdActions::errorText(name, message, timedOut(raw, result)));
}

/// The system bus for an action, or why not.
[[nodiscard]] std::optional<std::string> openForAction(Bus& bus)
{
    if (const int result = openSystemBus(bus); result < 0)
    {
        return std::string("The system bus couldn't be reached: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe)
    }
    // Never let polkit prompt: unprivileged, the call is refused with InteractiveAuthorizationRequired,
    // which reads as "Requires administrator" (#2006).
    (void) sd_bus_set_allow_interactive_authorization(bus.get(), 0);
    return std::nullopt;
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
        return actionFailure(error, result);
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
        Message reply;
        const std::string unit(name);
        if (const int result = sd_bus_call_method(
                bus, SYSTEMD, SYSTEMD_PATH, SYSTEMD_MANAGER, method, error.get(), reply.out(), "ss", unit.c_str(), "replace");
            result < 0)
        {
            return actionFailure(error, result);
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
[[nodiscard]] std::optional<ServiceActionResult> changeUnitFile(sd_bus* bus, const char* method, std::string_view name, bool withForce)
{
    BusError error;
    Message call;
    Message reply;
    const std::string unit(name);
    int result = sd_bus_message_new_method_call(bus, call.out(), SYSTEMD, SYSTEMD_PATH, SYSTEMD_MANAGER, method);
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
        result = sd_bus_call(bus, call.get(), 0, error.get(), reply.out());
    }
    if (result < 0)
    {
        return actionFailure(error, result);
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

ServiceActionResult startUnit(std::string_view name, const std::stop_token& stopToken, UnitWait wait)
{
    Bus bus;
    if (const auto failed = openForAction(bus))
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

ServiceActionResult stopUnit(std::string_view name, const std::stop_token& stopToken, UnitWait wait)
{
    Bus bus;
    if (const auto failed = openForAction(bus))
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

ServiceActionResult setUnitStartType(std::string_view name, ServiceStartType startType)
{
    const std::optional<SystemdActions::StartTypeSteps> steps = SystemdActions::startTypeSteps(startType);
    if (!steps.has_value())
    {
        return ServiceActionResult::failed("systemd services have no such startup type");
    }
    Bus bus;
    if (const auto failed = openForAction(bus))
    {
        return ServiceActionResult::failed(*failed);
    }
    if (steps->unmaskFirst)
    {
        if (auto failed = changeUnitFile(bus.get(), "UnmaskUnitFiles", name, false))
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
    if (auto failed = changeUnitFile(bus.get(), method, name, withForce))
    {
        return std::move(*failed);
    }
    // As systemctl does after changing unit files, so systemd sees the new state.
    BusError error;
    Message reply;
    if (const int result = sd_bus_call_method(bus.get(), SYSTEMD, SYSTEMD_PATH, SYSTEMD_MANAGER, "Reload", error.get(), reply.out(), "");
        result < 0)
    {
        return actionFailure(error, result);
    }
    return ServiceActionResult::succeeded();
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

ServiceActionResult startUnit(std::string_view /*name*/, const std::stop_token& /*stopToken*/, UnitWait /*wait*/)
{
    return ServiceActionResult::failed("This build has no systemd support (it was built without libsystemd)");
}

ServiceActionResult stopUnit(std::string_view /*name*/, const std::stop_token& /*stopToken*/, UnitWait /*wait*/)
{
    return ServiceActionResult::failed("This build has no systemd support (it was built without libsystemd)");
}

ServiceActionResult setUnitStartType(std::string_view /*name*/, ServiceStartType /*startType*/)
{
    return ServiceActionResult::failed("This build has no systemd support (it was built without libsystemd)");
}

#endif

} // namespace Platform::SystemdBus
