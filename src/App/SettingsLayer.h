#pragma once

#include "App/SettingsDialog.h"
#include "Core/Layer.h"

namespace App
{

/// Settings dialog layer. Opened by raising Core::OpenSettingsEvent.
/// Thread safety: All layer lifecycle methods (onAttach/onDetach/onUpdate/onRender)
/// are guaranteed to be called from the main thread only, as required by SDL and ImGui.
class SettingsLayer : public Core::Layer
{
  public:
    SettingsLayer();
    ~SettingsLayer() override;

    SettingsLayer(const SettingsLayer&) = delete;
    SettingsLayer& operator=(const SettingsLayer&) = delete;
    SettingsLayer(SettingsLayer&&) = delete;
    SettingsLayer& operator=(SettingsLayer&&) = delete;

    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

  private:
    void requestOpen();
    void applySettings();

    SettingsDialog::State m_State;
};

} // namespace App
