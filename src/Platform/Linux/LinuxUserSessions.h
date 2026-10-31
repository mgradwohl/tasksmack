#pragma once

// The Users tab's sessions on Linux (#3076): logind's, through SystemdBus (LogindSessions.cpp for the read,
// SystemdUnitActions.cpp for Lock and Sign out). Without sd-bus in the build there are none to list.

#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"
#include "Platform/Linux/SystemdBus.h"

#include <cstdlib>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace Platform
{

class LinuxUserSessions final : public IUserSessions
{
  public:
    /// The session TaskSmack runs in is the one pam_systemd named in XDG_SESSION_ID, read once here.
    LinuxUserSessions() : m_CurrentSessionId(currentSessionId())
    {}

    [[nodiscard]] bool hasSessions() const override
    {
        return SystemdBus::built();
    }

    [[nodiscard]] UserSessionsRead read() override
    {
        SystemdBus::LoginSessionsRead read = SystemdBus::readLoginSessions(m_CurrentSessionId);
        if (!read.ok)
        {
            return {.status = UserSessionsStatus::Failed, .sessions = {}, .detail = std::move(read.error)};
        }
        return {.status = UserSessionsStatus::Ok, .sessions = std::move(read.sessions), .detail = {}};
    }

    [[nodiscard]] bool canAct(SessionAction /*action*/) const override
    {
        return SystemdBus::built();
    }

    [[nodiscard]] ServiceActionResult
    act(std::string_view id, SessionAction action, bool allowInteractive, const std::stop_token& stopToken) override
    {
        return SystemdBus::sessionAction(id, action, allowInteractive, stopToken);
    }

  private:
    [[nodiscard]] static std::string currentSessionId()
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - read once, at construction on the composition root's thread
        const char* const id = std::getenv("XDG_SESSION_ID");
        return id != nullptr ? std::string(id) : std::string{};
    }

    std::string m_CurrentSessionId;
};

} // namespace Platform
