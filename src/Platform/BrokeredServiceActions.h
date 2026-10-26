#pragma once

// Service actions with the privileged helper behind them (#2728): an action systemd refuses for want of
// authorization goes to a running tasksmack-helper, which does it as root over the system bus it opened
// before its sandbox, so polkit asks for no password; and the user's "Authenticate and retry" starts the
// helper (one password, then held) rather than letting polkit prompt for each action. Everything else is
// the inner actions'. Linux only: Windows' helper doesn't do service actions.

#include "Platform/IPrivilegedBroker.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"

#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace Platform
{

class BrokeredServiceActions final : public IServiceActions
{
  public:
    BrokeredServiceActions(std::unique_ptr<IServiceActions> inner, std::shared_ptr<IPrivilegedBroker> broker);

    /// The inner capabilities; Authenticate and retry is offered wherever the inner actions or the helper can.
    [[nodiscard]] ServiceActionCapabilities capabilities() const override;

    [[nodiscard]] ServiceActionResult start(std::string_view name, const std::stop_token& stopToken) override;
    [[nodiscard]] ServiceActionResult stop(std::string_view name, const std::stop_token& stopToken) override;
    [[nodiscard]] ServiceActionResult restart(std::string_view name, const std::stop_token& stopToken) override;
    [[nodiscard]] ServiceActionResult setStartType(std::string_view name, ServiceStartType startType) override;

    /// Starts the helper if it isn't running (its password prompt), then does @p action through it.
    [[nodiscard]] ServiceActionResult
    runAuthorized(ServiceAction action, std::string_view name, ServiceStartType startType, const std::stop_token& stopToken) override;

  private:
    /// @p result, or, when only authorization stood in its way and the helper is running, the helper's.
    [[nodiscard]] ServiceActionResult
    retried(ServiceActionResult result, ServiceAction action, std::string_view name, ServiceStartType startType);
    /// @p action through the helper: its result; nullopt from a helper that doesn't do service actions; or
    /// why it couldn't be asked.
    [[nodiscard]] std::expected<std::optional<ServiceActionResult>, std::string>
    askBroker(ServiceAction action, std::string_view name, ServiceStartType startType, const std::stop_token& stopToken);

    std::unique_ptr<IServiceActions> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

/// @p actions decorated with @p broker, or @p actions as they are when there is no broker.
[[nodiscard]] std::unique_ptr<IServiceActions> withPrivilegedBroker(std::unique_ptr<IServiceActions> actions,
                                                                    std::shared_ptr<IPrivilegedBroker> broker);

} // namespace Platform
