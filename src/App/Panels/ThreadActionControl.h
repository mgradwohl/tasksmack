#pragma once

// One thread's Suspend, Resume and End (#3069, Windows): Process Details' Threads table's row menu items, their
// confirm and the result line, as Close connection's (ConnectionCloseControl.h). The action runs on a worker
// of its own (ProcessActionRun, #2305), so the helper's round trip or a UAC prompt never holds the frame, and a
// permission refusal offers "Authenticate and retry" (#2308): the helper acts as an administrator.
//
// The control owns only its UI state; the IProcessActions it acts through stays the panel's (from the
// composition root) and is passed in, and the panel declares it before the view holding this control, so the
// control (and its worker, waited for) is destroyed first. Free of ImGui: ProcessThreadsView draws the confirm
// and the line. Tested in tests/App/test_ProcessThreadsView.cpp.

#include "App/Panels/ProcessActionRun.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/RestartAsAdministratorButton.h"
#include "Platform/IProcessActions.h"

#include <cstdint>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

/// A thread action waiting for its confirm, or running: what it is, on which thread of which process.
struct PendingThreadAction
{
    Platform::ProcessTarget target;
    std::int32_t tid = 0;
    std::uint64_t threadCreateTime = 0; ///< When the thread confirmed was created (ProcessThread::createTime): a reused TID is refused
    std::string threadName;             ///< Empty: the thread has none
    std::string processName;            ///< The process's, as Process Details shows it
    Platform::ThreadAction action = Platform::ThreadAction::Suspend;
};

/// "thread 1234 (worker)", or "thread 1234" for one with no name.
[[nodiscard]] inline std::string threadLabel(std::int32_t tid, std::string_view name)
{
    return name.empty() ? std::format("thread {}", tid) : std::format("thread {} ({})", tid, name);
}

/// The confirm's title: "Suspend thread", "Resume thread", "End thread".
[[nodiscard]] constexpr const char* threadActionTitle(Platform::ThreadAction action) noexcept
{
    switch (action)
    {
    case Platform::ThreadAction::Suspend:
        return "Suspend thread";
    case Platform::ThreadAction::Resume:
        return "Resume thread";
    case Platform::ThreadAction::End:
        break;
    }
    return "End thread";
}

/// The confirm's button: "Suspend Thread", "Resume Thread", "End Thread" (in the danger fill, threadActionIsDestructive()).
[[nodiscard]] constexpr const char* threadActionConfirmLabel(Platform::ThreadAction action) noexcept
{
    switch (action)
    {
    case Platform::ThreadAction::Suspend:
        return "Suspend Thread";
    case Platform::ThreadAction::Resume:
        return "Resume Thread";
    case Platform::ThreadAction::End:
        break;
    }
    return "End Thread";
}

/// Whether the confirm's button takes the danger fill: End, which can't be undone.
[[nodiscard]] constexpr bool threadActionIsDestructive(Platform::ThreadAction action) noexcept
{
    return action == Platform::ThreadAction::End;
}

/// The confirm's question, naming the thread and its process, and what the action risks.
[[nodiscard]] inline std::string threadActionQuestion(const PendingThreadAction& request)
{
    const std::string thread = threadLabel(request.tid, request.threadName);
    switch (request.action)
    {
    case Platform::ThreadAction::Suspend:
        return std::format("Suspend {} of {} (PID {})?\n\n"
                           "The thread stops until it is resumed. If it holds a lock or a resource the rest of {} waits for, "
                           "the whole process may hang until then. Windows counts suspensions: a thread suspended twice "
                           "needs two resumes.",
                           thread,
                           request.processName,
                           request.target.pid,
                           request.processName);
    case Platform::ThreadAction::Resume:
        return std::format("Resume {} of {} (PID {})?\n\n"
                           "This takes back one suspension. The thread runs again once none are left; one suspended by the "
                           "program itself, or by a debugger, may not expect it.",
                           thread,
                           request.processName,
                           request.target.pid);
    case Platform::ThreadAction::End:
        break;
    }
    return std::format("End {} of {} (PID {})?\n\n"
                       "WARNING: ending a single thread can corrupt the process. The thread is stopped at once, with no "
                       "chance to clean up: a lock it holds stays held, memory it was changing is left half-written, and "
                       "{} may hang, crash or save damaged data. This can't be undone.\n\n"
                       "End the whole process instead unless you know this thread is safe to end.",
                       thread,
                       request.processName,
                       request.target.pid,
                       request.processName);
}

class ThreadActionControl
{
  public:
    /// A row menu item chose @p request: its confirm opens. Ignored while an action runs.
    void request(PendingThreadAction request)
    {
        if (busy())
        {
            return;
        }
        m_Question = threadActionQuestion(request);
        m_Pending = std::move(request);
        m_ShowConfirm = true;
    }

    /// The action the confirm is up for, if any.
    [[nodiscard]] const std::optional<PendingThreadAction>& pending() const noexcept
    {
        return m_Pending;
    }

    [[nodiscard]] const std::string& question() const noexcept
    {
        return m_Question;
    }

    /// The flag the confirm modal opens on, and clears when it closes (ProcessActionConfirm::renderLabelled()).
    [[nodiscard]] bool& showConfirm() noexcept
    {
        return m_ShowConfirm;
    }

    /// The confirm was cancelled (or its process is no longer shown): nothing is done.
    void cancel()
    {
        m_Pending.reset();
        m_ShowConfirm = false;
    }

    /// The confirm was accepted: the pending action runs through @p actions, on the worker.
    void confirm(Platform::IProcessActions* actions)
    {
        if (!m_Pending.has_value())
        {
            return;
        }
        PendingThreadAction request = std::move(*m_Pending);
        m_Pending.reset();
        m_ShowConfirm = false;
        if (actions == nullptr || !actions->actionCapabilities().canActOnThreads)
        {
            m_RunRequest = std::move(request);
            finish(Platform::ProcessActionResult::error("Acting on one thread isn't available"));
            return;
        }
        Platform::AuthorizedAction retry;
        retry.kind = Platform::AuthorizedAction::Kind::ThreadAction;
        retry.target = request.target;
        retry.tid = request.tid;
        retry.threadCreateTime = request.threadCreateTime;
        retry.threadAction = request.action;
        std::string pendingText =
            std::format("{} {} of {}...", runningVerb(request.action), threadLabel(request.tid, request.threadName), request.processName);
        m_RunRequest = std::move(request);
        static_cast<void>(m_Run.start(
            std::move(pendingText),
            [actions, retry] { return actions->actOnThread(retry.target, retry.tid, retry.threadCreateTime, retry.threadAction); },
            [this, actions, retry, generation = m_Generation](const Platform::ProcessActionResult& result)
            {
                finish(result);
                // Never for an action started before the selection changed: the retry would be offered under
                // another process's Threads section, for a thread of the one no longer shown.
                if (generation != m_Generation)
                {
                    return;
                }
                const bool helperCanRedo = actions->actionCapabilities().canAuthorizeInteractively;
                if (result.permissionDenied && helperCanRedo)
                {
                    m_Retry = Retry{.actions = actions, .action = retry};
                    m_ResultSecondsLeft = Detail::AUTHORIZE_RESULT_SECONDS; // time to read it and decide
                }
                // No helper to retry it (#3669): Restart as administrator... instead, where TaskSmack can restart.
                else if (RestartAsAdministrator::offersRestart(result.permissionDenied, helperCanRedo, RestartAsAdministrator::offered()))
                {
                    m_OfferRestart = true;
                    m_ResultSecondsLeft = Detail::AUTHORIZE_RESULT_SECONDS;
                }
            }));
    }

    /// Whether the result line offers "Authenticate and retry" (#2308).
    [[nodiscard]] bool offersAuthenticateAndRetry() const noexcept
    {
        return m_Retry.has_value() && !busy();
    }

    /// Whether the result line offers "Restart as administrator..." in its place (#3669): the action was refused
    /// for want of permission, with no helper to retry it, where TaskSmack can restart elevated.
    [[nodiscard]] bool offersRestartAsAdministrator() const noexcept
    {
        return m_OfferRestart && !busy();
    }

    /// "Authenticate and retry" was pressed: the refused action again, by the helper as an administrator,
    /// after the UAC prompt (IProcessActions::runAuthorized()).
    void retryAuthorized()
    {
        if (!m_Retry.has_value() || busy())
        {
            return;
        }
        const Retry retry = *m_Retry;
        m_Retry.reset();
        static_cast<void>(m_Run.start(
            std::format(
                "Waiting for authorization to {} {}...", verb(m_RunRequest.action), threadLabel(m_RunRequest.tid, m_RunRequest.threadName)),
            [actions = retry.actions, action = retry.action](const std::stop_token& stopToken)
            { return actions->runAuthorized(action, stopToken); },
            // A refusal to the administrator isn't offered again.
            [this](const Platform::ProcessActionResult& result) { finish(result); }));
    }

    /// Each frame: a finished action's result goes to the line, and the line goes after a while. @p wait
    /// blocks until a running action finishes (the tests' way to take it in).
    void update(float deltaSeconds, bool wait = false)
    {
        if (m_Run.update(wait))
        {
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

    /// A different process was selected: the last result and its retry are dropped. A confirm still up is left
    /// for ProcessThreadsView::renderConfirmation() to close, as its process is no longer the one shown (ImGui
    /// keeps a modal open until it is closed). An action already running finishes, and says so, but a refusal it
    /// meets offers no Authenticate and retry.
    void onSelectionChanged()
    {
        ++m_Generation; // an action still running finishes, but offers no retry (it was for the previous process)
        m_Retry.reset();
        m_OfferRestart = false;
        m_Result = {};
        m_ResultSecondsLeft = 0.0F;
    }

    [[nodiscard]] bool busy() const noexcept
    {
        return m_Run.busy();
    }

    /// What the line says while busy(): "Suspending thread 1234 (worker) of server...".
    [[nodiscard]] const std::string& pendingText() const noexcept
    {
        return m_Run.pendingText();
    }

    /// The last action's result line; empty when there is none to show.
    [[nodiscard]] const Detail::ActionResultMessage& result() const noexcept
    {
        return m_Result;
    }

    /// Whether an action succeeded since the last call: the view then reads the threads again at once, so
    /// the state column shows the new suspend count (or the thread gone).
    [[nodiscard]] bool takeActed() noexcept
    {
        return std::exchange(m_Acted, false);
    }

    /// How long the result line stays, in seconds.
    static constexpr float RESULT_SECONDS = 6.0F;

  private:
    struct Retry
    {
        Platform::IProcessActions* actions = nullptr;
        Platform::AuthorizedAction action;
    };

    [[nodiscard]] static constexpr const char* verb(Platform::ThreadAction action) noexcept
    {
        switch (action)
        {
        case Platform::ThreadAction::Suspend:
            return "suspend";
        case Platform::ThreadAction::Resume:
            return "resume";
        case Platform::ThreadAction::End:
            break;
        }
        return "end";
    }

    [[nodiscard]] static constexpr const char* runningVerb(Platform::ThreadAction action) noexcept
    {
        switch (action)
        {
        case Platform::ThreadAction::Suspend:
            return "Suspending";
        case Platform::ThreadAction::Resume:
            return "Resuming";
        case Platform::ThreadAction::End:
            break;
        }
        return "Ending";
    }

    [[nodiscard]] static constexpr const char* doneVerb(Platform::ThreadAction action) noexcept
    {
        switch (action)
        {
        case Platform::ThreadAction::Suspend:
            return "Suspended";
        case Platform::ThreadAction::Resume:
            return "Resumed";
        case Platform::ThreadAction::End:
            break;
        }
        return "Ended";
    }

    void finish(const Platform::ProcessActionResult& result)
    {
        m_Retry.reset();
        m_OfferRestart = false;
        const std::string thread = threadLabel(m_RunRequest.tid, m_RunRequest.threadName);
        if (result.success)
        {
            std::string text = std::format("{} {} of {}", doneVerb(m_RunRequest.action), thread, m_RunRequest.processName);
            if (!result.successNote.empty())
            {
                text += std::format(" ({})", result.successNote); // the suspend count, when it isn't the plain one
            }
            m_Result = {.ok = true, .text = std::move(text)};
        }
        else
        {
            m_Result = {.ok = false, .text = std::format("Couldn't {} {}: {}", verb(m_RunRequest.action), thread, result.errorMessage)};
        }
        m_ResultSecondsLeft = RESULT_SECONDS;
        m_Acted = m_Acted || result.success;
    }

    std::optional<PendingThreadAction> m_Pending;
    std::string m_Question;
    bool m_ShowConfirm = false;
    std::optional<Retry> m_Retry;
    bool m_OfferRestart = false;      ///< offersRestartAsAdministrator(): refused, with no helper (#3669)
    PendingThreadAction m_RunRequest; ///< The action running (or last run)
    Detail::ActionResultMessage m_Result;
    float m_ResultSecondsLeft = 0.0F;
    bool m_Acted = false;
    std::uint64_t m_Generation = 0; ///< Bumped by each selection change
    // Last, so it is destroyed (stopping and waiting for the worker) before the members its finish uses.
    ProcessActionRun m_Run;
};

} // namespace App
