#include "App/HelpLayer.h"

#include "App/HelpWindow.h"
#include "App/SelectOverride.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"

namespace App
{

HelpLayer::HelpLayer() : Core::Layer("HelpLayer")
{}

HelpLayer::~HelpLayer() = default;

void HelpLayer::onAttach()
{
    // TASKSMACK_OPEN=help (#172): open at startup, for an unattended capture.
    if (SelectOverride::startupDialog() == SelectOverride::StartupDialog::Help)
    {
        HelpWindow::requestOpen(m_State);
    }
}

void HelpLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No-op
}

void HelpLayer::onRender()
{
    if (HelpWindow::render(m_State) == HelpWindow::Action::OpenAbout)
    {
        Core::OpenAboutEvent event;
        Core::Application::get().raiseEvent(event);
    }
}

void HelpLayer::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::OpenHelpEvent>(
        [this](Core::OpenHelpEvent&)
        {
            HelpWindow::requestOpen(m_State);
            return false; // Don't consume
        });
}

} // namespace App
