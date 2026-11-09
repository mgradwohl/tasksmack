#include "ElevationNoticeLayer.h"

#include "App/AccessChoice.h"
#include "App/AccessNoticeDialog.h"
#include "App/ElevationNoticeDialog.h"
#include "App/ElevationNoticeText.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"

#include <spdlog/spdlog.h>

#include <cstdint>

namespace App
{

ElevationNoticeLayer::ElevationNoticeLayer() : Core::Layer("ElevationNoticeLayer")
{}

ElevationNoticeLayer::~ElevationNoticeLayer() = default;

void ElevationNoticeLayer::onRender()
{
    if constexpr (AccessChoice::HOST == AccessChoice::AccessPlatform::Linux)
    {
        if (AccessNoticeDialog::render(m_AccessState) && m_AccessState.dontShowAgain)
        {
            suppressFutureNotices();
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
        // The event comes only for a session that isn't root and whose user hasn't turned the note off, or from
        // the Standard chip (ShellLayer).
        AccessNoticeDialog::requestOpen(m_AccessState);
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

} // namespace App
