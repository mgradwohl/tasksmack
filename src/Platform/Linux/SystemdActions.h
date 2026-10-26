#pragma once

// systemd service control (#2006 phase 2), the parts that need no bus: what each start type asks
// systemd for, how a unit's ActiveState answers a waiting action, and the words a refusal is shown in.
// SystemdBus::startUnit()/stopUnit()/setUnitStartType() send the calls; LinuxServiceActions is the
// IServiceActions over them.
//
// Decided on #2006: Automatic enables the unit file, Manual disables it and Disabled masks it, each
// unmasking first when moving off Disabled (a systemd "disabled" unit can still be started by hand,
// which is Windows' Manual; masking is what stops that, Windows' Disabled). There is no delayed start.
// A stop never takes running dependents with it (systemd would stop every unit that Requires=, BindsTo=,
// PartOf= or Requisite= it):
// it is refused, naming them, as on Windows. polkit's "interactive authentication required" reads as
// "Requires administrator": TaskSmack lets polkit prompt only on the user's "Authenticate and retry"
// (#2304), and then reads its refusals as a dismissed prompt or no agent to show one.

#include "Platform/IServiceProbe.h"

#include <algorithm>
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

/// Whether @p name may be acted on through tasksmack-helper (#2728): a service's unit name as systemd writes
/// one -- 1 to 255 bytes of letters, digits and ":-_.\\@", ending in ".service" after at least one more --
/// so a request can't name a path, another kind of unit, or an option.
[[nodiscard]] constexpr bool isServiceUnitName(std::string_view name) noexcept
{
    constexpr std::string_view SUFFIX = ".service";
    constexpr std::size_t MAX_UNIT_NAME = 255;
    if (name.size() <= SUFFIX.size() || name.size() > MAX_UNIT_NAME || !name.ends_with(SUFFIX) || name.front() == '.' ||
        name.front() == '-')
    {
        return false;
    }
    return std::ranges::all_of(name,
                               [](char c)
                               {
                                   const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                                   const bool digit = c >= '0' && c <= '9';
                                   return letter || digit || c == ':' || c == '-' || c == '_' || c == '.' || c == '\\' || c == '@';
                               });
}

/// Why Automatic did nothing for a unit file without an [Install] section (static, generated, transient):
/// EnableUnitFiles answers carries_install_info=false and changes nothing (#2146).
inline constexpr const char* NO_INSTALL_SECTION_MESSAGE =
    "This service has no [Install] section, so it can't start at boot; other units or a user start it";

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

/// Whether a start that ended with the unit inactive was a oneshot service's successful run (#2123): a
/// Type=oneshot unit without RemainAfterExit= goes activating -> inactive when it has done its work, so
/// polling alone reads it as "didn't start". It ran if its main process started after the start was
/// asked for (@p execStartAfter > @p execStartBefore, ExecMainStartTimestampMonotonic), which a skipped
/// condition or a failed dependency never does, and that run succeeded if its Result is "success".
[[nodiscard]] constexpr bool oneshotRanSuccessfully(std::string_view serviceType,
                                                    std::string_view result,
                                                    std::uint64_t execStartBefore,
                                                    std::uint64_t execStartAfter) noexcept
{
    return serviceType == "oneshot" && result == "success" && execStartAfter > execStartBefore;
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

/// polkit's refusal when it would have to prompt and the call didn't allow it (#2304).
inline constexpr std::string_view INTERACTIVE_AUTHORIZATION_REQUIRED = "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired";

/// A refused or failed call in the user's terms, from its D-Bus error name and @p message (the error's
/// own text). @p timedOut: the call ran out of time.
[[nodiscard]] inline std::string errorText(std::string_view errorName, std::string_view message, bool timedOut, bool interactive = false)
{
    if (timedOut)
    {
        // Allowed to prompt, systemd still holds the request (#2402): it may yet act once the password
        // is entered, so say so rather than report a plain failure.
        return interactive ? "No answer in time; it may still happen if the password prompt is answered" : "systemd didn't answer in time";
    }
    // With a prompt allowed (#2304): still "interactive authorization required" means nothing could ask
    // (no polkit agent in the session); access denied, that the prompt was dismissed or refused.
    if (interactive && errorName == INTERACTIVE_AUTHORIZATION_REQUIRED)
    {
        return "No authentication agent is running to ask for the password";
    }
    if (interactive && errorName == "org.freedesktop.DBus.Error.AccessDenied")
    {
        return "Authentication was canceled or refused";
    }
    if (errorName == INTERACTIVE_AUTHORIZATION_REQUIRED || errorName == "org.freedesktop.DBus.Error.AccessDenied")
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
