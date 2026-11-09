#pragma once

#include "App/AccessNoticeDialog.h"
#include "App/ElevationNoticeDialog.h"
#include "Core/Layer.h"

#include <cstdint>

namespace App
{

/// The startup notice's layer: shown once at startup when capabilities().hasReducedPrivileges is true
/// and UserConfig::showPrivilegeNotice is true. The user can suppress future notices via its "Don't
/// show this again" checkbox.
///
/// On Linux it is the Standard note (#2728, #3580, AccessNoticeDialog): a session never becomes Administrator after it
/// starts, so it says how to start one.
/// On Windows it is still the Limited Data notice (ElevationNoticeDialog) until TaskSmack can restart
/// itself elevated, which is what Administrator means there.
///
/// Thread safety: All layer lifecycle methods are called from the main thread only,
/// as required by SDL and ImGui.
class ElevationNoticeLayer : public Core::Layer
{
  public:
    ElevationNoticeLayer();
    ~ElevationNoticeLayer() override;

    ElevationNoticeLayer(const ElevationNoticeLayer&) = delete;
    ElevationNoticeLayer& operator=(const ElevationNoticeLayer&) = delete;
    ElevationNoticeLayer(ElevationNoticeLayer&&) = delete;
    ElevationNoticeLayer& operator=(ElevationNoticeLayer&&) = delete;

    void onRender() override;
    void onEvent(Core::Event& event) override;

  private:
    void requestOpen(std::uint8_t missingPrivileges);
    static void suppressFutureNotices();

    ElevationNoticeDialog::State m_State;
    AccessNoticeDialog::State m_AccessState;
};

} // namespace App
