#include "App/DemoLayer.h"

#include "App/DemoOverride.h"
#include "App/SelectOverride.h"
#include "App/ShellLayer.h"
#include "App/ThemeChange.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Layer.h"

#include <spdlog/spdlog.h>

#include <utility>
#include <variant>
#include <vector>

namespace App
{

namespace
{

/// Makes one step's request through the path the UI or a startup hook takes (DemoLayer.h).
class Issue
{
  public:
    Issue(ShellLayer& shell, const DemoOverride::Step& step) noexcept : m_Shell(&shell), m_Step(&step)
    {}

    void operator()(const DemoOverride::TabAction& action) const
    {
        // As TASKSMACK_TAB: by the tab's registered id or visible text, or an alias; asked for until
        // ImGui shows it selected.
        if (!m_Shell->requestMainTab(action.name))
        {
            spdlog::warn("{}: {}: '{}' names no tab; skipped", DemoOverride::ENV_VAR, DemoOverride::describe(*m_Step), action.name);
        }
    }

    void operator()(const DemoOverride::SelectAction& action) const
    {
        // As TASKSMACK_SELECT_PID/_NAME: the first snapshot that holds the process selects it as a
        // click does. The main tab stays; details= brings Process Details forward.
        m_Shell->requestSelection(action.target);
    }

    void operator()(const DemoOverride::DetailsAction& action) const
    {
        // The event the row menu's Details raises selects the Process Details tab (ShellLayer::onEvent);
        // its own tab as TASKSMACK_DETAILS_TAB asks for one.
        Core::ShowProcessDetailsEvent event;
        Core::Application::get().raiseEvent(event);
        m_Shell->requestDetailsTab(action.tab);
    }

    void operator()(const DemoOverride::MachineAction& action) const
    {
        // The machine tab as TASKSMACK_TAB=machine asks for it (its alias always resolves: the tab is
        // always registered), and its sub-tab as TASKSMACK_MACHINE_TAB asks for one (#2869).
        if (!m_Shell->requestMainTab("machine"))
        {
            spdlog::warn("{}: {}: the machine tab is not registered; skipped", DemoOverride::ENV_VAR, DemoOverride::describe(*m_Step));
            return;
        }
        m_Shell->requestMachineTab(action.tab);
    }

    void operator()(const DemoOverride::OpenAction& action) const
    {
        // The events the title bar's buttons, F1, F2 and Settings raise (#172).
        switch (action.dialog)
        {
        case SelectOverride::StartupDialog::Help:
        {
            Core::OpenHelpEvent event;
            Core::Application::get().raiseEvent(event);
            break;
        }
        case SelectOverride::StartupDialog::About:
        {
            Core::OpenAboutEvent event;
            Core::Application::get().raiseEvent(event);
            break;
        }
        case SelectOverride::StartupDialog::Settings:
        {
            Core::OpenSettingsEvent event;
            Core::Application::get().raiseEvent(event);
            break;
        }
        }
    }

    void operator()(const DemoOverride::CloseDialogAction& /*action*/) const
    {
        Core::CloseDialogEvent event;
        Core::Application::get().raiseEvent(event);
    }

    void operator()(const DemoOverride::ThemeAction& action) const
    {
        // As Settings' Apply: saved with the settings, applied at the next frame boundary.
        if (!changeTheme(action.id))
        {
            spdlog::warn("{}: {}: '{}' is not a loaded theme; skipped", DemoOverride::ENV_VAR, DemoOverride::describe(*m_Step), action.id);
        }
    }

    void operator()(const DemoOverride::SectionAction& action) const
    {
        m_Shell->requestDetailsSection(action.section);
    }

  private:
    ShellLayer* m_Shell;
    const DemoOverride::Step* m_Step;
};

} // namespace

DemoLayer::DemoLayer(std::vector<DemoOverride::Step> steps, ShellLayer& shell)
    : Core::Layer("DemoLayer"), m_Steps(std::move(steps)), m_Shell(&shell)
{}

void DemoLayer::onUpdate(float /*deltaTime*/)
{
    if (!m_Started)
    {
        return;
    }
    // Every step whose time has come, in order: before any layer renders, so this frame shows them.
    const double elapsed = Core::Application::getTime() - m_StartTime;
    while (m_Next < m_Steps.size() && static_cast<double>(m_Steps[m_Next].timeMs) / DemoOverride::MILLIS_PER_SECOND <= elapsed)
    {
        const DemoOverride::Step& step = m_Steps[m_Next];
        spdlog::info("{}: step {} of {}: {}", DemoOverride::ENV_VAR, m_Next + 1, m_Steps.size(), DemoOverride::describe(step));
        issue(step);
        ++m_Next;
    }
}

void DemoLayer::onPostRender()
{
    // The settle point, read where ScreenshotLayer reads it: this frame drew with every startup
    // selection in place, and the script's clock starts now.
    if (m_Started || m_Shell->startupSelectionPending())
    {
        return;
    }
    m_Started = true;
    m_StartTime = Core::Application::getTime();
    spdlog::info("{}: startup settled; the script's {} step(s) count from now", DemoOverride::ENV_VAR, m_Steps.size());
}

void DemoLayer::issue(const DemoOverride::Step& step) const
{
    std::visit(Issue(*m_Shell, step), step.action);
}

} // namespace App
