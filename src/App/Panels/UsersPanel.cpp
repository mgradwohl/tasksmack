#include "UsersPanel.h"

#include "App/Panel.h"
#include "App/PanelPlatforms.h"
#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/SessionActionControl.h"
#include "App/Panels/UsersView.h"
#include "App/SyntheticScenario.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ProcessModel.h"
#include "Domain/UsersModel.h"
#include "Platform/IUserSessions.h"
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

UsersPanel::UsersPanel() : Panel("Users")
{}

UsersPanel::UsersPanel(std::function<std::shared_ptr<Platform::IUserSessions>()> makeSessions, std::function<void(Core::Event&)> raiseEvent)
    : Panel("Users"), m_MakeSessions(std::move(makeSessions)), m_RaiseEvent(std::move(raiseEvent))
{}

UsersPanel::~UsersPanel()
{
    m_Sampler.reset(); // the sampler stops before the model it samples goes away
}

void UsersPanel::raiseEvent(Core::Event& event)
{
    if (m_RaiseEvent)
    {
        m_RaiseEvent(event);
        return;
    }
    Core::Application::get().raiseEvent(event);
}

void UsersPanel::onAttach()
{
    // The composition root's one sessions creation for this panel; sampling waits for the tab to show.
    m_Sessions = m_MakeSessions ? m_MakeSessions() : makeUsersPanelSessions(Synthetic::activeScenario());
    m_Model = std::make_shared<Domain::UsersModel>(m_Sessions);
    m_Gate = std::make_shared<SamplingGate>(m_Model);
    m_Actions.emplace(m_Sessions);
}

void UsersPanel::onDetach()
{
    m_Sampler.reset(); // joins the sampler thread: the one place it is waited for
    m_Actions.reset(); // stops and waits for an action in flight
    m_Gate.reset();
    m_Publication.reset();
    m_Model.reset();
    m_Sessions.reset();
}

void UsersPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            setActive(e.tabName() == "Users");
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

void UsersPanel::setActive(bool active)
{
    if (!m_Gate || !m_Model->hasSessions())
    {
        return;
    }
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
    config.threadName = std::string(Platform::USERS_SAMPLER_THREAD_NAME);
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(config);
    m_Sampler->addSamplable(m_Gate, "users");
    m_Sampler->start();
}

void UsersPanel::renderContent()
{
    if (!m_Model)
    {
        return;
    }
    if (!m_Publication || m_Publication->version != m_Model->version())
    {
        m_Publication = m_Model->publication();
    }
    // The processes' newest generation, shared, not copied (the Processes panel samples it).
    if (const std::shared_ptr<Domain::ProcessModel> processes = m_ProcessModel.lock(); processes != nullptr)
    {
        static_cast<void>(processes->tryCopySnapshotsIfNewer(m_ProcessesVersion, m_Processes, m_ProcessesVersion));
    }
    if (m_Publication)
    {
        refreshUserRows(*m_Publication, m_Processes, m_ProcessesVersion, m_ViewState);
    }
    if (m_Actions)
    {
        m_Actions->update(ImGui::GetIO().DeltaTime);
    }
    renderActionLine();
    const bool canAct = m_Sessions != nullptr && m_Sessions->canAct(Platform::SessionAction::SignOut);
    if (const std::optional<UsersViewAction> action = renderUsersView(m_Publication.get(), m_Model->hasSessions(), canAct, m_ViewState))
    {
        if (action->selectPid != 0)
        {
            Core::ProcessSelectedEvent selected(action->selectPid, action->selectStartTimeTicks);
            raiseEvent(selected);
            if (action->showDetails)
            {
                Core::ShowProcessDetailsEvent show;
                raiseEvent(show);
            }
        }
        if (m_Actions && action->lock.has_value())
        {
            m_Actions->lock(*action->lock);
        }
        if (m_Actions && action->signOut.has_value())
        {
            m_Actions->requestSignOut(*action->signOut);
        }
    }
    renderSignOutConfirm();
}

void UsersPanel::renderActionLine()
{
    if (!m_Actions)
    {
        return;
    }
    const auto& scheme = UI::Theme::get().scheme();
    if (m_Actions->busy())
    {
        ImGui::TextColored(scheme.textMuted, "%s", m_Actions->pendingText().c_str());
        return;
    }
    const Detail::ActionResultMessage& result = m_Actions->result();
    if (result.empty())
    {
        return;
    }
    ImGui::TextColored(result.ok ? scheme.textSuccess : scheme.textError, "%s", result.text.c_str());
    if (m_Actions->takeDone() && m_Sampler)
    {
        m_Sampler->requestRefresh(); // the session's new state without waiting a sample
    }
    // A refusal an administrator could finish (#2304): the lock and the words, never colour alone.
    if (m_Actions->offersAuthenticateAndRetry())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(AuthorizeWording::RETRY_LABEL))
        {
            m_Actions->retryAuthorized();
        }
        ImGui::SetItemTooltip("%s", AuthorizeWording::RETRY_TOOLTIP);
    }
}

void UsersPanel::renderSignOutConfirm()
{
    if (!m_Actions || !m_Actions->pending().has_value())
    {
        return;
    }
    switch (ProcessActionConfirm::renderLabelled(m_Actions->showConfirm(), "Sign Out", true, "Sign out", m_Actions->question()))
    {
    case ProcessActionConfirm::Outcome::Confirmed:
        m_Actions->confirm();
        break;
    case ProcessActionConfirm::Outcome::Cancelled:
        m_Actions->cancel();
        break;
    case ProcessActionConfirm::Outcome::None:
        break;
    }
}

} // namespace App
