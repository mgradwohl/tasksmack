#pragma once

// Enabling and disabling startup entries (#801, phase 2), beside the read-only IStartupProbe. An
// action records the entry's enabled state the way the OS's own tools do and never removes the entry
// itself, so it can be enabled again.

#include "Platform/IStartupProbe.h"

#include <string>
#include <utility>

namespace Platform
{

/// The outcome of one startup action, in the style of ServiceActionResult: on failure, `message` says
/// why in words the UI shows as they stand ("Requires administrator"). Empty on success.
struct StartupActionResult
{
    bool ok = false;
    std::string message;

    [[nodiscard]] static StartupActionResult succeeded()
    {
        return {.ok = true, .message = {}};
    }
    [[nodiscard]] static StartupActionResult failed(std::string message)
    {
        return {.ok = false, .message = std::move(message)};
    }
};

/// Which actions this platform can run at all. False hides the actions in the UI.
struct StartupActionCapabilities
{
    bool canSetEnabled = false;
    /// Whether TaskSmack runs elevated: an all-users (Machine scope) entry can be changed only then.
    bool elevated = false;
};

/// Whether an entry registered at @p location has an enabled state that can be changed: RunOnce
/// entries run once and are removed, and have none.
[[nodiscard]] constexpr bool hasSwitchableState(StartupLocation location) noexcept
{
    return location != StartupLocation::RunOnceUser && location != StartupLocation::RunOnceMachine;
}

/// Changes startup entries' enabled state. Every call is self-contained and quick (one registry write
/// on Windows), but callers still run it off the UI thread. Called from one thread at a time.
class IStartupActions
{
  public:
    virtual ~IStartupActions() = default;

    IStartupActions() = default;
    IStartupActions(const IStartupActions&) = default;
    IStartupActions& operator=(const IStartupActions&) = default;
    IStartupActions(IStartupActions&&) = default;
    IStartupActions& operator=(IStartupActions&&) = default;

    [[nodiscard]] virtual StartupActionCapabilities capabilities() const = 0;

    /// Enables or disables @p entry (as IStartupProbe listed it) from the next sign-in on.
    [[nodiscard]] virtual StartupActionResult setEnabled(const StartupEntry& entry, bool enabled) = 0;
};

/// The actions for a platform without them (Linux until XDG autostart lands): every capability false,
/// every action refused.
class UnsupportedStartupActions final : public IStartupActions
{
  public:
    [[nodiscard]] StartupActionCapabilities capabilities() const override
    {
        return {};
    }
    [[nodiscard]] StartupActionResult setEnabled(const StartupEntry& /*entry*/, bool /*enabled*/) override
    {
        return StartupActionResult::failed("Startup apps can't be changed on this platform yet");
    }
};

} // namespace Platform
