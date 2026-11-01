#pragma once

// The Users tab's sessions on Windows (#3076): the Terminal Services sessions, read with
// WTSEnumerateSessionsW and WTSQuerySessionInformationW (WTSSessionInfo, WTSClientName,
// WTSClientProtocolType) on the tab's sampler, and Disconnect (WTSDisconnectSession) and Sign out
// (WTSLogoffSession) on its action worker. Windows has no Lock here: Disconnect takes its place.
//
// Every action is checked first against a fresh list (UserSessionsMath::checkSessionAction: a live,
// signed-in session that isn't session 0). One's own session needs no rights; another user's is refused
// with "Requires administrator" (needsAuthorization), which BrokeredUserSessions sends to the
// administrator helper, and the helper does it through the same checks (handleHelperSessionAction()).
//
// The WTS calls go through WtsCalls, so the tests drive the reader and the checks with fakes and never
// disconnect or sign out a real session.

#include "Platform/BrokerProtocol.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"
#include "Platform/Windows/WindowsUserSessionsMath.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{

/// What WindowsUserSessions calls: the real WTS functions (realWtsCalls()), or a test's.
struct WtsCalls
{
    /// Every session's facts, or why WTS couldn't list them.
    std::function<std::expected<std::vector<UserSessionsMath::WtsSessionFacts>, std::string>()> enumerate;
    /// The session this process runs in (ProcessIdToSessionId).
    std::uint32_t currentSessionId = 0;
    /// WTSDisconnectSession, not waiting: 0 when it was done, else the Win32 error.
    std::function<std::uint32_t(std::uint32_t sessionId)> disconnect;
    /// WTSLogoffSession, not waiting: 0 when it was done, else the Win32 error.
    std::function<std::uint32_t(std::uint32_t sessionId)> logoff;
    /// FormatMessage's words for a Win32 error; empty when there are none.
    std::function<std::string(std::uint32_t error)> describe;
};

/// The real WTS calls, for this process's session.
[[nodiscard]] WtsCalls realWtsCalls();

/// The sessions through @p calls as the Users tab's (UserSessionsMath::toUserSession()), or Failed with why.
[[nodiscard]] UserSessionsRead readUserSessions(const WtsCalls& calls);

/// Checks @p action on session @p id against a fresh list through @p calls, then does it: the one path
/// TaskSmack's own calls and the helper's share.
[[nodiscard]] ServiceActionResult actOnSession(const WtsCalls& calls, std::string_view id, SessionAction action);

/// The administrator helper's answer to @p request (#3076): actOnSession() with @p calls, the helper's own.
/// A Lock, session 0, an ended session and one nobody is signed in to are refused there.
[[nodiscard]] ServiceActionResult handleHelperSessionAction(const Broker::SessionActionRequest& request, const WtsCalls& calls);

class WindowsUserSessions final : public IUserSessions
{
  public:
    /// The real WTS calls.
    WindowsUserSessions();
    /// Test seam: @p calls in their place.
    explicit WindowsUserSessions(WtsCalls calls);

    [[nodiscard]] bool hasSessions() const override
    {
        return true;
    }

    [[nodiscard]] UserSessionsRead read() override
    {
        return readUserSessions(m_Calls);
    }

    /// Disconnect and Sign out; Lock is Linux's.
    [[nodiscard]] bool canAct(SessionAction action) const override
    {
        return action == SessionAction::Disconnect || action == SessionAction::SignOut;
    }

    /// @p allowInteractive changes nothing here: Windows has no prompt in-process, and another user's
    /// session goes to the helper (BrokeredUserSessions), whose start is the UAC prompt.
    [[nodiscard]] ServiceActionResult
    act(std::string_view id, SessionAction action, bool /*allowInteractive*/, const std::stop_token& /*stopToken*/) override
    {
        return actOnSession(m_Calls, id, action);
    }

  private:
    WtsCalls m_Calls; // stateless calls: read() on the sampler and act() on the action worker may overlap
};

} // namespace Platform::Windows
