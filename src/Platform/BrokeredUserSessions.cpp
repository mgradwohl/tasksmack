#include "BrokeredUserSessions.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"

#include <charconv>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace Platform
{

namespace
{

/// Session ID @p id as the request carries it, or nullopt for one that isn't a whole decimal number.
[[nodiscard]] std::optional<std::uint32_t> numericId(std::string_view id) noexcept
{
    std::uint32_t value = 0;
    const auto [stop, error] = std::from_chars(id.data(), id.data() + id.size(), value);
    if (id.empty() || error != std::errc{} || stop != id.data() + id.size())
    {
        return std::nullopt;
    }
    return value;
}

} // namespace

BrokeredUserSessions::BrokeredUserSessions(std::unique_ptr<IUserSessions> inner, std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

std::expected<std::optional<ServiceActionResult>, std::string>
BrokeredUserSessions::askBroker(std::string_view id, SessionAction action, const std::stop_token& stopToken)
{
    const std::optional<std::uint32_t> session = numericId(id);
    if (!session.has_value())
    {
        return std::optional<ServiceActionResult>(ServiceActionResult::failed(std::string("Not a session: ") + std::string(id)));
    }
    const std::expected<Broker::Response, std::string> response =
        m_Broker->call(Broker::SessionActionRequest{.action = action, .sessionId = *session}, stopToken);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (const auto* reply = std::get_if<Broker::ServiceActionReply>(&*response))
    {
        return reply->result;
    }
    return std::nullopt; // an ErrorReply: a helper that doesn't do session actions
}

ServiceActionResult
BrokeredUserSessions::act(std::string_view id, SessionAction action, bool allowInteractive, const std::stop_token& stopToken)
{
    if (allowInteractive && m_Broker != nullptr)
    {
        // Authenticate and retry: the refusal came before, so straight to the helper, started if need be.
        if (!m_Broker->connected())
        {
            if (const std::expected<void, std::string> connected = m_Broker->connect(stopToken); !connected.has_value())
            {
                if (stopToken.stop_requested())
                {
                    return ServiceActionResult::stopRequested();
                }
                ServiceActionResult failed = ServiceActionResult::failed("Couldn't act as an administrator: " + connected.error());
                failed.authorizationUnavailable = true; // the UAC prompt was dismissed or couldn't be shown
                return failed;
            }
        }
        const auto helper = askBroker(id, action, stopToken);
        if (!helper.has_value() && stopToken.stop_requested())
        {
            return ServiceActionResult::stopRequested();
        }
        if (!helper.has_value())
        {
            return ServiceActionResult::failed("Couldn't ask the administrator helper: " + helper.error());
        }
        if (!helper->has_value())
        {
            return ServiceActionResult::failed("The administrator helper doesn't act on sessions");
        }
        return **helper;
    }

    ServiceActionResult result = m_Inner->act(id, action, allowInteractive, stopToken);
    if (result.ok || !result.needsAuthorization)
    {
        return result;
    }
    if (m_Broker == nullptr)
    {
        // Nothing could finish it: no retry is offered, only how to get the rights.
        result.needsAuthorization = false;
        result.message += NO_HELPER;
        return result;
    }
    if (!m_Broker->connected())
    {
        return result; // the UI offers Authenticate and retry, which starts the helper
    }
    const auto helper = askBroker(id, action, stopToken);
    if (!helper.has_value() && stopToken.stop_requested())
    {
        return ServiceActionResult::stopRequested();
    }
    if (!helper.has_value())
    {
        // The helper exited or broke between connected() and the call: the refusal stands, so the retry is
        // still offered, and starts a fresh helper.
        result.message += " (the administrator helper couldn't be asked: " + helper.error() + ")";
        return result;
    }
    return helper->value_or(std::move(result));
}

} // namespace Platform
