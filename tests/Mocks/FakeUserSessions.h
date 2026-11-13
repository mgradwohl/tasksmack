/// @file FakeUserSessions.h
/// @brief Signed-in sessions a test controls (#3076, #3377): what read() lists and what act() answers, with
/// each action recorded. Linux's actions (Lock and Sign out), or with @p windows Windows' (Disconnect and
/// Sign out). Session "5" is another user's: refused for want of an administrator unless the action may ask.
/// Shared by the Users tab's tests and the Processes table's Group: User tests.

#pragma once

#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"

#include <atomic>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TestMocks
{

class FakeUserSessions final : public Platform::IUserSessions
{
  public:
    explicit FakeUserSessions(std::vector<Platform::UserSession> sessions, bool has = true, bool windows = false)
        : m_Sessions(std::move(sessions)), m_Has(has), m_Windows(windows)
    {}
    [[nodiscard]] bool hasSessions() const override
    {
        return m_Has;
    }
    [[nodiscard]] Platform::UserSessionsRead read() override
    {
        ++m_Reads;
        if (!m_Failure.empty())
        {
            return {.status = Platform::UserSessionsStatus::Failed, .sessions = {}, .detail = m_Failure};
        }
        return {.status = Platform::UserSessionsStatus::Ok, .sessions = m_Sessions, .detail = {}};
    }
    [[nodiscard]] bool canAct(Platform::SessionAction action) const override
    {
        return m_Has && (action == Platform::SessionAction::SignOut || (action == Platform::SessionAction::Disconnect) == m_Windows);
    }
    [[nodiscard]] Platform::ServiceActionResult
    act(std::string_view id, Platform::SessionAction action, bool allowInteractive, const std::stop_token& /*stopToken*/) override
    {
        const std::scoped_lock lock(m_Mutex);
        m_Calls.push_back(std::string(actionName(action)) + " " + std::string(id) + (allowInteractive ? " interactive" : ""));
        if (id == "5" && !allowInteractive)
        {
            Platform::ServiceActionResult refused = Platform::ServiceActionResult::failed("Requires administrator");
            refused.needsAuthorization = true;
            return refused;
        }
        return Platform::ServiceActionResult::succeeded();
    }
    /// Each act() so far: "signout 5", "lock 2 interactive".
    [[nodiscard]] std::vector<std::string> calls()
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Calls;
    }
    std::string m_Failure; ///< Set before a read: it fails with this reason
    std::atomic<int> m_Reads{0};

  private:
    [[nodiscard]] static const char* actionName(Platform::SessionAction action)
    {
        switch (action)
        {
        case Platform::SessionAction::Lock:
            return "lock";
        case Platform::SessionAction::Disconnect:
            return "disconnect";
        case Platform::SessionAction::SignOut:
            break;
        }
        return "signout";
    }

    std::vector<Platform::UserSession> m_Sessions;
    bool m_Has = true;
    bool m_Windows = false;
    std::mutex m_Mutex;
    std::vector<std::string> m_Calls;
};

} // namespace TestMocks
