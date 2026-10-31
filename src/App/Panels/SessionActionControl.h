#pragma once

// The Users tab's Lock and Sign out (#3076). Lock runs at once, as Task Manager's does; Sign out first asks,
// naming the user and the session and warning that unsaved work is lost (and, for TaskSmack's own session,
// that TaskSmack closes too). Either runs on a worker of its own, since logind answers only once polkit has
// (another user's session asks for an administrator), and a refusal for want of one offers "Authenticate and
// retry", which calls again allowing the password prompt (#2304).
//
// The control owns only its UI state; the IUserSessions it acts through is the panel's, shared with the model,
// and outlives it. Free of ImGui: the panel draws the confirm and the line. Tested in tests/App/test_Users.cpp.

#include "App/Panels/BackgroundTask.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/UsersView.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"
#include "Platform/ThreadName.h"

#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace App
{

/// The Sign out confirm's question for @p session.
[[nodiscard]] inline std::string signOutQuestion(const Platform::UserSession& session)
{
    std::string question = std::format(
        "Sign out {} from {}?\n\nEvery program in the session ends, and unsaved work is lost.", session.user, sessionPlace(session));
    if (session.current)
    {
        question += " This is the session TaskSmack runs in, so TaskSmack closes too.";
    }
    return question;
}

class SessionActionControl
{
  public:
    explicit SessionActionControl(std::shared_ptr<Platform::IUserSessions> sessions) : m_Sessions(std::move(sessions))
    {}

    /// The row menu's Lock: run at once.
    void lock(const Platform::UserSession& session)
    {
        start(session, Platform::SessionAction::Lock, false);
    }

    /// The row menu's Sign out...: the confirm opens for @p session.
    void requestSignOut(const Platform::UserSession& session)
    {
        if (busy())
        {
            return;
        }
        m_Pending = session;
        m_Question = signOutQuestion(session);
        m_ShowConfirm = true;
    }

    [[nodiscard]] const std::optional<Platform::UserSession>& pending() const noexcept
    {
        return m_Pending;
    }

    [[nodiscard]] const std::string& question() const noexcept
    {
        return m_Question;
    }

    [[nodiscard]] bool& showConfirm() noexcept
    {
        return m_ShowConfirm;
    }

    void cancel()
    {
        m_Pending.reset();
        m_ShowConfirm = false;
    }

    /// The confirm was accepted: the pending session is signed out, on the worker.
    void confirm()
    {
        if (!m_Pending.has_value())
        {
            return;
        }
        const Platform::UserSession session = std::move(*m_Pending);
        cancel();
        start(session, Platform::SessionAction::SignOut, false);
    }

    /// Whether the result line offers "Authenticate and retry" (#2304).
    [[nodiscard]] bool offersAuthenticateAndRetry() const noexcept
    {
        return m_Retry.has_value() && !busy();
    }

    /// "Authenticate and retry" was pressed: the refused action again, allowing the password prompt.
    void retryAuthorized()
    {
        if (!m_Retry.has_value() || busy())
        {
            return;
        }
        const Retry retry = *m_Retry;
        m_Retry.reset();
        start(retry.session, retry.action, true);
    }

    /// Each frame: a finished action's result goes to the line, which goes after a while. @p wait blocks
    /// until a running action finishes (the tests' way to take it in).
    void update(float deltaSeconds, bool wait = false)
    {
        if (std::optional<Platform::ServiceActionResult> result = m_Worker.poll(wait))
        {
            finish(*result);
            return;
        }
        if (m_ResultSecondsLeft > 0.0F)
        {
            m_ResultSecondsLeft -= deltaSeconds;
            if (m_ResultSecondsLeft <= 0.0F)
            {
                m_Result = {};
                m_Retry.reset();
            }
        }
    }

    [[nodiscard]] bool busy() const noexcept
    {
        return m_Worker.busy();
    }

    /// What the line says while busy(): "Locking alice's session...".
    [[nodiscard]] const std::string& pendingText() const noexcept
    {
        return m_PendingText;
    }

    [[nodiscard]] const Detail::ActionResultMessage& result() const noexcept
    {
        return m_Result;
    }

    /// Whether an action succeeded since the last call: the panel then reads the sessions again at once.
    [[nodiscard]] bool takeDone() noexcept
    {
        return std::exchange(m_Done, false);
    }

    static constexpr float RESULT_SECONDS = 6.0F;

  private:
    struct Retry
    {
        Platform::UserSession session;
        Platform::SessionAction action = Platform::SessionAction::Lock;
    };

    void start(const Platform::UserSession& session, Platform::SessionAction action, bool allowInteractive)
    {
        if (busy() || m_Sessions == nullptr)
        {
            return;
        }
        m_Running = Retry{.session = session, .action = action};
        const bool lock = action == Platform::SessionAction::Lock;
        m_PendingText = allowInteractive
                          ? std::format("Waiting for authorization to {} {}'s session...", lock ? "lock" : "sign out", session.user)
                          : std::format("{} {}'s session...", lock ? "Locking" : "Signing out", session.user);
        m_RetryAllowed = !allowInteractive; // a refusal to the administrator isn't offered again
        if (std::optional<Platform::ServiceActionResult> failed =
                m_Worker.start([sessions = m_Sessions, id = session.id, action, allowInteractive](const std::stop_token& stop)
                               { return sessions->act(id, action, allowInteractive, stop); }))
        {
            finish(*failed);
        }
    }

    void finish(const Platform::ServiceActionResult& result)
    {
        m_PendingText.clear();
        m_Retry.reset();
        const bool lock = m_Running.action == Platform::SessionAction::Lock;
        const std::string& user = m_Running.session.user;
        if (result.ok)
        {
            m_Result = {.ok = true, .text = std::format("{} {}'s session", lock ? "Locked" : "Signed out", user)};
            m_Done = true;
        }
        else
        {
            m_Result = {.ok = false, .text = std::format("Couldn't {} {}'s session: {}", lock ? "lock" : "sign out", user, result.message)};
            if (result.needsAuthorization && m_RetryAllowed)
            {
                m_Retry = m_Running;
            }
        }
        m_ResultSecondsLeft = m_Retry.has_value() ? Detail::AUTHORIZE_RESULT_SECONDS : RESULT_SECONDS;
    }

    [[nodiscard]] static Platform::ServiceActionResult failed(std::string detail)
    {
        return Platform::ServiceActionResult::failed(std::move(detail));
    }

    std::shared_ptr<Platform::IUserSessions> m_Sessions;
    std::optional<Platform::UserSession> m_Pending;
    std::string m_Question;
    bool m_ShowConfirm = false;
    Retry m_Running;
    bool m_RetryAllowed = false;
    std::optional<Retry> m_Retry;
    std::string m_PendingText;
    Detail::ActionResultMessage m_Result;
    float m_ResultSecondsLeft = 0.0F;
    bool m_Done = false;
    // Last, so it is destroyed (stopping and waiting for the worker) before the members above.
    BackgroundTask<Platform::ServiceActionResult> m_Worker{Platform::SESSION_ACTION_THREAD_NAME, &SessionActionControl::failed};
};

} // namespace App
