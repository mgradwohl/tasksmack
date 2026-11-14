#include "UserSessionsFeed.h"

#include "App/PanelPlatforms.h"
#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/SamplingGate.h"
#include "App/SyntheticScenario.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/UsersModel.h"
#include "Platform/IUserSessions.h"
#include "Platform/ThreadName.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace App
{

UserSessionsFeed::UserSessionsFeed() = default;

UserSessionsFeed::UserSessionsFeed(std::function<std::shared_ptr<Platform::IUserSessions>()> makeSessions)
    : m_MakeSessions(std::move(makeSessions))
{}

UserSessionsFeed::~UserSessionsFeed()
{
    stop(); // the sampler stops before the model it samples goes away
}

void UserSessionsFeed::start()
{
    if (m_Model)
    {
        return;
    }
    m_Sessions = m_MakeSessions ? m_MakeSessions() : makeSignedInSessions(Synthetic::activeScenario());
    m_Model = std::make_shared<Domain::UsersModel>(m_Sessions);
    m_Gate = std::make_shared<SamplingGate>(m_Model);
    m_Actions.emplace(m_Sessions);
    applyDemand(); // the view shown before start(), or still shown across a stop() and start()
}

void UserSessionsFeed::stop()
{
    m_Sampler.reset(); // joins the sampler thread: the one place it is waited for
    m_Actions.reset(); // stops and waits for an action in flight
    m_Gate.reset();
    m_Model.reset();
    m_Sessions.reset();
    m_Demanded = false; // the gate is gone; the view's inputs stay, so start() resumes what they demand
}

void UserSessionsFeed::setProcessesView(bool shown, ProcessGrouping::GroupBy groupBy)
{
    m_ProcessesTabShown = shown;
    m_GroupBy = groupBy;
    applyDemand();
}

void UserSessionsFeed::setInterval(std::chrono::milliseconds interval)
{
    m_Interval = interval;
    if (m_Sampler)
    {
        m_Sampler->setInterval(m_Interval);
    }
}

void UserSessionsFeed::requestRefresh()
{
    if (m_Sampler)
    {
        m_Sampler->requestRefresh();
    }
}

void UserSessionsFeed::applyDemand()
{
    if (!m_Gate || !m_Model->hasSessions())
    {
        return;
    }
    const bool demanded = ProcessGrouping::sessionsDemand(m_ProcessesTabShown, m_GroupBy);
    if (demanded == m_Demanded)
    {
        return; // only a change does anything
    }
    m_Demanded = demanded;
    m_Gate->setOpen(demanded);
    if (!demanded)
    {
        return; // the gate closes at once; the thread is joined only at stop()
    }
    if (m_Sampler)
    {
        m_Sampler->requestRefresh(); // current as soon as a view shows them again
        return;
    }
    Domain::SamplerConfig config;
    config.interval = m_Interval;
    config.threadName = std::string(Platform::USERS_SAMPLER_THREAD_NAME);
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(config);
    m_Sampler->addSamplable(m_Gate, "users");
    m_Sampler->start();
}

} // namespace App
