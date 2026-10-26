#include "BrokeredServiceActions.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"

#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace Platform
{

BrokeredServiceActions::BrokeredServiceActions(std::unique_ptr<IServiceActions> inner, std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

ServiceActionCapabilities BrokeredServiceActions::capabilities() const
{
    ServiceActionCapabilities caps = m_Inner->capabilities();
    caps.canAuthorizeInteractively = caps.canAuthorizeInteractively || (m_Broker != nullptr && caps.canStart);
    return caps;
}

std::expected<std::optional<ServiceActionResult>, std::string>
BrokeredServiceActions::askBroker(ServiceAction action, std::string_view name, ServiceStartType startType, const std::stop_token& stopToken)
{
    const std::expected<Broker::Response, std::string> response =
        m_Broker->call(Broker::ServiceActionRequest{.action = action, .unit = std::string(name), .startType = startType}, stopToken);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (const auto* reply = std::get_if<Broker::ServiceActionReply>(&*response))
    {
        return reply->result;
    }
    return std::nullopt; // an ErrorReply: a helper that doesn't do service actions
}

ServiceActionResult
BrokeredServiceActions::retried(ServiceActionResult result, ServiceAction action, std::string_view name, ServiceStartType startType)
{
    if (result.ok || !result.needsAuthorization || m_Broker == nullptr || !m_Broker->connected())
    {
        return result;
    }
    const auto helper = askBroker(action, name, startType, std::stop_token{});
    if (!helper.has_value())
    {
        // The helper exited, timed out or broke between connected() and the call (#2414): the refusal stands,
        // so the UI still offers Authenticate and retry, which starts a fresh helper.
        result.message += " (the administrator helper couldn't be asked: " + helper.error() + ")";
        return result;
    }
    return helper->value_or(std::move(result));
}

ServiceActionResult BrokeredServiceActions::start(std::string_view name, const std::stop_token& stopToken)
{
    return retried(m_Inner->start(name, stopToken), ServiceAction::Start, name, ServiceStartType::Unknown);
}

ServiceActionResult BrokeredServiceActions::stop(std::string_view name, const std::stop_token& stopToken)
{
    return retried(m_Inner->stop(name, stopToken), ServiceAction::Stop, name, ServiceStartType::Unknown);
}

ServiceActionResult BrokeredServiceActions::restart(std::string_view name, const std::stop_token& stopToken)
{
    return retried(m_Inner->restart(name, stopToken), ServiceAction::Restart, name, ServiceStartType::Unknown);
}

ServiceActionResult BrokeredServiceActions::setStartType(std::string_view name, ServiceStartType startType)
{
    return retried(m_Inner->setStartType(name, startType), ServiceAction::SetStartType, name, startType);
}

ServiceActionResult BrokeredServiceActions::runAuthorized(ServiceAction action,
                                                          std::string_view name,
                                                          ServiceStartType startType,
                                                          const std::stop_token& stopToken)
{
    if (m_Broker == nullptr)
    {
        return m_Inner->runAuthorized(action, name, startType, stopToken);
    }
    if (!m_Broker->connected())
    {
        if (const std::expected<void, std::string> connected = m_Broker->connect(stopToken); !connected.has_value())
        {
            ServiceActionResult failed = ServiceActionResult::failed("Couldn't act as an administrator: " + connected.error());
            failed.authorizationUnavailable = true; // the prompt was dismissed or couldn't be shown
            return failed;
        }
    }
    const auto helper = askBroker(action, name, startType, stopToken);
    if (!helper.has_value())
    {
        return ServiceActionResult::failed("Couldn't ask the administrator helper: " + helper.error());
    }
    if (!helper->has_value())
    {
        return m_Inner->runAuthorized(action, name, startType, stopToken); // polkit's own prompt, as before (#2304)
    }
    return **helper;
}

std::unique_ptr<IServiceActions> withPrivilegedBroker(std::unique_ptr<IServiceActions> actions, std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr)
    {
        return actions;
    }
    return std::make_unique<BrokeredServiceActions>(std::move(actions), std::move(broker));
}

} // namespace Platform
