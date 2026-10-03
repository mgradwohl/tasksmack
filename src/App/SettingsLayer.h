#pragma once

#include "App/SettingsLayerDetail.h"
#include "Core/Layer.h"
#include "UI/Theme.h"

#include <string>
#include <vector>

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
    void renderSettingsDialog();
    void loadCurrentSettings();
    void applySettings();

    bool m_OpenRequested = false;

    // The combos' state while the dialog is open. Apply writes only the ones the user picked (#1120).
    Detail::ComboState m_ThemeChoice;
    Detail::ComboState m_FontSizeChoice;
    Detail::ComboState m_RefreshRateChoice;
    Detail::ComboState m_HistoryChoice;
    bool m_ForceNativeDecorationsOnWayland = false;

    // Previews for stored values that aren't among the options ("Custom (750 ms)"), built on open.
    std::string m_CustomThemePreview;
    std::string m_CustomRefreshPreview;
    std::string m_CustomHistoryPreview;

    // Available options
    std::vector<UI::DiscoveredTheme> m_Themes;
};

} // namespace App
