#include "App/HelpLayer.h"

#include "App/BackgroundOpen.h"
#include "App/HelpWindow.h"
#include "App/SelectOverride.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"

#include <string>

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
    // A finished open frees the links; a failure is in the log, as the opener writes it.
    static_cast<void>(m_Open.poll());
    m_State.linksBusy = m_Open.busy();
    switch (HelpWindow::render(m_State))
    {
    case HelpWindow::Action::OpenAbout:
    {
        Core::OpenAboutEvent event;
        Core::Application::get().raiseEvent(event);
        break;
    }
    case HelpWindow::Action::OpenLink:
        // ShellExecuteW or xdg-open, on the worker: never on the UI thread (#3155).
        static_cast<void>(m_Open.start(BackgroundOpen::openUtf8(std::string(m_State.linkUrl))));
        break;
    case HelpWindow::Action::None:
        break;
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
    // TASKSMACK_DEMO's close=dialog (#2827): closes the window as its close button does.
    dispatcher.dispatch<Core::CloseDialogEvent>(
        [this](Core::CloseDialogEvent&)
        {
            m_State.open = false;
            return false; // The About dialog closes on it too
        });
}

} // namespace App
