#pragma once

#include "App/Panel.h"
#include "App/Panels/BackgroundTask.h"
#include "App/Panels/SystemInfoView.h"
#include "Core/Event.h"
#include "Core/GraphicsHostInfo.h"
#include "Core/SaveFileDialog.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/CrashHistory.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SensorModel.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ThreadName.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App
{

/// The System tab (#1399): static facts about the machine, in titled sections. Owns the
/// SystemInfoModel; the facts are read on a worker thread the first time the tab shows, and again
/// only when Refresh or "Show Wi-Fi details" (#1782) is clicked: never per tick or per frame.
class SystemInfoPanel : public Panel
{
  public:
    SystemInfoPanel();

    /// Test seam (#1395): onAttach() takes its probe from @p makeProbe, and each new read's graphics
    /// facts come from @p queryHost, instead of the platform's probe and Core's window (tests: a fake
    /// probe and no window). The App layer stays the only place the platform's probe is made, still at
    /// onAttach(). @p makeProbe is called twice per attach, for the page's probe on the UI thread and for
    /// the sensor sampler's (#2737) on that thread, so it must make a new probe each time and be safe to
    /// call from either.
    SystemInfoPanel(std::function<std::unique_ptr<Platform::ISystemInfoProbe>()> makeProbe,
                    std::function<Core::GraphicsHostInfo()> queryHost);
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

    /// Shows a save dialog (#1866): the file types, the starting location, and where the result goes,
    /// called exactly once, maybe on another thread.
    using SaveFileDialog = std::function<void(std::vector<Core::FileFilter>, std::string, Core::SaveFileCallback)>;

    /// Test seam (#1866): Save report... asks @p dialog where to save, instead of SDL's dialog.
    void setSaveFileDialog(SaveFileDialog dialog)
    {
        m_SaveFileDialog = std::move(dialog);
    }

    void onAttach() override;
    void onDetach() override;
    void onEvent(Core::Event& event) override;
    void renderContent() override;

  private:
    /// Starts a read of every section on a worker thread, unless one is already in flight.
    void startRead();
    /// Takes in a finished read; with @p wait, waits for one in flight first.
    void finishRead(bool wait);
    /// Save report... (#1866): asks where to save the page as it shows now.
    void startSaveReport();
    /// Writes the report once the dialog has answered, on the UI thread, and notes how it went.
    void finishSaveReport();

    /// Where the dialog's answer waits for the UI thread: the callback may run on another thread, and
    /// after the panel is gone (it holds the slot, not the panel).
    struct SaveSlot
    {
        std::mutex mutex;
        std::optional<Core::SaveFileResult> result; // guarded by mutex
    };

    std::function<std::unique_ptr<Platform::ISystemInfoProbe>()> m_MakeProbe; ///< Set by the test seam only
    std::function<Core::GraphicsHostInfo()> m_QueryHost;                      ///< Set by the test seam only
    std::shared_ptr<Domain::CrashHistory> m_CrashHistory;
    std::shared_ptr<Domain::SystemInfoModel> m_Model;
    /// The Failed result of a read that threw or couldn't start: its message.
    [[nodiscard]] static std::optional<std::string> failedRead(std::string detail)
    {
        return detail;
    }

    // The read in flight, if any; it hands back only a failed read's message, copied on the worker.
    BackgroundTask<std::optional<std::string>> m_Pending{Platform::SYSTEM_INFO_READ_THREAD_NAME, &failedRead, "unknown error"};
    // "Show Wi-Fi details" (#1782) or "Show public IP" (#1518) was clicked and its read hasn't started yet
    // (another read was in flight).
    bool m_OptInReadDue = false;
    // The sensors are a metric (#2737): read by a sampler of their own from attach to detach, at the
    // Settings refresh interval, whether or not the tab shows; the page draws their newest read.
    std::shared_ptr<Domain::SensorModel> m_Sensors;
    std::unique_ptr<Domain::BackgroundSampler> m_SensorSampler;
    std::shared_ptr<const Domain::SensorPublication> m_SensorPublication;
    std::chrono::milliseconds m_RefreshInterval{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
    std::shared_ptr<const Domain::SystemInfoSnapshot> m_Snapshot;
    SystemInfoViewState m_ViewState;
    SaveFileDialog m_SaveFileDialog; ///< Set by the test seam only
    std::shared_ptr<SaveSlot> m_SaveSlot = std::make_shared<SaveSlot>();
    bool m_SaveShowIdentifiers = false; // Show identifiers when Save report... was clicked
};

} // namespace App
