#pragma once

#include "App/AccessNoticeDialog.h"
#include "App/Panels/BackgroundTask.h"
#include "Core/Layer.h"
#include "Platform/IElevatedRestart.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace App
{

/// The startup notice's layer, Standard or Administrator (#2728, AccessNoticeDialog): shown once at startup when
/// capabilities().hasReducedPrivileges is true and UserConfig::showPrivilegeNotice is true, and again from the
/// status bar's Standard chip. The user can suppress future notices via its "Don't show this again" checkbox.
///
/// On Windows its Restart as administrator... runs IElevatedRestart::restart() on a worker, as the UAC prompt
/// blocks it; once Windows has started the elevated copy this one closes (closeApplication), saving its settings
/// as any close does. A declined prompt, or a failure, leaves TaskSmack as Standard, and the notice says so.
///
/// Thread safety: All layer lifecycle methods are called from the main thread only,
/// as required by SDL and ImGui.
class ElevationNoticeLayer : public Core::Layer
{
  public:
    /// @p restart Windows' Restart as administrator, or null where there is none (Linux, a synthetic run).
    /// @p closeApplication Closes TaskSmack once the elevated copy runs; null: the window's close request.
    explicit ElevationNoticeLayer(std::shared_ptr<Platform::IElevatedRestart> restart = nullptr,
                                  std::function<void()> closeApplication = {});
    ~ElevationNoticeLayer() override;

    ElevationNoticeLayer(const ElevationNoticeLayer&) = delete;
    ElevationNoticeLayer& operator=(const ElevationNoticeLayer&) = delete;
    ElevationNoticeLayer(ElevationNoticeLayer&&) = delete;
    ElevationNoticeLayer& operator=(ElevationNoticeLayer&&) = delete;

    void onRender() override;
    void onEvent(Core::Event& event) override;

  private:
    void requestOpen();
    void startRestart();
    void takeRestartResult();
    static void suppressFutureNotices();

    AccessNoticeDialog::State m_AccessState;
    std::shared_ptr<Platform::IElevatedRestart> m_Restart;
    std::function<void()> m_CloseApplication;
    BackgroundTask<Platform::ElevatedRestartResult> m_RestartWorker;
};

} // namespace App
