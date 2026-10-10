#pragma once

// Service control (#1577, phase 2 of #800): start, stop, restart and set the start type, beside the
// read-only IServiceProbe. Every action blocks until the service reaches its new state or a bounded
// wait runs out, so callers run them off the UI thread. The waiting actions take a std::stop_token:
// a stop request abandons the wait (#1591), so closing TaskSmack never waits out a slow service.

#include "Platform/IServiceProbe.h"

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

    [[nodiscard]] static ServiceActionResult succeeded()
    {
        return {.ok = true, .message = {}, .cancelled = false};
    }
    [[nodiscard]] static ServiceActionResult failed(std::string message)
    {
        return {.ok = false, .message = std::move(message), .cancelled = false};
    }
    [[nodiscard]] static ServiceActionResult stopRequested()
    {
        return {.ok = false, .message = "Cancelled before it finished", .cancelled = true};
    }
};

/// Which actions this platform can run at all. A false action is hidden or disabled in the UI.
struct ServiceActionCapabilities
{
    bool canStart = false;
    bool canStop = false;
    bool canRestart = false;
    bool canSetStartType = false;
    /// Whether TaskSmack runs elevated. Not elevated, most actions are refused, but a service's own
    /// security descriptor may still allow some, so the UI only warns rather than disabling them.
    bool elevated = false;
};

/// Controls services by name (the ServiceInfo::name key). Every call is self-contained, so an
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
    [[nodiscard]] virtual ServiceActionResult start(std::string_view name, const std::stop_token& stopToken) = 0;
    /// Asks the service to stop and waits until it has, or until @p stopToken is stopped. Running
    /// dependents are never stopped with it: the action fails, naming them.
    [[nodiscard]] virtual ServiceActionResult stop(std::string_view name, const std::stop_token& stopToken) = 0;
    /// stop(), then start(); a stop request between the two skips the start.
    [[nodiscard]] virtual ServiceActionResult restart(std::string_view name, const std::stop_token& stopToken) = 0;
    /// Sets the start type: Automatic, AutomaticDelayed, Manual or Disabled (others are refused).
    [[nodiscard]] virtual ServiceActionResult setStartType(std::string_view name, ServiceStartType startType) = 0;
};

/// The actions for a platform without service control (Linux until systemd control lands, synthetic
/// runs): every capability false, and every action refused.
class UnsupportedServiceActions final : public IServiceActions
{
  public:
    [[nodiscard]] ServiceActionCapabilities capabilities() const override
    {
        return {};
    }
    // Nothing waits here, so the stop token is ignored.
    [[nodiscard]] ServiceActionResult start(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return unsupported();
    }
    [[nodiscard]] ServiceActionResult stop(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return unsupported();
    }
    [[nodiscard]] ServiceActionResult restart(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return unsupported();
    }
    [[nodiscard]] ServiceActionResult setStartType(std::string_view /*name*/, ServiceStartType /*startType*/) override
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
