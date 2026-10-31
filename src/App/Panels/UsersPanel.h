#pragma once

#include "App/Panel.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/SessionActionControl.h"
#include "App/Panels/UsersView.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/UsersModel.h"
#include "Platform/IUserSessions.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace App
{

/// The Users tab (#3076): each signed-in user with their sessions and their processes' CPU, memory, disk and
/// network totals, their processes under them; Lock and Sign out from a session's row menu. Owns the
/// UsersModel and samples it on its own BackgroundSampler at the Settings refresh interval, only while the tab
/// is shown, as the Connections tab does; the totals come from the Processes model's published snapshots
/// (setProcessModel()), so there is no second walk of the processes. Selecting a process row selects it; a
/// double-click shows it in Process Details.
class UsersPanel : public Panel
{
  public:
    UsersPanel();

    /// Test seam: onAttach() takes its sessions from @p makeSessions instead of the platform's, and events go
    /// to @p raiseEvent instead of Core::Application.
    UsersPanel(std::function<std::shared_ptr<Platform::IUserSessions>()> makeSessions, std::function<void(Core::Event&)> raiseEvent);
    ~UsersPanel() override;

    UsersPanel(const UsersPanel&) = delete;
    UsersPanel& operator=(const UsersPanel&) = delete;
    UsersPanel(UsersPanel&&) = delete;
    UsersPanel& operator=(UsersPanel&&) = delete;

    void onAttach() override;
    void onDetach() override;
    void onEvent(Core::Event& event) override;
    void renderContent() override;

    /// The processes the totals come from (ShellPanels::connectModels()).
    void setProcessModel(std::weak_ptr<Domain::ProcessModel> model)
    {
        m_ProcessModel = std::move(model);
    }

    /// The newest generation read (tests: wait for the sampler).
    [[nodiscard]] std::uint64_t publishedVersion() const noexcept
    {
        return m_Model ? m_Model->version() : 0;
    }

    /// Lock and Sign out (tests: take a finished action in, press Authenticate and retry).
    [[nodiscard]] SessionActionControl* actions() noexcept
    {
        return m_Actions ? &*m_Actions : nullptr;
    }

  private:
    void setActive(bool active);
    void raiseEvent(Core::Event& event);
    void renderActionLine();
    void renderSignOutConfirm();

    std::function<std::shared_ptr<Platform::IUserSessions>()> m_MakeSessions; ///< Set by the test seam only
    std::function<void(Core::Event&)> m_RaiseEvent;                           ///< Set by the test seam only
    std::shared_ptr<Platform::IUserSessions> m_Sessions;
    std::shared_ptr<Domain::UsersModel> m_Model;
    std::shared_ptr<SamplingGate> m_Gate;                 // open while the tab is shown
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler; // created when the tab first shows
    std::shared_ptr<const Domain::UsersPublication> m_Publication;
    std::weak_ptr<Domain::ProcessModel> m_ProcessModel;
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> m_Processes;
    std::uint64_t m_ProcessesVersion = 0;
    UsersViewState m_ViewState;
    std::optional<SessionActionControl> m_Actions; // made at attach, with the sessions it acts through
    std::chrono::milliseconds m_Interval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS}; // the Settings interval
};

} // namespace App
