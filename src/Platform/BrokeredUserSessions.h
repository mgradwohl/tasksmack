#pragma once

// The signed-in sessions' actions with the administrator helper behind them (#3076), Windows' counterpart of
// what polkit does for Linux: a Disconnect or Sign out Windows refuses for want of an administrator's rights
// (another user's session; the inner act() says needsAuthorization) goes to a running helper at once, and
// the user's "Authenticate and retry" starts the helper (its UAC prompt) and sends it there. One's own
// session, and everything else, is the inner sessions'. The helper checks the session again before it
// acts (BrokerProtocol's SessionActionRequest).
//
// Without a helper (none installed, a portable copy, or TaskSmack already elevated), a refusal isn't
// offered the retry: it says to run TaskSmack as an administrator instead.

#include "Platform/IPrivilegedBroker.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"

#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace Platform
{

class BrokeredUserSessions final : public IUserSessions
{
  public:
    /// @p broker may be null: there is no helper, and a refusal then says so.
    BrokeredUserSessions(std::unique_ptr<IUserSessions> inner, std::shared_ptr<IPrivilegedBroker> broker);

    [[nodiscard]] bool hasSessions() const override
    {
        return m_Inner->hasSessions();
    }
    [[nodiscard]] UserSessionsRead read() override
    {
        return m_Inner->read();
    }
    [[nodiscard]] bool canAct(SessionAction action) const override
    {
        return m_Inner->canAct(action);
    }

    /// The inner act(), by ID: no helper, since the helper acts only on a confirmed sign-in.
    [[nodiscard]] ServiceActionResult
    act(std::string_view id, SessionAction action, bool allowInteractive, const std::stop_token& stopToken) override
    {
        return m_Inner->act(id, action, allowInteractive, stopToken);
    }

    /// The inner actOnConfirmed(). A refusal an administrator could finish goes to the helper, naming the
    /// confirmed sign-in (its user, domain and sign-in time) so the helper acts only on it: at once while the
    /// helper runs, or, with @p allowInteractive (Authenticate and retry), once connect() has started it.
    [[nodiscard]] ServiceActionResult
    actOnConfirmed(const UserSession& confirmed, SessionAction action, bool allowInteractive, const std::stop_token& stopToken) override;

    /// What a refusal says when there is no helper to finish it.
    static constexpr std::string_view NO_HELPER = " (run TaskSmack as an administrator to act on another user's session)";

  private:
    /// @p action through the helper: its result; nullopt from a helper that doesn't do session actions;
    /// or why it couldn't be asked.
    [[nodiscard]] std::expected<std::optional<ServiceActionResult>, std::string>
    askBroker(const UserSession& confirmed, SessionAction action, const std::stop_token& stopToken);

    std::unique_ptr<IUserSessions> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

} // namespace Platform
