#pragma once

#include "App/Panel.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/StartupActionsView.h"
#include "App/Panels/StartupView.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/StartupModel.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"

#include <functional>
#include <memory>

namespace App
{

/// The probe and actions StartupPanel::onAttach() builds from (#1721).
struct StartupPanelPlatform
{
    std::unique_ptr<Platform::IStartupProbe> probe;
    std::shared_ptr<Platform::IStartupActions> actions; ///< Shared, as StartupActionsView holds it
};

/// The Startup tab (#801; enable / disable in phase 2). Owns the StartupModel and samples it on its own
/// BackgroundSampler at Sampling::STARTUP_REFRESH_MS, only while the tab is shown. As in the Services
/// tab, the sampler is started the first time the tab shows; hidden, a SamplingGate keeps it from
/// reading anything, so switching tabs never joins the sampler thread on the UI thread (only
/// onDetach() does), however long a sample (registry, shortcuts, version resources) takes.
class StartupPanel : public Panel
{
  public:
    StartupPanel();

    /// Test seam (#1721): onAttach() takes its probe and actions from @p makePlatform instead (tests:
    /// fakes, so no real startup entry is read or changed). The App layer stays the only place the
    /// platform's are made, still at onAttach().
    explicit StartupPanel(std::function<StartupPanelPlatform()> makePlatform);
    ~StartupPanel() override;

    StartupPanel(const StartupPanel&) = delete;
    StartupPanel& operator=(const StartupPanel&) = delete;
    StartupPanel(StartupPanel&&) = delete;
    StartupPanel& operator=(StartupPanel&&) = delete;

    void onAttach() override;
    void onDetach() override;
    void onEvent(Core::Event& event) override;
    void renderContent() override;

  private:
    void setActive(bool active);

    std::function<StartupPanelPlatform()> m_MakePlatform; ///< Set by the test seam only
    std::shared_ptr<Domain::StartupModel> m_Model;
    std::shared_ptr<SamplingGate> m_Gate;                 // open while the tab is shown
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler; // created when the tab first shows
    std::shared_ptr<const Domain::StartupPublication> m_Publication;
    StartupViewState m_ViewState;
    std::unique_ptr<StartupActionsView> m_Actions; // the row actions and their worker (#801, phase 2)
};

} // namespace App
