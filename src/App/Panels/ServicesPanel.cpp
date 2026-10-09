#include "ServicesPanel.h"

#include "App/Panel.h"
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
#include <memory>
#include <string>

namespace App
{

ServicesPanel::ServicesPanel() : Panel("Services")
{}

ServicesPanel::~ServicesPanel()
{
    // The sampler stops before the model it samples goes away (it holds only a weak_ptr anyway).
    m_Sampler.reset();
}

void ServicesPanel::onAttach()
{
    // The composition root's one probe creation for this panel; sampling waits for the tab to show.
    m_Model = std::make_shared<Domain::ServiceModel>(Platform::makeServiceProbe());
    m_Gate = std::make_shared<SamplingGate>(m_Model);
    m_Actions = std::make_unique<ServiceActionsView>(Platform::makeServiceActions());
}

void ServicesPanel::onDetach()
{
    m_Actions.reset(); // waits for an action still running (bounded by the platform's timeout)
    m_Sampler.reset(); // joins the sampler thread: the one place it is waited for
    m_Gate.reset();
    m_Publication.reset();
    m_Model.reset();
}

void ServicesPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            setActive(e.tabName() == "Services");
            return false;
        });
}

void ServicesPanel::setActive(bool active)
{
    if (!m_Gate || !m_Model->capabilities().canEnumerate)
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
    static_cast<void>(renderServicesView(m_Publication.get(), m_Model->capabilities(), m_ViewState, m_Actions.get()));
    m_Actions->renderConfirmation();
}

} // namespace App
