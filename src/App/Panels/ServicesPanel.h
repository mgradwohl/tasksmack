#pragma once

#include "App/Panel.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/ServiceActionsView.h"
#include "App/Panels/ServicesView.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ServiceModel.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"

#include <functional>
#include <memory>

namespace App
{

/// The probe and actions ServicesPanel::onAttach() builds from (#1721).
struct ServicesPanelPlatform
{
    std::unique_ptr<Platform::IServiceProbe> probe;
    std::shared_ptr<Platform::IServiceActions> actions; ///< Shared, as ServiceActionsView holds it
};

/// The Services tab (#800; actions #1577). Owns the ServiceModel and samples it on its own
/// BackgroundSampler at Sampling::SERVICES_REFRESH_MS, only while the tab is shown. The sampler is
/// started the first time the tab shows; hidden, a SamplingGate keeps it from reading any service,
/// so switching tabs never joins the sampler thread on the UI thread (only onDetach() does).
class ServicesPanel : public Panel
{
  public:
    ServicesPanel();

    /// Test seam (#1721): onAttach() takes its probe and actions from @p makePlatform instead (tests:
    /// fakes, so no real service is read or changed). The App layer stays the only place the
    /// platform's are made, still at onAttach().
    explicit ServicesPanel(std::function<ServicesPanelPlatform()> makePlatform);
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

    std::function<ServicesPanelPlatform()> m_MakePlatform; ///< Set by the test seam only
    std::shared_ptr<Domain::ServiceModel> m_Model;
    std::shared_ptr<SamplingGate> m_Gate;                 // open while the tab is shown
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler; // created when the tab first shows
    std::shared_ptr<const Domain::ServicePublication> m_Publication;
    ServicesViewState m_ViewState;
    std::unique_ptr<ServiceActionsView> m_Actions; // the row actions and their worker (#1577)
};

} // namespace App
