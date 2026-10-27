#include "ConnectionsPanel.h"

#include "App/Panel.h"
#include "App/Panels/ConnectionsView.h"
#include "App/Panels/SamplingGate.h"
#include "App/SyntheticScenario.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ConnectionsModel.h"
#include "Platform/ISystemConnections.h"
#include "Platform/ThreadName.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace App
{

ConnectionsPanel::ConnectionsPanel() : Panel("Connections")
{}

ConnectionsPanel::ConnectionsPanel(std::function<std::unique_ptr<Platform::ISystemConnectionsReader>()> makeReader,
                                   std::function<void(Core::Event&)> raiseEvent)
    : Panel("Connections"), m_MakeReader(std::move(makeReader)), m_RaiseEvent(std::move(raiseEvent))
{}

void ConnectionsPanel::raiseEvent(Core::Event& event)
{
    if (m_RaiseEvent)
    {
        m_RaiseEvent(event);
        return;
    }
    Core::Application::get().raiseEvent(event);
}

ConnectionsPanel::~ConnectionsPanel()
{
    m_Sampler.reset(); // the sampler stops before the model it samples goes away
}

void ConnectionsPanel::onAttach()
{
    // The composition root's one reader creation for this panel; sampling waits for the tab to show.
    m_Model = std::make_shared<Domain::ConnectionsModel>(
        m_MakeReader ? m_MakeReader() : Synthetic::makeSystemConnectionsReader(Synthetic::activeScenario()));
    m_Gate = std::make_shared<SamplingGate>(m_Model);
}

void ConnectionsPanel::onDetach()
{
    m_Sampler.reset(); // joins the sampler thread: the one place it is waited for
    m_Gate.reset();
    m_Publication.reset();
    m_Model.reset();
}

void ConnectionsPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            setActive(e.tabName() == "Connections");
            return false;
        });
    // Sampled at the interval chosen in Settings, like every live figure (#2731).
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            m_Interval = std::chrono::milliseconds(e.getIntervalMs());
            if (m_Sampler)
            {
                m_Sampler->setInterval(m_Interval);
            }
            return false;
        });
}

void ConnectionsPanel::setActive(bool active)
{
    if (!m_Gate || !m_Model->hasConnections())
    {
        return;
    }
    // Closing the gate returns at once; a sample in progress finishes on the sampler thread.
    m_Gate->setOpen(active);
    if (!active)
    {
        return;
    }
    if (m_Sampler)
    {
        m_Sampler->requestRefresh(); // current as soon as the tab shows again
        return;
    }
    Domain::SamplerConfig config;
    config.interval = m_Interval;
    config.threadName = std::string(Platform::CONNECTIONS_SAMPLER_THREAD_NAME);
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(config);
    m_Sampler->addSamplable(m_Gate, "connections");
    m_Sampler->start();
}

void ConnectionsPanel::renderContent()
{
    if (!m_Model)
    {
        return;
    }
    // A new generation is adopted by pointer; the table caches its rows against its version.
    if (!m_Publication || m_Publication->version != m_Model->version())
    {
        m_Publication = m_Model->publication();
    }
    if (const std::optional<ConnectionsViewAction> action =
            renderConnectionsView(m_Publication.get(), m_Model->hasConnections(), m_ViewState))
    {
        Core::ProcessSelectedEvent selected(action->pid, action->startTimeTicks);
        raiseEvent(selected);
        if (action->showDetails)
        {
            Core::ShowProcessDetailsEvent show;
            raiseEvent(show);
        }
    }
}

} // namespace App
