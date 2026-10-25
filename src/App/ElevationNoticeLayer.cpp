#include "ElevationNoticeLayer.h"

#include "App/AccessChoice.h"
#include "App/AccessNoticeDialog.h"
#include "App/ElevationNoticeDialog.h"
#include "App/ElevationNoticeText.h"
#include "App/PanelPlatforms.h"
#include "App/SyntheticScenario.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <stop_token>
#include <string>
#include <thread>

namespace App
{

ElevationNoticeLayer::ElevationNoticeLayer() : Core::Layer("ElevationNoticeLayer")
{}

ElevationNoticeLayer::~ElevationNoticeLayer() = default;

void ElevationNoticeLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    collectAdministrator();
}

void ElevationNoticeLayer::onRender()
{
    if constexpr (AccessChoice::HOST == AccessChoice::AccessPlatform::Linux)
    {
        switch (AccessNoticeDialog::render(m_AccessState))
        {
        case AccessNoticeDialog::Choice::Standard:
            if (m_AccessState.dontShowAgain)
            {
                suppressFutureNotices();
            }
            break;
        case AccessNoticeDialog::Choice::Administrator:
            startAdministrator();
            break;
        case AccessNoticeDialog::Choice::None:
            break;
        }
    }
    else
    {
        if (ElevationNoticeDialog::render(m_State) && m_State.dontShowAgain)
        {
            suppressFutureNotices();
        }
    }
}

void ElevationNoticeLayer::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);

    dispatcher.dispatch<Core::OpenElevationNoticeEvent>(
        [this](Core::OpenElevationNoticeEvent& open)
        {
            requestOpen(open.missingPrivileges());
            return false; // Don't consume; allow other layers to observe
        });
}

void ElevationNoticeLayer::requestOpen(std::uint8_t missingPrivileges)
{
    if constexpr (AccessChoice::HOST == AccessChoice::AccessPlatform::Linux)
    {
        // The event comes only for a session that isn't root and whose user hasn't turned the notice off
        // (ShellLayer), so what's left to decide is whether the helper is there.
        m_Broker = privilegedBroker(Synthetic::activeScenario());
        AccessNoticeDialog::requestOpen(
            m_AccessState,
            AccessChoice::decideNotice({.platform = AccessChoice::AccessPlatform::Linux, .helperInstalled = m_Broker != nullptr}));
    }
    else
    {
        m_State.openRequested = true;
        m_State.dontShowAgain = false;
        m_State.bodyText = ElevationNoticeText::forCurrentPlatform(missingPrivileges);
    }
}

void ElevationNoticeLayer::suppressFutureNotices()
{
    auto& config = UserConfig::get();
    config.settings().showPrivilegeNotice = false;
    config.saveInBackground(); // written by a worker, never in this render path (#2916)
    spdlog::info("ElevationNoticeLayer: user suppressed privilege notice");
}

void ElevationNoticeLayer::startAdministrator()
{
    if (m_Broker == nullptr || m_Worker.joinable())
    {
        AccessNoticeDialog::finishAdministrator(m_AccessState, false, "the administrator helper isn't installed");
        return;
    }
    m_Worker = std::jthread(
        [this, broker = m_Broker](const std::stop_token& stopToken)
        {
            m_AdministratorResult = broker->holdForSession(stopToken);
            m_AdministratorDone.store(true, std::memory_order_release);
        });
}

void ElevationNoticeLayer::collectAdministrator()
{
    if (!m_AdministratorDone.exchange(false, std::memory_order_acquire))
    {
        return;
    }
    m_Worker.join();
    const bool succeeded = m_AdministratorResult.has_value();
    AccessNoticeDialog::finishAdministrator(m_AccessState, succeeded, succeeded ? std::string{} : m_AdministratorResult.error());
    if (succeeded)
    {
        spdlog::info("ElevationNoticeLayer: Administrator for this session: the helper is held until TaskSmack closes");
        if (m_AccessState.dontShowAgain)
        {
            suppressFutureNotices();
        }
    }
    else
    {
        spdlog::info("ElevationNoticeLayer: Administrator not started, still Standard: {}", m_AdministratorResult.error());
    }
}

} // namespace App
