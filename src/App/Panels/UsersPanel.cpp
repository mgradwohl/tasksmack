#include "UsersPanel.h"

#include "App/Panel.h"
#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/SessionActionControl.h"
#include "App/Panels/UserSessionsFeed.h"
#include "App/Panels/UsersView.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/ProcessModel.h"
#include "Domain/UsersModel.h"
#include "Platform/IUserSessions.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace App
{

UsersPanel::UsersPanel(UserSessionsFeed& feed) : Panel("Users"), m_Feed(feed)
{}

UsersPanel::UsersPanel(UserSessionsFeed& feed, std::function<void(Core::Event&)> raiseEvent)
    : Panel("Users"), m_Feed(feed), m_RaiseEvent(std::move(raiseEvent))
{}

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
    // The composition root's one sessions creation, the feed's (#3377); sampling waits for the tab to show.
    m_Feed.start();
}

void UsersPanel::onDetach()
{
    m_Feed.stop(); // joins the sampler thread, then waits for an action in flight
    m_Publication.reset();
}

void UsersPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            m_Feed.setUsersTabShown(e.tabName() == "Users");
            return false;
        });
    // Sampled at the interval chosen in Settings, like every live figure (#2731).
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            m_Feed.setInterval(std::chrono::milliseconds(e.getIntervalMs()));
            return false;
        });
}

void UsersPanel::renderContent()
{
    const Domain::UsersModel* const model = m_Feed.model();
    if (model == nullptr)
    {
        return;
    }
    if (!m_Publication || m_Publication->version != model->version())
    {
        m_Publication = model->publication();
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
    SessionActionControl* const actions = m_Feed.actions();
    if (actions != nullptr)
    {
        actions->update(ImGui::GetIO().DeltaTime);
    }
    renderActionLine();
    // Each platform's own items (#3076): Lock on Linux, Disconnect on Windows, Sign out on both.
    const SessionMenu menu{
        .lock = m_Feed.canAct(Platform::SessionAction::Lock),
        .disconnect = m_Feed.canAct(Platform::SessionAction::Disconnect),
        .signOut = m_Feed.canAct(Platform::SessionAction::SignOut),
    };
    if (const std::optional<UsersViewAction> action = renderUsersView(m_Publication.get(), model->hasSessions(), menu, m_ViewState))
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
        if (actions != nullptr && action->lock.has_value())
        {
            actions->lock(*action->lock);
        }
        if (actions != nullptr && action->signOut.has_value())
        {
            actions->requestSignOut(*action->signOut);
        }
        if (actions != nullptr && action->disconnect.has_value())
        {
            actions->requestDisconnect(*action->disconnect);
        }
    }
    renderConfirm();
}

void UsersPanel::renderActionLine()
{
    SessionActionControl* const actions = m_Feed.actions();
    if (actions == nullptr)
    {
        return;
    }
    const auto& scheme = UI::Theme::get().scheme();
    if (actions->busy())
    {
        ImGui::TextColored(scheme.textMuted, "%s", actions->pendingText().c_str());
        return;
    }
    const Detail::ActionResultMessage& result = actions->result();
    if (result.empty())
    {
        return;
    }
    ImGui::TextColored(result.ok ? scheme.textSuccess : scheme.textError, "%s", result.text.c_str());
    if (actions->takeDone())
    {
        m_Feed.requestRefresh(); // the session's new state without waiting a sample
    }
    // A refusal an administrator could finish (#2304): the lock and the words, never colour alone.
    if (actions->offersAuthenticateAndRetry())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(AuthorizeWording::RETRY_LABEL))
        {
            actions->retryAuthorized();
        }
        ImGui::SetItemTooltip("%s", AuthorizeWording::RETRY_TOOLTIP);
    }
}

void UsersPanel::renderConfirm()
{
    SessionActionControl* const actions = m_Feed.actions();
    if (actions == nullptr || !actions->pending().has_value())
    {
        return;
    }
    // Sign out loses unsaved work, so its button is the destructive one; a Disconnect keeps every program.
    const bool signOut = actions->pendingAction() == Platform::SessionAction::SignOut;
    const char* const title = sessionActionWords(actions->pendingAction()).title;
    switch (ProcessActionConfirm::renderLabelled(actions->showConfirm(), title, signOut, title, actions->question()))
    {
    case ProcessActionConfirm::Outcome::Confirmed:
        actions->confirm();
        break;
    case ProcessActionConfirm::Outcome::Cancelled:
        actions->cancel();
        break;
    case ProcessActionConfirm::Outcome::None:
        break;
    }
}

} // namespace App
