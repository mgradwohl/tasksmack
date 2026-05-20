#pragma once

#include "Core/Layer.h"

#include <cstdint>

struct SDL_Cursor;

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
    void onPostRender() override;
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
    enum class ResizeEdge : std::uint8_t
    {
        None,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    void beginWindowInteraction(const SDL_Event& event);
    void handleTitleBarDoubleClick(const SDL_Event& event);
    void updateWindowInteraction();
    void endWindowInteraction();

    void createSystemCursors();
    void destroySystemCursors();
    void updateResizeCursor();
    void applyCursorForEdge(ResizeEdge edge);

    [[nodiscard]] static auto detectResizeEdge(float x, float y, int windowWidth, int windowHeight, bool isMaximized) -> ResizeEdge;
    [[nodiscard]] auto isPointInControlArea(float x, float y) const -> bool;

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

    bool m_CustomDragActive = false;
    bool m_CustomResizeActive = false;
    ResizeEdge m_ActiveResizeEdge = ResizeEdge::None;

    int m_DragStartMouseGlobalX = 0;
    int m_DragStartMouseGlobalY = 0;
    int m_DragStartWindowX = 0;
    int m_DragStartWindowY = 0;

    int m_ResizeStartMouseGlobalX = 0;
    int m_ResizeStartMouseGlobalY = 0;
    int m_ResizeStartWindowX = 0;
    int m_ResizeStartWindowY = 0;
    int m_ResizeStartWindowWidth = 0;
    int m_ResizeStartWindowHeight = 0;

    int m_LastAppliedWindowX = 0;
    int m_LastAppliedWindowY = 0;
    int m_LastAppliedWindowWidth = 0;
    int m_LastAppliedWindowHeight = 0;
    bool m_HasPendingResizeCommit = false;
    int m_PendingResizeWidth = 0;
    int m_PendingResizeHeight = 0;
    float m_LastResizeSizeCommitTime = 0.0F;
    float m_LastResizeDesiredChangeTime = 0.0F;

    bool m_PendingDragRestore = false;
    int m_MaximizedWindowX = 0;
    int m_MaximizedWindowWidth = 0;

    ResizeEdge m_CachedHoverEdge = ResizeEdge::None;
    int m_LastCursorMouseGlobalX = 0;
    int m_LastCursorMouseGlobalY = 0;
    int m_LastCursorWindowX = 0;
    int m_LastCursorWindowY = 0;
    int m_LastCursorWindowWidth = 0;
    int m_LastCursorWindowHeight = 0;
    bool m_LastCursorWindowMaximized = false;
    bool m_HasCursorSample = false;
    int m_LastImmediateResizePixelW = 0;
    int m_LastImmediateResizePixelH = 0;
    float m_LastImmediateResizeEventTime = 0.0F;

    SDL_Cursor* m_DefaultCursor = nullptr;
    SDL_Cursor* m_NsResizeCursor = nullptr;
    SDL_Cursor* m_EwResizeCursor = nullptr;
    SDL_Cursor* m_NeswResizeCursor = nullptr;
    SDL_Cursor* m_NwseResizeCursor = nullptr;
};

} // namespace App
