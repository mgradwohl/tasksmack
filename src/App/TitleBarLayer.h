#pragma once

#include "Core/Layer.h"

namespace App
{

/// Custom title bar layer - renders window chrome with icon, title, and controls
/// This layer handles:
/// - App icon display
/// - "TaskSmack" title in Sixtyfour pixel font
/// - Help and Settings buttons (emit events)
/// - Window controls: minimize, maximize/restore, close
/// - SDL hit test registration for window dragging
class TitleBarLayer : public Core::Layer
{
  public:
    TitleBarLayer();
    ~TitleBarLayer() override;

    TitleBarLayer(const TitleBarLayer&) = delete;
    auto operator=(const TitleBarLayer&) -> TitleBarLayer& = delete;
    TitleBarLayer(TitleBarLayer&&) = delete;
    auto operator=(TitleBarLayer&&) -> TitleBarLayer& = delete;

    void onAttach() override;
    void onDetach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onSDLEvent(SDL_Event* event) override;

    /// Get the title bar height (for content offset) - matches ImGui tab bar height
    [[nodiscard]] static auto height() -> float;

    // Cached button bounds for hit testing (public for hit test callback)
    struct ButtonBounds
    {
        float minX = 0.0F;
        float maxX = 0.0F;
        float minY = 0.0F;
        float maxY = 0.0F;
    };

    [[nodiscard]] auto getHelpBounds() const -> const ButtonBounds&
    {
        return m_HelpBounds;
    }
    [[nodiscard]] auto getSettingsBounds() const -> const ButtonBounds&
    {
        return m_SettingsBounds;
    }
    [[nodiscard]] auto getMinimizeBounds() const -> const ButtonBounds&
    {
        return m_MinimizeBounds;
    }
    [[nodiscard]] auto getMaximizeBounds() const -> const ButtonBounds&
    {
        return m_MaximizeBounds;
    }
    [[nodiscard]] auto getCloseBounds() const -> const ButtonBounds&
    {
        return m_CloseBounds;
    }
    [[nodiscard]] auto getIconBounds() const -> const ButtonBounds&
    {
        return m_IconBounds;
    }

  private:
    void renderTitleBar();
    void renderSystemMenu();
    void setupHitTest();

    // Icon texture
    unsigned int m_IconTexture = 0;
    int m_IconWidth = 0;
    int m_IconHeight = 0;

    // System menu state
    bool m_ShowSystemMenu = false;

    // Cached button bounds for hit testing
    ButtonBounds m_HelpBounds{};
    ButtonBounds m_SettingsBounds{};
    ButtonBounds m_MinimizeBounds{};
    ButtonBounds m_MaximizeBounds{};
    ButtonBounds m_CloseBounds{};
    ButtonBounds m_IconBounds{};
};

} // namespace App
