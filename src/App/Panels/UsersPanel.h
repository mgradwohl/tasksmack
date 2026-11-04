#pragma once

#include "App/Panel.h"
#include "App/Panels/SessionActionControl.h"
#include "App/Panels/UserSessionsFeed.h"
#include "App/Panels/UsersView.h"
#include "Core/Event.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/UsersModel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace App
{

/// The Users tab (#3076): each signed-in user with their sessions and their processes' CPU, memory, disk and
/// network totals, their processes under them; Lock (Linux) or Disconnect (Windows), and Sign out, from a
/// session's row menu. Its sessions and their actions are the feed's (#3377), which ShellPanels owns and
/// this tab starts at attach and stops at detach; the feed reads them at the Settings refresh interval, only
/// while the tab is shown, as the Connections tab does. The totals come from the Processes model's published
/// snapshots (setProcessModel()), so there is no second walk of the processes. Selecting a process row
/// selects it; a double-click shows it in Process Details.
class UsersPanel : public Panel
{
  public:
    /// @p feed outlives the panel.
    explicit UsersPanel(UserSessionsFeed& feed);

    /// Test seam: events go to @p raiseEvent instead of Core::Application.
    UsersPanel(UserSessionsFeed& feed, std::function<void(Core::Event&)> raiseEvent);
    ~UsersPanel() override = default;

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
        return m_Feed.publishedVersion();
    }

    /// Lock and Sign out (tests: take a finished action in, press Authenticate and retry).
    [[nodiscard]] SessionActionControl* actions() noexcept
    {
        return m_Feed.actions();
    }

  private:
    void raiseEvent(Core::Event& event);
    void renderActionLine();
    void renderConfirm(); // Sign out's or Disconnect's

    UserSessionsFeed& m_Feed;
    std::function<void(Core::Event&)> m_RaiseEvent; ///< Set by the test seam only
    std::shared_ptr<const Domain::UsersPublication> m_Publication;
    std::weak_ptr<Domain::ProcessModel> m_ProcessModel;
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> m_Processes;
    std::uint64_t m_ProcessesVersion = 0;
    UsersViewState m_ViewState;
};

} // namespace App
