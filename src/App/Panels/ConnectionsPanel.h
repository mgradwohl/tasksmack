#pragma once

#include "App/Panel.h"
#include "App/Panels/ConnectionsView.h"
#include "App/Panels/SamplingGate.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ConnectionsModel.h"
#include "Domain/SamplingConfig.h"
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
/// process; a double-click shows it in Process Details.
class ConnectionsPanel : public Panel
{
  public:
    ConnectionsPanel();

    /// Test seam: onAttach() takes its reader from @p makeReader instead of the platform's, and events go
    /// to @p raiseEvent instead of Core::Application.
    ConnectionsPanel(std::function<std::unique_ptr<Platform::ISystemConnectionsReader>()> makeReader,
                     std::function<void(Core::Event&)> raiseEvent);
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

  private:
    void setActive(bool active);
    void raiseEvent(Core::Event& event);

    std::function<std::unique_ptr<Platform::ISystemConnectionsReader>()> m_MakeReader; ///< Set by the test seam only
    std::function<void(Core::Event&)> m_RaiseEvent;                                    ///< Set by the test seam only
    std::shared_ptr<Domain::ConnectionsModel> m_Model;
    std::shared_ptr<SamplingGate> m_Gate;                 // open while the tab is shown
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler; // created when the tab first shows
    std::shared_ptr<const Domain::ConnectionsPublication> m_Publication;
    ConnectionsViewState m_ViewState;
    std::chrono::milliseconds m_Interval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS}; // the Settings interval
};

} // namespace App
