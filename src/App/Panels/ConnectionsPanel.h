#pragma once

#include "App/Panel.h"
#include "App/Panels/ConnectionCloseControl.h"
#include "App/Panels/ConnectionsView.h"
#include "App/Panels/SamplingGate.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ConnectionsModel.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/ISystemConnections.h"

#include <chrono>
#include <functional>
#include <memory>

namespace App
{

/// The Connections tab (#3066): every TCP and UDP socket on the machine and the process holding it.
/// Owns the ConnectionsModel and samples it on its own BackgroundSampler at the Settings refresh
/// interval, only while the tab is shown, as the Startup and Services tabs do: hidden, a SamplingGate
/// keeps it from reading anything (each read walks every process's descriptors), and switching tabs
/// never joins the sampler thread on the UI thread (only onDetach() does). Selecting a row selects its
/// process; a double-click shows it in Process Details. The row menu's "Close connection..." shuts a
/// TCP connection down after a confirm (#3066).
class ConnectionsPanel : public Panel
{
  public:
    ConnectionsPanel();

    /// Test seam: onAttach() takes its reader from @p makeReader and its process actions from @p makeActions
    /// (none when empty) instead of the platform's, and events go to @p raiseEvent instead of Core::Application.
    ConnectionsPanel(std::function<std::unique_ptr<Platform::ISystemConnectionsReader>()> makeReader,
                     std::function<void(Core::Event&)> raiseEvent,
                     std::function<std::unique_ptr<Platform::IProcessActions>()> makeActions = {});
    ~ConnectionsPanel() override;

    ConnectionsPanel(const ConnectionsPanel&) = delete;
    ConnectionsPanel& operator=(const ConnectionsPanel&) = delete;
    ConnectionsPanel(ConnectionsPanel&&) = delete;
    ConnectionsPanel& operator=(ConnectionsPanel&&) = delete;

    void onAttach() override;
    void onDetach() override;
    void onEvent(Core::Event& event) override;
    void renderContent() override;

    /// The newest generation read (tests: wait for the sampler).
    [[nodiscard]] std::uint64_t publishedVersion() const noexcept
    {
        return m_Model ? m_Model->version() : 0;
    }

    /// The Close connection control (tests: take a finished close in, press Authenticate and retry).
    [[nodiscard]] ConnectionCloseControl& closeControl() noexcept
    {
        return m_CloseControl;
    }

  private:
    void setActive(bool active);
    void raiseEvent(Core::Event& event);
    void renderCloseResultLine();
    void renderCloseConfirm();

    std::function<std::unique_ptr<Platform::ISystemConnectionsReader>()> m_MakeReader; ///< Set by the test seam only
    std::function<void(Core::Event&)> m_RaiseEvent;                                    ///< Set by the test seam only
    std::function<std::unique_ptr<Platform::IProcessActions>()> m_MakeActions;         ///< Set by the test seam only
    bool m_Seamed = false;                                                             ///< Built through the test seam
    std::shared_ptr<Domain::ConnectionsModel> m_Model;
    std::shared_ptr<SamplingGate> m_Gate;                 // open while the tab is shown
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler; // created when the tab first shows
    std::shared_ptr<const Domain::ConnectionsPublication> m_Publication;
    ConnectionsViewState m_ViewState;
    std::chrono::milliseconds m_Interval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS}; // the Settings interval
    // Close connection (#3066). The actions are declared before the control, whose worker uses them, so the
    // control (and its worker, waited for) is destroyed first.
    std::unique_ptr<Platform::IProcessActions> m_Actions;
    bool m_CanClose = false; ///< The actions can close connections (read once, at attach)
    ConnectionCloseControl m_CloseControl;
};

} // namespace App
