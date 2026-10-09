#pragma once

#include "App/Panel.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/StartupView.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/StartupModel.h"

#include <memory>

namespace App
{

/// The Startup tab (#801, phase 1: read-only). Owns the StartupModel and samples it on its own
/// BackgroundSampler at Sampling::STARTUP_REFRESH_MS, only while the tab is shown. As in the Services
/// tab, the sampler is started the first time the tab shows; hidden, a SamplingGate keeps it from
/// reading anything, so switching tabs never joins the sampler thread on the UI thread (only
/// onDetach() does), however long a sample (registry, shortcuts, version resources) takes.
class StartupPanel : public Panel
{
  public:
    StartupPanel();
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

    std::shared_ptr<Domain::StartupModel> m_Model;
    std::shared_ptr<SamplingGate> m_Gate;                 // open while the tab is shown
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler; // created when the tab first shows
    std::shared_ptr<const Domain::StartupPublication> m_Publication;
    StartupViewState m_ViewState;
};

} // namespace App
