#include "StartupPanel.h"

#include "App/Panel.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/StartupView.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/SamplingConfig.h"
#include "Domain/StartupModel.h"
#include "Platform/Factory.h"
#include "Platform/ThreadName.h"

#include <chrono>
#include <memory>
#include <string>

namespace App
{

StartupPanel::StartupPanel() : Panel("Startup")
{}

StartupPanel::~StartupPanel()
{
    // The sampler stops before the model it samples goes away (it holds only a weak_ptr anyway).
    m_Sampler.reset();
}

void StartupPanel::onAttach()
{
    // The composition root's one probe creation for this panel; sampling waits for the tab to show.
    m_Model = std::make_shared<Domain::StartupModel>(Platform::makeStartupProbe());
    m_Gate = std::make_shared<SamplingGate>(m_Model);
}

void StartupPanel::onDetach()
{
    m_Sampler.reset(); // joins the sampler thread: the one place it is waited for
    m_Gate.reset();
    m_Publication.reset();
    m_Model.reset();
}

void StartupPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            setActive(e.tabName() == "Startup");
            return false;
        });
}

void StartupPanel::setActive(bool active)
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
    config.interval = std::chrono::milliseconds(Domain::Sampling::STARTUP_REFRESH_MS);
    config.threadName = std::string(Platform::STARTUP_SAMPLER_THREAD_NAME);
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(config);
    m_Sampler->addSamplable(m_Gate, "startup");
    m_Sampler->start();
}

void StartupPanel::renderContent()
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
    static_cast<void>(renderStartupView(m_Publication.get(), m_Model->capabilities(), m_ViewState));
}

} // namespace App
