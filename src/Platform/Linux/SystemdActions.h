#pragma once

// systemd service control (#2006 phase 2), the parts that need no bus: what each start type asks
// systemd for, how a unit's ActiveState answers a waiting action, and the words a refusal is shown in.
// SystemdBus::startUnit()/stopUnit()/setUnitStartType() send the calls; LinuxServiceActions is the
// IServiceActions over them.
//
// Decided on #2006: Automatic enables the unit file, Manual disables it and Disabled masks it, each
// unmasking first when moving off Disabled (a systemd "disabled" unit can still be started by hand,
// which is Windows' Manual; masking is what stops that, Windows' Disabled). There is no delayed start.
// A stop never takes running dependents with it (systemd would stop every unit that Requires= it):
// it is refused, naming them, as on Windows. polkit's "interactive authentication required", which
// TaskSmack never asks for, reads as "Requires administrator".

#include "Platform/IServiceProbe.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Platform::SystemdActions
{

/// The unit-file change a start type asks for.
enum class FileChange : std::uint8_t
{
    Enable,  ///< EnableUnitFiles: started at boot (Automatic)
    Disable, ///< DisableUnitFiles: started only when asked (Manual)
    Mask,    ///< MaskUnitFiles: never started (Disabled)
};

/// What setting @p startType sends: whether to unmask first, and the change. nullopt for a start type
/// systemd has no equivalent of (Automatic (delayed), and the read-only Boot/System/Unknown).
struct StartTypeSteps
{
    bool unmaskFirst = false;
    FileChange change = FileChange::Disable;
};

[[nodiscard]] constexpr std::optional<StartTypeSteps> startTypeSteps(ServiceStartType startType) noexcept
{
    switch (startType)
    {
    case ServiceStartType::Automatic:
        return StartTypeSteps{.unmaskFirst = true, .change = FileChange::Enable};
    case ServiceStartType::Manual:
        return StartTypeSteps{.unmaskFirst = true, .change = FileChange::Disable};
    case ServiceStartType::Disabled:
        return StartTypeSteps{.unmaskFirst = false, .change = FileChange::Mask};
    default:
        return std::nullopt;
    }
}

/// Which way a waiting action is going.
enum class Goal : std::uint8_t
{
    Running,
    Stopped,
};

/// How one poll of the unit answers a wait for @p goal.
enum class WaitStep : std::uint8_t
{
    Done,   ///< The unit got there
    Failed, ///< It ended somewhere else (failed to start; its start job is gone and it isn't running)
    Wait,   ///< Still on its way
};

/// @p activeState is the unit's ActiveState now; @p jobGone whether the job the action queued has
/// finished (its object is gone). A start is done once the unit is active; it failed if the unit
/// failed, or its job ended with the unit not running (a condition unmet, it exited at once). A stop is
/// done once the unit is inactive or failed: either way it isn't running.
[[nodiscard]] constexpr WaitStep waitStep(Goal goal, std::string_view activeState, bool jobGone) noexcept
{
    if (goal == Goal::Running)
    {
        if (activeState == "active" || activeState == "reloading" || activeState == "refreshing")
        {
            return WaitStep::Done;
        }
        if (activeState == "failed")
        {
            return WaitStep::Failed;
        }
        return jobGone && activeState != "activating" ? WaitStep::Failed : WaitStep::Wait;
    }
    if (activeState == "inactive" || activeState == "failed")
    {
        return WaitStep::Done;
    }
    return jobGone && activeState != "deactivating" ? WaitStep::Failed : WaitStep::Wait;
}

/// Whether a unit in @p activeState counts as a running dependent, one a stop would take down.
[[nodiscard]] constexpr bool isRunning(std::string_view activeState) noexcept
{
    return activeState == "active" || activeState == "activating" || activeState == "reloading" || activeState == "refreshing";
}

/// A stop refused because of running dependents, naming them (Windows' wording, #1577).
[[nodiscard]] inline std::string dependentsText(std::span<const std::string> dependents)
{
    if (dependents.empty())
    {
        return "Other running services depend on it";
    }
    std::string text = "Stop the services that depend on it first: ";
    for (std::size_t i = 0; i < dependents.size(); ++i)
    {
        text += (i == 0) ? "" : ", ";
        text += dependents[i];
    }
    return text;
}

/// A refused or failed call in the user's terms, from its D-Bus error name and @p message (the error's
/// own text). @p timedOut: the call ran out of time.
[[nodiscard]] inline std::string errorText(std::string_view errorName, std::string_view message, bool timedOut)
{
    if (timedOut)
    {
        return "systemd didn't answer in time";
    }
    if (errorName == "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired" ||
        errorName == "org.freedesktop.DBus.Error.AccessDenied")
    {
        return "Requires administrator";
    }
    if (errorName == "org.freedesktop.DBus.Error.ServiceUnknown" || errorName == "org.freedesktop.DBus.Error.NameHasNoOwner")
    {
        return "systemd isn't running";
    }
    if (errorName == "org.freedesktop.systemd1.NoSuchUnit")
    {
        return "No such service";
    }
    if (errorName == "org.freedesktop.systemd1.UnitMasked")
    {
        return "It is disabled (masked): set its startup type first";
    }
    return message.empty() ? std::string("systemd refused it") : std::string(message);
}

} // namespace Platform::SystemdActions
