#include "ConnectionsPanel.h"

#include "App/Panel.h"
#include "App/PanelPlatforms.h"
#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ConnectionCloseControl.h"
#include "App/Panels/ConnectionsView.h"
#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/SamplingGate.h"
#include "App/SyntheticScenario.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ConnectionsModel.h"
#include "Platform/IProcessActions.h"
#include "Platform/ISystemConnections.h"
#include "Platform/ThreadName.h"
#include "UI/Theme.h"

#include <imgui.h>

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
                                   std::function<void(Core::Event&)> raiseEvent,
                                   std::function<std::unique_ptr<Platform::IProcessActions>()> makeActions)
    : Panel("Connections"),
      m_MakeReader(std::move(makeReader)),
      m_RaiseEvent(std::move(raiseEvent)),
      m_MakeActions(std::move(makeActions)),
      m_Seamed(true)
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
    // Close connection's actions (#3066): the test seam's, if any, else the composition root's.
    if (m_Seamed)
    {
        m_Actions = m_MakeActions ? m_MakeActions() : nullptr;
    }
    else
    {
        m_Actions = makeConnectionsPanelActions(Synthetic::activeScenario());
    }
    m_CanClose = m_Actions != nullptr && m_Actions->actionCapabilities().canCloseConnections;
}

void ConnectionsPanel::onDetach()
{
    m_Sampler.reset(); // joins the sampler thread: the one place it is waited for
    m_CloseControl.cancel();
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
    m_CloseControl.update(ImGui::GetIO().DeltaTime);
    renderCloseResultLine();
    if (const std::optional<ConnectionsViewAction> action =
            renderConnectionsView(m_Publication.get(), m_Model->hasConnections(), m_ViewState, m_CanClose))
    {
        if (action->close.has_value())
        {
            m_CloseControl.request(*action->close);
        }
        Core::ProcessSelectedEvent selected(action->pid, action->startTimeTicks);
        raiseEvent(selected);
        if (action->showDetails)
        {
            Core::ShowProcessDetailsEvent show;
            raiseEvent(show);
        }
    }
    renderCloseConfirm();
}

void ConnectionsPanel::renderCloseResultLine()
{
    const auto& scheme = UI::Theme::get().scheme();
    if (m_CloseControl.busy())
    {
        ImGui::TextColored(scheme.textMuted, "%s", m_CloseControl.pendingText().c_str());
        return;
    }
    const Detail::ActionResultMessage& result = m_CloseControl.result();
    if (result.empty())
    {
        return;
    }
    // The colour comes from the result's flag, not its text (#1203).
    ImGui::TextColored(result.ok ? scheme.textSuccess : scheme.textError, "%s", result.text.c_str());
    if (m_CloseControl.takeClosed() && m_Sampler)
    {
        m_Sampler->requestRefresh(); // the closed connection goes from the table without waiting a sample
    }
    // A permission refusal an administrator could redo (#2308): the lock and the words, never colour alone.
    if (m_CloseControl.offersAuthenticateAndRetry())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(AuthorizeWording::RETRY_LABEL))
        {
            m_CloseControl.retryAuthorized();
        }
        ImGui::SetItemTooltip("%s", AuthorizeWording::RETRY_TOOLTIP);
    }
}

void ConnectionsPanel::renderCloseConfirm()
{
    if (!m_CloseControl.pending().has_value())
    {
        return;
    }
    switch (ProcessActionConfirm::renderLabelled(
        m_CloseControl.showConfirm(), "Close Connection", true, "Close connection", m_CloseControl.question()))
    {
    case ProcessActionConfirm::Outcome::Confirmed:
        m_CloseControl.confirm(m_Actions.get());
        break;
    case ProcessActionConfirm::Outcome::Cancelled:
        m_CloseControl.cancel();
        break;
    case ProcessActionConfirm::Outcome::None:
        break;
    }
}

} // namespace App
