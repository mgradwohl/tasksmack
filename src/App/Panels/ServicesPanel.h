#pragma once

#include "App/Panel.h"
#include "App/Panels/ProcessIconView.h"
#include "App/Panels/SamplingGate.h"
#include "App/Panels/ServiceActionsView.h"
#include "App/Panels/ServicesView.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ServiceModel.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"

#include <functional>
#include <memory>
#include <string>

namespace App
{

/// The probe and actions ServicesPanel::onAttach() builds from (#1721).
struct ServicesPanelPlatform
{
    std::unique_ptr<Platform::IServiceProbe> probe;
    std::shared_ptr<Platform::IServiceActions> actions; ///< Shared, as ServiceActionsView holds it
};

/// The Services tab (#800; actions #1577). Owns the ServiceModel and samples it on its own
/// BackgroundSampler at Sampling::SERVICES_REFRESH_MS, only while the tab is shown or something else
/// needs the list (setBackgroundDemand(), #1880). The sampler is started the first time either is
/// so; otherwise a SamplingGate keeps it from reading any service, so switching tabs never joins the
/// sampler thread on the UI thread (only onDetach() does).
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

    /// The icons beside each row's name (#1800): the Processes tab's view, shared so every tab uses
    /// one icon cache, one set of textures and the "Show process icons" setting (which ShellLayer
    /// applies to it each frame). Set by ShellLayer, which owns both panels; nullptr: no icons.
    void setIconView(ProcessIconView* icons) noexcept
    {
        m_Icons = icons;
    }

    /// The service list (#1880): ShellLayer hands it to the process model, which says from it which
    /// services each process hosts, and to Process Details. Null before onAttach() and after onDetach().
    [[nodiscard]] std::shared_ptr<const Domain::ServiceModel> serviceModel() const noexcept
    {
        return m_Model;
    }

    /// Whether the list is wanted while the tab is hidden (#1880): ShellLayer sets it each frame while
    /// the Processes table shows its Services column or Process Details' Services section is open. It
    /// is then read at the tab's own cadence, SERVICES_REFRESH_MS, never per frame; false again, it is
    /// read only while the tab is shown. Cheap to call every frame: only a change does anything.
    void setBackgroundDemand(bool wanted);

    /// Selects the service named @p name and scrolls it into view the next time the table is drawn
    /// (#1880). A ShowServiceEvent does this too.
    void selectService(std::string name);

    /// Whether the list is being sampled now: the tab is shown or setBackgroundDemand() wants it, and
    /// the platform can list services (tests).
    [[nodiscard]] bool isSampling() const noexcept
    {
        return m_Gate != nullptr && m_Gate->isOpen();
    }

  private:
    /// Opens the gate while the tab is shown or the list is wanted elsewhere, and closes it otherwise,
    /// starting the sampler the first time it opens. Opening it, or @p refreshNow (the tab has just
    /// been shown), asks for a read straight away.
    void updateSampling(bool refreshNow);

    std::function<ServicesPanelPlatform()> m_MakePlatform; ///< Set by the test seam only
    std::shared_ptr<Domain::ServiceModel> m_Model;
    std::shared_ptr<SamplingGate> m_Gate;                 // open while the tab is shown
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler; // created when the tab first shows
    std::shared_ptr<const Domain::ServicePublication> m_Publication;
    ServicesViewState m_ViewState;
    std::unique_ptr<ServiceActionsView> m_Actions; // the row actions and their worker (#1577)
    ProcessIconView* m_Icons = nullptr;            // not owned: ShellLayer's Processes panel owns it
    bool m_TabShown = false;                       // the Services tab is the active one
    bool m_BackgroundDemand = false;               // the list is wanted with the tab hidden (#1880)
};

} // namespace App
