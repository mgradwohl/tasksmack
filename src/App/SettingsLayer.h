#pragma once

#include "Core/Layer.h"
#include "UI/Theme.h"

#include <vector>

namespace App
{

/// Settings dialog layer (singleton).
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

    void onAttach() override;
    void onDetach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

    static auto instance() -> SettingsLayer*;
    // Set singleton without taking ownership (use when layer is in layer stack)
    static void setInstance(SettingsLayer& layer);
    void requestOpen();

  private:
    void renderSettingsDialog();
    void loadCurrentSettings();
    void applySettings();

    bool m_OpenRequested = false;

    // Cached settings for editing (applied on "Apply")
    // Using size_t to match vector indices, avoiding signed/unsigned comparisons
    std::size_t m_SelectedThemeIndex = 0;
    std::size_t m_SelectedFontSizeIndex = 0;
    std::size_t m_SelectedRefreshRateIndex = 0;
    std::size_t m_SelectedHistoryIndex = 0;

    // Available options
    std::vector<UI::DiscoveredTheme> m_Themes;

    // Non-owning singleton pointer; points to a layer owned by the application's layer stack
    static SettingsLayer* s_Instance;
};

} // namespace App
