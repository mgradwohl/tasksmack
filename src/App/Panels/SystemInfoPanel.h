#pragma once

#include "App/Panel.h"
#include "App/Panels/SystemInfoView.h"
#include "Domain/SystemInfoModel.h"

#include <future>
#include <memory>

namespace App
{

/// The System tab (#1399): static facts about the machine, in titled sections. Owns the
/// SystemInfoModel; the facts are read on a worker thread the first time the tab shows, and again
/// only when Refresh is clicked: never per tick or per frame.
class SystemInfoPanel : public Panel
{
  public:
    SystemInfoPanel();
    ~SystemInfoPanel() override;

    SystemInfoPanel(const SystemInfoPanel&) = delete;
    SystemInfoPanel& operator=(const SystemInfoPanel&) = delete;
    SystemInfoPanel(SystemInfoPanel&&) = delete;
    SystemInfoPanel& operator=(SystemInfoPanel&&) = delete;

    void onAttach() override;
    void onDetach() override;
    void onEvent(Core::Event& event) override;
    void renderContent() override;

  private:
    /// Starts a read on a worker thread, unless one is already in flight.
    void startRead();
    /// Takes in a finished read; with @p wait, waits for one in flight first.
    void finishRead(bool wait);

    std::shared_ptr<Domain::SystemInfoModel> m_Model;
    std::future<void> m_Pending; // the read in flight, if any
    std::shared_ptr<const Domain::SystemInfoSnapshot> m_Snapshot;
    SystemInfoViewState m_ViewState;
};

} // namespace App
