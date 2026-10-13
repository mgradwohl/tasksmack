#include "ServicesPanel.h"

#include "App/Panel.h"
#include "App/Panels/ProcessIconView.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/ServiceActionsView.h"
#include "App/Panels/ServicesView.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/SamplingConfig.h"
#include "Domain/ServiceModel.h"
#include "Platform/Factory.h"
#include "Platform/ThreadName.h"

#include <imgui.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace App
{

ServicesPanel::ServicesPanel() : Panel("Services")
{}

ServicesPanel::ServicesPanel(std::function<ServicesPanelPlatform()> makePlatform)
    : Panel("Services"), m_MakePlatform(std::move(makePlatform))
{}

ServicesPanel::~ServicesPanel()
{
    // The sampler stops before the model it samples goes away (it holds only a weak_ptr anyway).
    m_Sampler.reset();
}

void ServicesPanel::onAttach()
{
    // The composition root's one probe creation for this panel; sampling waits for the tab to show.
    // An injected factory (tests, #1721) replaces the platform's.
    ServicesPanelPlatform platform =
        m_MakePlatform ? m_MakePlatform()
                       : ServicesPanelPlatform{.probe = Platform::makeServiceProbe(), .actions = Platform::makeServiceActions()};
    m_Model = std::make_shared<Domain::ServiceModel>(std::move(platform.probe));
    m_Gate = std::make_shared<SamplingGate>(m_Model);
    m_Actions = std::make_unique<ServiceActionsView>(std::move(platform.actions));
}

void ServicesPanel::onDetach()
{
    // An action still running is cancelled first, so the wait below is one platform poll (#1591), not
    // the service's full stop or restart. The request already sent to the service manager stands.
    if (m_Actions)
    {
        m_Actions->cancel();
    }
    m_Actions.reset();
    m_Sampler.reset(); // joins the sampler thread: the one place it is waited for
    m_Gate.reset();
    m_TabShown = false;
    m_BackgroundDemand = false;
    m_Publication.reset();
    m_Model.reset();
}

void ServicesPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            const bool shown = e.tabName() == "Services";
            const bool showing = shown && !m_TabShown;
            m_TabShown = shown;
            updateSampling(showing); // current as soon as the tab shows again
            return false;
        });
    // Process Details' Services section asked for a service (#1880); ShellLayer brings the tab forward.
    dispatcher.dispatch<Core::ShowServiceEvent>(
        [this](Core::ShowServiceEvent& e)
        {
            selectService(e.serviceName());
            return false;
        });
}

void ServicesPanel::setBackgroundDemand(bool wanted)
{
    if (wanted == m_BackgroundDemand)
    {
        return;
    }
    m_BackgroundDemand = wanted;
    updateSampling(false);
}

void ServicesPanel::selectService(std::string name)
{
    requestShowService(m_ViewState, std::move(name));
}

void ServicesPanel::updateSampling(bool refreshNow)
{
    if (!m_Gate || !m_Model->capabilities().canEnumerate)
    {
        return;
    }
    const bool wanted = m_TabShown || m_BackgroundDemand;
    const bool opening = wanted && !m_Gate->isOpen();
    // Closing the gate returns at once; a sample in progress finishes on the sampler thread.
    m_Gate->setOpen(wanted);
    if (!wanted)
    {
        return;
    }
    if (m_Sampler)
    {
        if (opening || refreshNow)
        {
            m_Sampler->requestRefresh();
        }
        return;
    }
    // The first sample runs straight away, so the list is current as soon as the tab first shows.
    Domain::SamplerConfig config;
    config.interval = std::chrono::milliseconds(Domain::Sampling::SERVICES_REFRESH_MS);
    config.threadName = std::string(Platform::SERVICE_SAMPLER_THREAD_NAME);
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(config);
    m_Sampler->addSamplable(m_Gate, "services");
    m_Sampler->start();
}

void ServicesPanel::renderContent()
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
    m_Actions->tick(ImGui::GetIO().DeltaTime);
    if (m_Actions->takeFinished())
    {
        // The action changed a state or a start type: re-read now, configuration included.
        m_Model->requestConfigReread();
        if (m_Sampler)
        {
            m_Sampler->requestRefresh();
        }
    }
    if (m_Icons != nullptr)
    {
        m_Icons->beginFrame(); // The icons' size follows this tab's font
    }
    static_cast<void>(renderServicesView(m_Publication.get(), m_Model->capabilities(), m_ViewState, m_Actions.get(), m_Icons));
    m_Actions->renderConfirmation();
}

} // namespace App
