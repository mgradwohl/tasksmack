#include "ElevationNoticeLayer.h"

#include "App/ElevationNoticeDialog.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"

#include <spdlog/spdlog.h>

namespace App
{

ElevationNoticeLayer::ElevationNoticeLayer() : Core::Layer("ElevationNoticeLayer")
{}

ElevationNoticeLayer::~ElevationNoticeLayer() = default;

void ElevationNoticeLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No-op
}

void ElevationNoticeLayer::onRender()
{
    if (ElevationNoticeDialog::render(m_State) && m_State.dontShowAgain)
    {
        auto& config = UserConfig::get();
        config.settings().showPrivilegeNotice = false;
        config.save();
        spdlog::info("ElevationNoticeLayer: user suppressed privilege notice");
    }
}

void ElevationNoticeLayer::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);

    dispatcher.dispatch<Core::OpenElevationNoticeEvent>(
        [this](Core::OpenElevationNoticeEvent&)
        {
            requestOpen();
            return false; // Don't consume; allow other layers to observe
        });
}

void ElevationNoticeLayer::requestOpen()
{
    m_State.openRequested = true;
    m_State.dontShowAgain = false;
}

} // namespace App
