#pragma once

#include "App/Panel.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/ServiceActionsView.h"
#include "App/Panels/ServicesView.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ServiceModel.h"

#include <memory>

namespace App
{

/// The Services tab (#800; actions #1577). Owns the ServiceModel and samples it on its own
/// BackgroundSampler at Sampling::SERVICES_REFRESH_MS, only while the tab is shown. The sampler is
/// started the first time the tab shows; hidden, a SamplingGate keeps it from reading any service,
/// so switching tabs never joins the sampler thread on the UI thread (only onDetach() does).
class ServicesPanel : public Panel
{
  public:
    ServicesPanel();
    ~ServicesPanel() override;

    ServicesPanel(const ServicesPanel&) = delete;
    ServicesPanel& operator=(const ServicesPanel&) = delete;
    ServicesPanel(ServicesPanel&&) = delete;
    ServicesPanel& operator=(ServicesPanel&&) = delete;

    void onAttach() override;
    void onDetach() override;
    void onEvent(Core::Event& event) override;
    void renderContent() override;

  private:
    void setActive(bool active);

    std::shared_ptr<Domain::ServiceModel> m_Model;
    std::shared_ptr<SamplingGate> m_Gate;                 // open while the tab is shown
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler; // created when the tab first shows
    std::shared_ptr<const Domain::ServicePublication> m_Publication;
    ServicesViewState m_ViewState;
    std::unique_ptr<ServiceActionsView> m_Actions; // the row actions and their worker (#1577)
};

} // namespace App
