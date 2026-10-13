#pragma once

#include "App/TitleBarGeometry.h"
#include "App/WindowInteraction.h"
#include "Core/Layer.h"
#include "UI/IconLoader.h"

#include <SDL3/SDL_mouse.h>

#include <cstdint>
#include <optional>
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
/// - About (i), Help (?) and Settings (gear) buttons (emit events)
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

    /// Size the panels below need, from computeContentMinimumWidth() (#1207) and
    /// computeContentMinimumHeight() (#1278). The window's minimum size covers it from the next frame
    /// the bar is drawn. Set by ShellLayer every frame. @p estimatedHeightPx is the height without
    /// what the tabs measured (the tab-independent part): a change there may grow the window, a
    /// change in the measured part only may not (minimumSizeChangeFor(), #1945).
    void setContentMinimumSize(float widthPx, float heightPx, float estimatedHeightPx) noexcept
    {
        m_ContentMinimumWidthPx = widthPx;
        m_ContentMinimumHeightPx = heightPx;
        m_ContentEstimatedHeightPx = estimatedHeightPx;
    }

    // Cached button bounds for hit testing - see App::ButtonBounds (TitleBarGeometry.h).
    using ButtonBounds = App::ButtonBounds;

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
    /// Widens the window's SDL minimum size to the bar's @p contentWidth and the panels' content.
    void updateMinimumWindowSize(float contentWidth);
#ifdef _WIN32
    /// The native Windows system menu at the cursor (the icon's click).
    static void showNativeSystemMenu();
#endif
    void setupHitTest();

    // Load the bundled application icon closest above @p pixelSize (selectIconPixelSize()).
    void loadIconTexture(int pixelSize);

    // Icon texture, and the bundled size it was loaded from: reloaded when the drawn size calls for
    // another one, after a display-scale change (#1169). 0 before the first load.
    UI::Texture m_IconTexture;
    int m_IconTexturePx = 0;

    // System menu state
    bool m_ShowSystemMenu = false;

    // Cached button bounds for hit testing
    TitleBarButtonLayout m_ButtonLayout{};
    ButtonBounds m_IconBounds{};

    InteractionMode m_InteractionMode = InteractionMode::None;
    // The drag or resize under way (WindowInteraction.h); reset by endWindowInteraction().
    DragTracker m_Drag;
    ResizeTracker m_Resize;

    // Smallest size the window may be resized to, from the title bar's own content and the display
    // scale (see computeMinimumWindowSize()). Recomputed each frame the bar is drawn and handed to
    // Window::setMinimumSize() on a change (#970), growing the window only for a change in its
    // tab-independent part (#1945); custom edge-drags use what that applied (Window::getMinimumSize()).
    WindowMinimumSize m_MinimumSize{};
    // The minimum before capMinimumToUsable(), and the display it was capped for: the display's
    // usable bounds are re-read only when one of them changes (#1207).
    WindowMinimumSize m_DesiredMinimumSize{};
    // The same from the panels' estimated content height alone (setContentMinimumSize()), and the
    // capped one the minimum was last set with: a change in it may grow the window (#1945).
    WindowMinimumSize m_DesiredGrowMinimumSize{};
    std::optional<WindowMinimumSize> m_GrowMinimumSize;
    SDL_DisplayID m_MinimumSizeDisplayId = 0;
    // The panels' share of that minimum (#1207, #1278), from setContentMinimumSize().
    float m_ContentMinimumWidthPx = 0.0F;
    float m_ContentMinimumHeightPx = 0.0F;
    float m_ContentEstimatedHeightPx = 0.0F;

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
