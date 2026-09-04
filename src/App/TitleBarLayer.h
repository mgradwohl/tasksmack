#pragma once

#include "App/TitleBarGeometry.h"
#include "Core/Layer.h"
#include "UI/IconLoader.h"

#include <SDL3/SDL_mouse.h>

#include <cstdint>
#include <tuple>

struct SDL_Cursor;

namespace Core
{
class Window;
} // namespace Core

namespace App
{

/// Custom title bar layer - renders window chrome with icon, title, and controls.
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

    /// Get the title bar height (for content offset) - matches ImGui tab bar
    /// height
    [[nodiscard]] static auto height() -> float;

    // Cached button bounds for hit testing.
    struct ButtonBounds
    {
        float minX = 0.0F;
        float maxX = 0.0F;
        float minY = 0.0F;
        float maxY = 0.0F;
    };

    /// True if (x, y) falls inside any title-bar button's bounds. Public so the
    /// free-function hit-test callback can use it directly.
    [[nodiscard]] auto isPointInControlArea(float x, float y) const -> bool;

  private:
    enum class InteractionMode : std::uint8_t
    {
        None,
        Drag,
        Resize,
    };

    // Per-drag session state — zeroed by endWindowInteraction via m_Drag = {}.
    struct DragState
    {
        int startMouseGlobalX = 0;
        int startMouseGlobalY = 0;
        int startWindowX = 0;
        int startWindowY = 0;
        int lastAppliedX = 0;
        int lastAppliedY = 0;
        bool pendingRestore = false;
        int maximizedWindowX = 0;
        int maximizedWindowWidth = 0;
    };

    // Per-resize session state — zeroed by endWindowInteraction via m_Resize =
    // {}.
    struct ResizeState
    {
        ResizeEdge edge = ResizeEdge::None;
        int startMouseGlobalX = 0;
        int startMouseGlobalY = 0;
        int startWindowX = 0;
        int startWindowY = 0;
        int startWindowWidth = 0;
        int startWindowHeight = 0;
        int lastAppliedX = 0;
        int lastAppliedY = 0;
        int lastAppliedWidth = 0;
        int lastAppliedHeight = 0;
        bool hasPendingCommit = false;
        int pendingWidth = 0;
        int pendingHeight = 0;
        float lastSizeCommitTime = 0.0F;
        int lastImmediatePixelW = 0;
        int lastImmediatePixelH = 0;
        float lastImmediateEventTime = 0.0F;
    };

    void beginWindowInteraction(const SDL_Event& event);
    void handleTitleBarDoubleClick(const SDL_Event& event) const;
    void updateWindowInteraction();
    void updateDrag(int mx, int my, Core::Window& window, double& restoreMs, double& setPositionMs);
    void updateResize(int mx, int my, Core::Window& window, double& setPositionMs, double& setSizeMs, double& raiseResizeEventMs);
    void endWindowInteraction();

    /// Query the current mouse position and button mask together, using whichever SDL query is
    /// reliable for the active backend (see VideoBackend::supportsGlobalMouseState()): the local
    /// query on native Wayland, the global query elsewhere. Returns {x, y, buttonMask}.
    [[nodiscard]] static auto queryMouseState() -> std::tuple<int, int, SDL_MouseButtonFlags>;

    void createSystemCursors();
    void destroySystemCursors();
    void updateResizeCursor();
    void applyCursorForEdge(ResizeEdge edge);

    [[nodiscard]] static auto detectResizeEdge(float x, float y, int windowWidth, int windowHeight, bool isMaximized) -> ResizeEdge;

    void renderTitleBar();
    void renderSystemMenu();
    void setupHitTest();

    // Icon texture
    UI::Texture m_IconTexture;

    // System menu state
    bool m_ShowSystemMenu = false;

    // Cached button bounds for hit testing
    ButtonBounds m_HelpBounds{};
    ButtonBounds m_SettingsBounds{};
    ButtonBounds m_MinimizeBounds{};
    ButtonBounds m_MaximizeBounds{};
    ButtonBounds m_CloseBounds{};
    ButtonBounds m_IconBounds{};

    InteractionMode m_InteractionMode = InteractionMode::None;
    DragState m_Drag{};
    ResizeState m_Resize{};

    ResizeEdge m_CachedHoverEdge = ResizeEdge::None;
    int m_LastCursorMouseLocalX = 0;
    int m_LastCursorMouseLocalY = 0;
    int m_LastCursorWindowWidth = 0;
    int m_LastCursorWindowHeight = 0;
    bool m_LastCursorWindowMaximized = false;
    bool m_HasCursorSample = false;

    // Diagnostic-only: last logged SDL_GetMouseFocus()-mismatch state, so
    // updateResizeCursor() logs a transition instead of spamming every frame. Added during
    // the #749 follow-up investigation into a WSLg no-resize-cursor report -- that specific
    // report turned out to be a WSL session issue, not this code path, but the logging is
    // kept to help diagnose any future genuine Wayland-compositor cursor report.
    bool m_LastLoggedFocusMismatch = false;
    bool m_HasLoggedFocusMismatch = false;

    bool m_TraceEnabled = false;

    SDL_Cursor* m_DefaultCursor = nullptr;
    SDL_Cursor* m_NsResizeCursor = nullptr;
    SDL_Cursor* m_EwResizeCursor = nullptr;
    SDL_Cursor* m_NeswResizeCursor = nullptr;
    SDL_Cursor* m_NwseResizeCursor = nullptr;
};

} // namespace App
