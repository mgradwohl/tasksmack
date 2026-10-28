// systemd service control over D-Bus (#2006 phase 2): starting and stopping units and setting their start
// types. Split out of SystemdUnits.cpp (#2728) so tasksmack-helper links these alone, not the services
// read: it holds one system bus connection, opened before its sandbox (holdSystemBusForActions()).

#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/Linux/SystemdActions.h"
#include "Platform/Linux/SystemdBus.h"

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#ifdef TASKSMACK_HAS_SDBUS
#include "Platform/Linux/SdBusCall.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

// As SystemdUnits.cpp: newer libsystemd declares sd_bus and sd_bus_error in sd-bus-protocol.h, older in sd-bus.h.
#include <systemd/sd-bus-protocol.h> // NOLINT(misc-include-cleaner) - see above
#include <systemd/sd-bus.h>
#endif

namespace Platform::SystemdBus
{

#ifdef TASKSMACK_HAS_SDBUS

using Detail::Bus;
using Detail::BusError;
using Detail::Message;
using Detail::openSystemBus;
using Detail::timedOut;

namespace
{

// As SystemdUnits.cpp names them.
constexpr const char* SYSTEMD = "org.freedesktop.systemd1";
constexpr const char* SYSTEMD_PATH = "/org/freedesktop/systemd1";
constexpr const char* SYSTEMD_MANAGER = "org.freedesktop.systemd1.Manager";
constexpr const char* SYSTEMD_SERVICE = "org.freedesktop.systemd1.Service";
constexpr const char* SYSTEMD_UNIT = "org.freedesktop.systemd1.Unit";
constexpr const char* SYSTEMD_JOB = "org.freedesktop.systemd1.Job";

/// The connection holdSystemBusForActions() opened, which every later action borrows; null until then
/// (TaskSmack itself opens one per action). tasksmack-helper serves one request at a time, so one thread.
[[nodiscard]] std::unique_ptr<Bus>& heldBus()
{
    static std::unique_ptr<Bus> bus;
    return bus;
}

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
    if (const std::unique_ptr<Bus>& held = heldBus(); held != nullptr)
    {
        bus.borrow(held->get()); // tasksmack-helper's, opened before its sandbox (holdSystemBusForActions())
    }
    else if (const int result = openSystemBus(bus); result < 0)
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

std::optional<std::string> holdSystemBusForActions()
{
    if (heldBus() != nullptr)
    {
        return std::nullopt;
    }
    auto bus = std::make_unique<Bus>();
    if (const int result = openSystemBus(*bus); result < 0)
    {
        return std::string("The system bus couldn't be reached: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe)
    }
    heldBus() = std::move(bus);
    return std::nullopt;
}

void releaseHeldSystemBusForTesting()
{
    heldBus().reset();
}

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

std::optional<std::string> holdSystemBusForActions()
{
    return "This build has no systemd support (it was built without libsystemd)";
}

void releaseHeldSystemBusForTesting()
{}

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
