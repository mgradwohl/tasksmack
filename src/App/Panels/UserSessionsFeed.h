#pragma once

// The signed-in sessions, read once for every view that shows them (#3377): the sessions the platform lists
// and acts through, the UsersModel that publishes them, the gate and the sampler that read them at the
// Settings interval while a view needs them (ProcessGrouping::sessionsDemand()), and the Lock, Disconnect and
// Sign out worker. ShellPanels owns it; the Users tab draws from it today, and the Processes table's Group:
// User will too. The views only read its immutable publications on the UI thread: no view calls a probe.

#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/SessionActionControl.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/SamplingConfig.h"
#include "Domain/UsersModel.h"
#include "Platform/IUserSessions.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace App
{

class UserSessionsFeed
{
  public:
    /// The platform's sessions (PanelPlatforms' makeUsersPanelSessions(), or none in a synthetic run).
    UserSessionsFeed();

    /// Test seam: start() takes its sessions from @p makeSessions instead of the platform's.
    explicit UserSessionsFeed(std::function<std::shared_ptr<Platform::IUserSessions>()> makeSessions);
    ~UserSessionsFeed();

    UserSessionsFeed(const UserSessionsFeed&) = delete;
    UserSessionsFeed& operator=(const UserSessionsFeed&) = delete;
    UserSessionsFeed(UserSessionsFeed&&) = delete;
    UserSessionsFeed& operator=(UserSessionsFeed&&) = delete;

    /// Makes the sessions, the model and the action worker, then samples if a view already demands them
    /// (one shown before start(), or still shown across stop()); otherwise sampling waits for demand. Once
    /// until stop().
    /// On the UI thread, from the Users tab's onAttach() (the composition root's one sessions creation).
    void start();

    /// Joins the sampler thread (the one place it is waited for), then stops and waits for an action in
    /// flight, and lets the sessions go. The views' inputs (setUsersTabShown(), setProcessesView()) are kept.
    /// Safe to call twice, or without start().
    void stop();

    /// Whether the Users tab is shown, one of sessionsDemand()'s inputs.
    void setUsersTabShown(bool shown);

    /// Whether the Processes tab is shown and how its table is grouped, sessionsDemand()'s other inputs.
    /// Until Group: User shows sessions (ProcessGrouping::USER_GROUPS_SHOW_SESSIONS), they read nothing.
    void setProcessesView(bool shown, ProcessGrouping::GroupBy groupBy);

    /// The Settings refresh interval (#2731): the sampler's, now or when it starts.
    void setInterval(std::chrono::milliseconds interval);

    /// A read as soon as the sampler can, if it runs (an action finished: the session's new state).
    void requestRefresh();

    /// Whether the sessions are read now (tests).
    [[nodiscard]] bool demanded() const noexcept
    {
        return m_Demanded;
    }

    /// The model the views read publications from, or null before start() and after stop().
    [[nodiscard]] const Domain::UsersModel* model() const noexcept
    {
        return m_Model.get();
    }

    /// The newest generation read (tests: wait for the sampler).
    [[nodiscard]] std::uint64_t publishedVersion() const noexcept
    {
        return m_Model ? m_Model->version() : 0;
    }

    /// Whether the platform offers @p action on a session (the menus' items).
    [[nodiscard]] bool canAct(Platform::SessionAction action) const
    {
        return m_Sessions != nullptr && m_Sessions->canAct(action);
    }

    /// Lock, Disconnect and Sign out, or null before start() and after stop().
    [[nodiscard]] SessionActionControl* actions() noexcept
    {
        return m_Actions ? &*m_Actions : nullptr;
    }

  private:
    void applyDemand();

    std::function<std::shared_ptr<Platform::IUserSessions>()> m_MakeSessions; ///< Set by the test seam only
    std::shared_ptr<Platform::IUserSessions> m_Sessions;
    std::shared_ptr<Domain::UsersModel> m_Model;
    std::shared_ptr<SamplingGate> m_Gate;                                                // open while sessionsDemand() says so
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler;                                // created when first demanded
    std::optional<SessionActionControl> m_Actions;                                       // made at start, with the sessions it acts through
    std::chrono::milliseconds m_Interval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS}; // the Settings interval
    bool m_UsersTabShown = false;
    bool m_ProcessesTabShown = false;
    ProcessGrouping::GroupBy m_GroupBy = ProcessGrouping::GroupBy::None;
    bool m_Demanded = false; // what the gate was last set to
};

} // namespace App
