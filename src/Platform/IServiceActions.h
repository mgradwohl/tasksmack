#pragma once

// Service control (#1577, phase 2 of #800): start, stop, restart and set the start type, beside the
// read-only IServiceProbe. Every action blocks until the service reaches its new state or a bounded
// wait runs out, so callers run them off the UI thread. The waiting actions take a std::stop_token:
// a stop request abandons the wait (#1591), so closing TaskSmack never waits out a slow service.

#include "Platform/IServiceProbe.h"
#include "Platform/NativeId.h"

#include <concepts>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace Platform
{

/// The outcome of one service action, in the style of ProcessActionResult: on failure, `message`
/// says why in words the UI can show as they stand ("Requires administrator"). Empty on success.
struct ServiceActionResult
{
    bool ok = false;
    std::string message;
    /// The wait was abandoned on a stop request (#1591). Whatever request was already sent to the
    /// service manager stands: the service may still reach its new state.
    bool cancelled = false;
    /// Refused only because authorizing it would need a password prompt, which the call didn't allow. No service
    /// action sets it any more (#3867); IUserSessions' session actions still do, for Windows' helper retry.
    bool needsAuthorization = false;
    /// An authorized retry allowed a prompt, but nothing could show one: offering it again would fail the same
    /// way. Kept for IUserSessions' results, as needsAuthorization (#3867).
    bool authorizationUnavailable = false;
    /// Refused for want of an administrator's rights, with nothing in this TaskSmack that could get them (no
    /// helper, and no password prompt to ask for): restarting TaskSmack as administrator could do it (#3669).
    /// Set in process only (the helper never sends it): Windows' "Requires administrator", and a session action
    /// with no helper (BrokeredUserSessions).
    bool needsAdministrator = false;

    [[nodiscard]] static ServiceActionResult succeeded()
    {
        return {
            .ok = true,
            .message = {},
            .cancelled = false,
            .needsAuthorization = false,
            .authorizationUnavailable = false,
            .needsAdministrator = false,
        };
    }
    [[nodiscard]] static ServiceActionResult failed(std::string message)
    {
        return {
            .ok = false,
            .message = std::move(message),
            .cancelled = false,
            .needsAuthorization = false,
            .authorizationUnavailable = false,
            .needsAdministrator = false,
        };
    }
    [[nodiscard]] static ServiceActionResult stopRequested()
    {
        return {
            .ok = false,
            .message = "Cancelled before it finished",
            .cancelled = true,
            .needsAuthorization = false,
            .authorizationUnavailable = false,
            .needsAdministrator = false,
        };
    }
};

/// The service an action is for (#4304): its name, ServiceInfo::name, and the probe's opaque ServiceInfo::nativeId.
/// Windows acts on the exact native name the id maps to, since a listed name may be lossy (a lone surrogate is
/// shown as U+FFFD), and refuses an id it no longer lists; a platform whose names are exact (Linux's unit names)
/// acts on the name. Converts from a bare name, with no id, for a caller that has only the name.
struct ServiceTarget
{
    std::string name;
    NativeId nativeId = NativeId::None;

    // Implicit on purpose: every caller with only a name ("Spooler", a std::string) stays as it was.
    // NOLINTBEGIN(cppcoreguidelines-explicit-constructor,misc-explicit-constructor)
    template<typename Name>
        requires std::convertible_to<const Name&, std::string_view>
    ServiceTarget(const Name& serviceName) : name(std::string_view(serviceName))
    {}
    // NOLINTEND(cppcoreguidelines-explicit-constructor,misc-explicit-constructor)
    ServiceTarget(std::string_view serviceName, NativeId id) : name(serviceName), nativeId(id)
    {}
};

/// Which actions this platform can run at all. A false action is hidden or disabled in the UI.
struct ServiceActionCapabilities
{
    bool canStart = false;
    bool canStop = false;
    bool canRestart = false;
    bool canSetStartType = false;
    /// Whether "Automatic (delayed)" is a start type here: Windows' delayed auto-start. systemd has none,
    /// so Linux clears it and the startup-type menu leaves it out (#2006).
    bool canDelayStart = true;
    /// Whether TaskSmack runs elevated. Not elevated, most actions are refused, but a service's own
    /// security descriptor may still allow some, so the UI only warns rather than disabling them.
    bool elevated = false;
};

/// Controls services by ServiceTarget: the ServiceInfo::name key and, where the platform gives one, the
/// probe's ServiceInfo::nativeId (#4304). Every call is self-contained, so an
/// implementation may be called from any one thread at a time; each blocks for at most the platform's
/// bounded wait (on Windows 10 s per state change, 20 s for a restart).
///
/// start(), stop() and restart() take a std::stop_token. Once a stop is requested, an action that has
/// sent nothing yet sends nothing and returns ServiceActionResult::stopRequested(); one that is waiting
/// returns it at its next poll. A request already sent is never undone. An implementation that does
/// not wait may ignore the token.
class IServiceActions
{
  public:
    virtual ~IServiceActions() = default;

    IServiceActions() = default;
    IServiceActions(const IServiceActions&) = default;
    IServiceActions& operator=(const IServiceActions&) = default;
    IServiceActions(IServiceActions&&) = default;
    IServiceActions& operator=(IServiceActions&&) = default;

    [[nodiscard]] virtual ServiceActionCapabilities capabilities() const = 0;

    /// Starts the service and waits until it is running, or until @p stopToken is stopped.
    [[nodiscard]] virtual ServiceActionResult start(const ServiceTarget& service, const std::stop_token& stopToken) = 0;
    /// Asks the service to stop and waits until it has, or until @p stopToken is stopped. Running
    /// dependents are never stopped with it: the action fails, naming them.
    [[nodiscard]] virtual ServiceActionResult stop(const ServiceTarget& service, const std::stop_token& stopToken) = 0;
    /// stop(), then start(); a stop request between the two skips the start.
    [[nodiscard]] virtual ServiceActionResult restart(const ServiceTarget& service, const std::stop_token& stopToken) = 0;
    /// Sets the start type: Automatic, AutomaticDelayed, Manual or Disabled (others are refused).
    [[nodiscard]] virtual ServiceActionResult setStartType(const ServiceTarget& service, ServiceStartType startType) = 0;
};

/// The actions for a platform without service control (synthetic runs, a Linux build without systemd
/// support): every capability false, and every action refused.
class UnsupportedServiceActions final : public IServiceActions
{
  public:
    [[nodiscard]] ServiceActionCapabilities capabilities() const override
    {
        return {};
    }
    // Nothing waits here, so the stop token is ignored.
    [[nodiscard]] ServiceActionResult start(const ServiceTarget& /*service*/, const std::stop_token& /*stopToken*/) override
    {
        return unsupported();
    }
    [[nodiscard]] ServiceActionResult stop(const ServiceTarget& /*service*/, const std::stop_token& /*stopToken*/) override
    {
        return unsupported();
    }
    [[nodiscard]] ServiceActionResult restart(const ServiceTarget& /*service*/, const std::stop_token& /*stopToken*/) override
    {
        return unsupported();
    }
    [[nodiscard]] ServiceActionResult setStartType(const ServiceTarget& /*service*/, ServiceStartType /*startType*/) override
    {
        return unsupported();
    }

  private:
    [[nodiscard]] static ServiceActionResult unsupported()
    {
        return ServiceActionResult::failed("Service control isn't available on this platform yet");
    }
};

} // namespace Platform
