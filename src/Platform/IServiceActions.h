#pragma once

// Service control (#1577, phase 2 of #800): start, stop, restart and set the start type, beside the
// read-only IServiceProbe. Every action blocks until the service reaches its new state or a bounded
// wait runs out, so callers run them off the UI thread.

#include "Platform/IServiceProbe.h"

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

    [[nodiscard]] static ServiceActionResult succeeded()
    {
        return {.ok = true, .message = {}};
    }
    [[nodiscard]] static ServiceActionResult failed(std::string message)
    {
        return {.ok = false, .message = std::move(message)};
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

    /// Starts the service and waits until it is running.
    [[nodiscard]] virtual ServiceActionResult start(std::string_view name) = 0;
    /// Asks the service to stop and waits until it has. Running dependents are never stopped with it:
    /// the action fails, naming them.
    [[nodiscard]] virtual ServiceActionResult stop(std::string_view name) = 0;
    /// stop(), then start().
    [[nodiscard]] virtual ServiceActionResult restart(std::string_view name) = 0;
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
    [[nodiscard]] ServiceActionResult start(std::string_view /*name*/) override
    {
        return unsupported();
    }
    [[nodiscard]] ServiceActionResult stop(std::string_view /*name*/) override
    {
        return unsupported();
    }
    [[nodiscard]] ServiceActionResult restart(std::string_view /*name*/) override
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
