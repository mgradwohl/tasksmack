#pragma once

// A signed-in session's Lock, Disconnect and Sign out (#3076). Lock (Linux) runs at once, as Task Manager's does;
// Sign out first asks, naming the user and the session and warning that unsaved work is lost (and, for
// TaskSmack's own session, that TaskSmack closes too); Disconnect (Windows) asks too, saying the session's
// programs keep running (and, for TaskSmack's own, that this screen locks). Each runs on a worker of its own,
// since logind answers only once polkit has, and Windows' helper only once its UAC prompt is answered
// (another user's session needs an administrator), and a refusal for want of one offers "Authenticate and
// retry", which calls again allowing the prompt (#2304).
//
// The control owns only its UI state; the IUserSessions it acts through is the panel's, shared with the model,
// and outlives it. Free of ImGui: the panel draws the confirm and the line. Tested in tests/App/test_UserSessions.cpp.

#include "App/Panels/BackgroundTask.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/RestartAsAdministratorButton.h"
#include "App/Panels/UserGrouping.h"
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

/// The words a session action's line and confirm use.
struct SessionActionWords
{
    const char* verb = "";  ///< "lock", "disconnect", "sign out"
    const char* doing = ""; ///< "Locking", "Disconnecting", "Signing out"
    const char* done = "";  ///< "Locked", "Disconnected", "Signed out"
    const char* title = ""; ///< The confirm's title and button: "Disconnect", "Sign out"
};

[[nodiscard]] constexpr SessionActionWords sessionActionWords(Platform::SessionAction action) noexcept
{
    switch (action)
    {
    case Platform::SessionAction::Lock:
        return {.verb = "lock", .doing = "Locking", .done = "Locked", .title = "Lock"};
    case Platform::SessionAction::Disconnect:
        return {.verb = "disconnect", .doing = "Disconnecting", .done = "Disconnected", .title = "Disconnect"};
    case Platform::SessionAction::SignOut:
        break;
    }
    return {.verb = "sign out", .doing = "Signing out", .done = "Signed out", .title = "Sign out"};
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
        request(session, Platform::SessionAction::SignOut, UserGrouping::signOutQuestion(session));
    }

    /// The row menu's Disconnect... (Windows): the confirm opens for @p session.
    void requestDisconnect(const Platform::UserSession& session)
    {
        request(session, Platform::SessionAction::Disconnect, UserGrouping::disconnectQuestion(session));
    }

    [[nodiscard]] const std::optional<Platform::UserSession>& pending() const noexcept
    {
        return m_Pending;
    }

    /// What the open confirm asks to do: SignOut or Disconnect.
    [[nodiscard]] Platform::SessionAction pendingAction() const noexcept
    {
        return m_PendingAction;
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

    /// The confirm was accepted: the pending session is signed out or disconnected, on the worker.
    void confirm()
    {
        if (!m_Pending.has_value())
        {
            return;
        }
        const Platform::UserSession session = std::move(*m_Pending);
        const Platform::SessionAction action = m_PendingAction;
        cancel();
        start(session, action, false);
    }

    /// Whether the result line offers "Authenticate and retry" (#2304).
    [[nodiscard]] bool offersAuthenticateAndRetry() const noexcept
    {
        return m_Retry.has_value() && !busy();
    }

    /// Whether the result line offers "Restart as administrator..." in its place (#3669): the action was refused
    /// for want of an administrator, with no helper to retry it (ServiceActionResult::needsAdministrator), where
    /// TaskSmack can restart elevated.
    [[nodiscard]] bool offersRestartAsAdministrator() const noexcept
    {
        return m_OfferRestart && !busy();
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
                m_OfferRestart = false;
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

    void request(const Platform::UserSession& session, Platform::SessionAction action, std::string question)
    {
        if (busy())
        {
            return;
        }
        m_Pending = session;
        m_PendingAction = action;
        m_Question = std::move(question);
        m_ShowConfirm = true;
    }

    void start(const Platform::UserSession& session, Platform::SessionAction action, bool allowInteractive)
    {
        if (busy() || m_Sessions == nullptr)
        {
            return;
        }
        m_Running = Retry{.session = session, .action = action};
        const SessionActionWords words = sessionActionWords(action);
        m_PendingText = allowInteractive ? std::format("Waiting for authorization to {} {}'s session...", words.verb, session.user)
                                         : std::format("{} {}'s session...", words.doing, session.user);
        m_RetryAllowed = !allowInteractive; // a refusal to the administrator isn't offered again
        if (std::optional<Platform::ServiceActionResult> failed =
                // An init-capture, not [session]: a copy-capture of the const reference would make a const
                // member, so moving the work into its worker (noexcept) would copy the session's strings.
            m_Worker.start([sessions = m_Sessions, confirmed = session, action, allowInteractive](const std::stop_token& stop)
                           { return sessions->actOnConfirmed(confirmed, action, allowInteractive, stop); }))
        {
            finish(*failed);
        }
    }

    void finish(const Platform::ServiceActionResult& result)
    {
        m_PendingText.clear();
        m_Retry.reset();
        m_OfferRestart = false;
        const SessionActionWords words = sessionActionWords(m_Running.action);
        const std::string& user = m_Running.session.user;
        if (result.ok)
        {
            m_Result = {.ok = true, .text = std::format("{} {}'s session", words.done, user)};
            m_Done = true;
        }
        else
        {
            m_Result = {.ok = false, .text = std::format("Couldn't {} {}'s session: {}", words.verb, user, result.message)};
            if (result.needsAuthorization && m_RetryAllowed)
            {
                m_Retry = m_Running;
            }
            // No helper to retry it (#3669): Restart as administrator... instead, where TaskSmack can restart.
            m_OfferRestart =
                RestartAsAdministrator::offersRestart(result.needsAdministrator, m_Retry.has_value(), RestartAsAdministrator::offered());
        }
        m_ResultSecondsLeft = (m_Retry.has_value() || m_OfferRestart) ? Detail::AUTHORIZE_RESULT_SECONDS : RESULT_SECONDS;
    }

    [[nodiscard]] static Platform::ServiceActionResult failed(std::string detail)
    {
        return Platform::ServiceActionResult::failed(std::move(detail));
    }

    std::shared_ptr<Platform::IUserSessions> m_Sessions;
    std::optional<Platform::UserSession> m_Pending;
    Platform::SessionAction m_PendingAction = Platform::SessionAction::SignOut;
    std::string m_Question;
    bool m_ShowConfirm = false;
    Retry m_Running;
    bool m_RetryAllowed = false;
    std::optional<Retry> m_Retry;
    bool m_OfferRestart = false; // offersRestartAsAdministrator(): refused, with no helper (#3669)
    std::string m_PendingText;
    Detail::ActionResultMessage m_Result;
    float m_ResultSecondsLeft = 0.0F;
    bool m_Done = false;
    // Last, so it is destroyed (stopping and waiting for the worker) before the members above.
    BackgroundTask<Platform::ServiceActionResult> m_Worker{Platform::SESSION_ACTION_THREAD_NAME, &SessionActionControl::failed};
};

} // namespace App
