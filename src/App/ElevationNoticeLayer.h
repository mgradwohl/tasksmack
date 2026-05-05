#pragma once

#include "Core/Layer.h"

namespace App
{

/// Modal dialog layer that notifies the user when TaskSmack is running without
/// elevated privileges (non-root on Linux, non-admin on Windows).
///
/// Shown once at startup when capabilities().hasReducedPrivileges is true and
/// UserConfig::showPrivilegeNotice is true. The user can suppress future notices
/// via the "Don't show again" checkbox.
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

    void onAttach() override;
    void onDetach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

    [[nodiscard]] static auto instance() -> ElevationNoticeLayer*;
    // Set singleton without taking ownership (use when layer is in layer stack)
    static void setInstance(ElevationNoticeLayer& layer);
    void requestOpen();

  private:
    void renderDialog();

    bool m_OpenRequested = false;
    bool m_DontShowAgain = false;

    // Non-owning singleton pointer; points to a layer owned by the application's layer stack
    static ElevationNoticeLayer* s_Instance;
};

} // namespace App
