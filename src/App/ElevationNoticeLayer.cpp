#include "ElevationNoticeLayer.h"

#include "App/AccessNoticeDialog.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/Window.h"
#include "Platform/IElevatedRestart.h"
#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace App
{

namespace
{

/// The restart's result for work that threw, or that no thread could be started for.
Platform::ElevatedRestartResult failedRestart(std::string detail)
{
    return {.outcome = Platform::ElevatedRestartResult::Outcome::Failed, .detail = std::move(detail)};
}

} // namespace

ElevationNoticeLayer::ElevationNoticeLayer(std::shared_ptr<Platform::IElevatedRestart> restart, std::function<void()> closeApplication)
    : Core::Layer("ElevationNoticeLayer"),
      m_Restart(std::move(restart)),
      m_CloseApplication(std::move(closeApplication)),
      m_RestartWorker(Platform::ELEVATED_RESTART_THREAD_NAME, &failedRestart)
{
    if (!m_CloseApplication)
    {
        m_CloseApplication = []
        {
            Core::Application::get().getWindow().requestClose();
        };
    }
    if (m_Restart != nullptr)
    {
        m_AccessState.restart = AccessNoticeDialog::Restart::Ready;
        m_AccessState.credentials = m_Restart->prompt() == Platform::AdministratorPrompt::Credentials;
    }
}

ElevationNoticeLayer::~ElevationNoticeLayer() = default;

void ElevationNoticeLayer::onRender()
{
    takeRestartResult();
    const AccessNoticeDialog::Action action = AccessNoticeDialog::render(m_AccessState);
    if (action == AccessNoticeDialog::Action::RestartAsAdministrator)
    {
        startRestart();
    }
    if (action != AccessNoticeDialog::Action::None && m_AccessState.dontShowAgain)
    {
        suppressFutureNotices();
    }
}

void ElevationNoticeLayer::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);

    dispatcher.dispatch<Core::OpenElevationNoticeEvent>(
        [this](Core::OpenElevationNoticeEvent& /*open*/)
        {
            requestOpen();
            return false; // Don't consume; allow other layers to observe
        });
}

void ElevationNoticeLayer::requestOpen()
{
    // The event comes only for a Standard session whose user hasn't turned the notice off, or from the Standard
    // chip (ShellLayer).
    AccessNoticeDialog::requestOpen(m_AccessState);
}

void ElevationNoticeLayer::startRestart()
{
    if (m_Restart == nullptr || m_AccessState.restart != AccessNoticeDialog::Restart::Ready)
    {
        return;
    }
    // The launching user's config directory, which the elevated copy keeps (#2728 decision 4).
    std::filesystem::path configDir = UserConfig::get().configPath().parent_path();
    m_AccessState.restart = AccessNoticeDialog::Restart::Waiting;
    m_AccessState.outcome.clear();
    spdlog::info("ElevationNoticeLayer: restarting as administrator");
    if (std::optional<Platform::ElevatedRestartResult> failed =
            m_RestartWorker.start([restart = m_Restart, configDir = std::move(configDir)] { return restart->restart(configDir); });
        failed.has_value())
    {
        m_AccessState.restart = AccessNoticeDialog::Restart::Ready;
        m_AccessState.outcome = AccessNoticeDialog::restartFailedNote(failed->detail);
    }
}

void ElevationNoticeLayer::takeRestartResult()
{
    const std::optional<Platform::ElevatedRestartResult> result = m_RestartWorker.poll();
    if (!result.has_value())
    {
        return;
    }
    m_AccessState.restart = AccessNoticeDialog::Restart::Ready;
    switch (result->outcome)
    {
    case Platform::ElevatedRestartResult::Outcome::Started:
        spdlog::info("ElevationNoticeLayer: the elevated TaskSmack started; closing this one");
        m_CloseApplication();
        break;
    case Platform::ElevatedRestartResult::Outcome::Declined:
        spdlog::info("ElevationNoticeLayer: the UAC prompt was declined; carrying on as Standard");
        m_AccessState.outcome = std::string(AccessNoticeDialog::DECLINED_NOTE);
        break;
    case Platform::ElevatedRestartResult::Outcome::Failed:
        spdlog::warn("ElevationNoticeLayer: couldn't restart as administrator: {}", result->detail);
        m_AccessState.outcome = AccessNoticeDialog::restartFailedNote(result->detail);
        break;
    }
}

void ElevationNoticeLayer::suppressFutureNotices()
{
    auto& config = UserConfig::get();
    config.settings().showPrivilegeNotice = false;
    config.saveInBackground(); // written by a worker, never in this render path (#2916)
    spdlog::info("ElevationNoticeLayer: user suppressed privilege notice");
}

} // namespace App
