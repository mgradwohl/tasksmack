#pragma once

#include "App/Panel.h"
#include "App/Panels/SystemInfoView.h"
#include "Core/GraphicsHostInfo.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/CrashHistory.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SensorModel.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISensorProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace App
{

/// The System tab (#1399): static facts about the machine, in titled sections. Owns the
/// SystemInfoModel; the facts are read on a worker thread the first time the tab shows, and again
/// only when Refresh is clicked: never per tick or per frame. Also owns the SensorModel and its
/// sampler (#1522), started at onAttach() so the Sensors section's minimum and maximum cover the time
/// since TaskSmack started; it samples at the refresh interval while the tab shows, half as often
/// otherwise.
class SystemInfoPanel : public Panel
{
  public:
    SystemInfoPanel();

    /// Test seam (#1395): onAttach() takes its probe from @p makeProbe, and each new read's graphics
    /// facts come from @p queryHost, instead of the platform's probe and Core's window (tests: a fake
    /// probe and no window). The live sensors come from @p makeSensorProbe; without one there are none.
    /// The App layer stays the only place the platform's probes are made, still at onAttach().
    SystemInfoPanel(std::function<std::unique_ptr<Platform::ISystemInfoProbe>()> makeProbe,
                    std::function<Core::GraphicsHostInfo()> queryHost,
                    std::function<std::unique_ptr<Platform::ISensorProbe>()> makeSensorProbe = {});
    ~SystemInfoPanel() override;

    SystemInfoPanel(const SystemInfoPanel&) = delete;
    SystemInfoPanel& operator=(const SystemInfoPanel&) = delete;
    SystemInfoPanel(SystemInfoPanel&&) = delete;
    SystemInfoPanel& operator=(SystemInfoPanel&&) = delete;

    /// The crash cache Process Details shows (#1675), handed each read's crashes. Set by the composition
    /// root (ShellLayer) before onAttach(); none by default.
    void setCrashHistory(std::shared_ptr<Domain::CrashHistory> crashHistory) noexcept
    {
        m_CrashHistory = std::move(crashHistory);
    }

    void onAttach() override;
    void onDetach() override;
    void onEvent(Core::Event& event) override;
    void renderContent() override;

  private:
    /// Starts a read on a worker thread, unless one is already in flight.
    void startRead();
    /// Takes in a finished read; with @p wait, waits for one in flight first.
    void finishRead(bool wait);
    /// The sensor sampler's interval: the refresh interval while the tab shows, relaxed otherwise.
    void applySensorInterval();

    std::function<std::unique_ptr<Platform::ISystemInfoProbe>()> m_MakeProbe;   ///< Set by the test seam only
    std::function<Core::GraphicsHostInfo()> m_QueryHost;                        ///< Set by the test seam only
    std::function<std::unique_ptr<Platform::ISensorProbe>()> m_MakeSensorProbe; ///< Set by the test seam only
    bool m_TestSeam = false;
    std::shared_ptr<Domain::SensorModel> m_SensorModel;
    std::unique_ptr<Domain::BackgroundSampler> m_SensorSampler;
    std::chrono::milliseconds m_RefreshInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    bool m_IsActiveTab = false;
    std::shared_ptr<Domain::CrashHistory> m_CrashHistory;
    std::shared_ptr<Domain::SystemInfoModel> m_Model;
    // The read in flight, if any; it hands back only a failed read's message, copied on the worker.
    std::future<std::optional<std::string>> m_Pending;
    std::shared_ptr<const Domain::SystemInfoSnapshot> m_Snapshot;
    SystemInfoViewState m_ViewState;
};

} // namespace App
