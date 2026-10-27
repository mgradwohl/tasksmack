#pragma once

#include "App/TitleBarGeometry.h"
#include "App/TitleBarView.h"
#include "App/WindowInteraction.h"
#include "Core/Layer.h"
#include "UI/IconLoader.h"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>

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
/// - SDL hit test registration: the OS or compositor runs the title-bar drag and the border resize
///   where it can (hitTest()), and the layer runs them itself elsewhere
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

    /// True if (x, y) is a title-bar control's -- any button's or the icon's -- so neither a drag nor a
    /// resize starts there: computeIsPointInControlArea() over hitTestInput(), which leaves the
    /// top-right resize corner to the border (#2882).
    [[nodiscard]] auto isPointInControlArea(float x, float y) const -> bool;

    /// Everything SDL's hit test is decided from, as the bar has it now (WindowHitTestInput): the
    /// window's size and maximized state, the bar's height and resize border at the display's scale,
    /// where the buttons and the icon were last drawn, and the backend's policy
    /// (Core::VideoBackend::usesSystemDrag()/usesSystemResize(), a client-side maximized window
    /// excepted: computeSystemDragApplies()). Valid outside an ImGui frame.
    [[nodiscard]] auto hitTestInput() const -> WindowHitTestInput;

    /// What the SDL hit-test callback installed on the window answers for window-local (@p x, @p y):
    /// computeWindowHitTestAt() over hitTestInput(). Public so a test can ask the real window's
    /// callback the same question SDL does (#2716).
    [[nodiscard]] auto hitTest(float x, float y) const -> SDL_HitTestResult;

    /// Mouse state a drag or resize in progress reads instead of SDL's, as {x, y, buttonMask} in
    /// global coordinates: tests hold the left button down and move the pointer, which a pushed
    /// event can't do (SDL's own mouse state doesn't follow pushed events). An empty function
    /// restores SDL's.
    void setMouseStateQueryForTesting(std::function<std::tuple<int, int, SDL_MouseButtonFlags>()> query)
    {
        m_MouseStateQueryForTesting = std::move(query);
    }

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
    /// Moves a window dragged off a maximize back under the pointer if the window manager's late
    /// restore moved it away (RestoreSettle, #2761). @p setPositionMs accumulates the move's time.
    void settleRestore(Core::Window& window, double& setPositionMs);
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
    /// The native Windows system menu at the cursor (the icon's click), its items enabled for a
    /// window that is @p maximized or not (TitleBarView::systemMenuItems()). Returns the window
    /// command chosen, for renderTitleBar() to carry out as the title-bar buttons do, client-side
    /// maximize included (#3161); None when the menu was dismissed or the choice was posted to the OS
    /// as WM_SYSCOMMAND (Move, Size).
    [[nodiscard]] static TitleBarView::Action showNativeSystemMenu(bool maximized);
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
    // Keeps a window dragged off a maximize under the pointer for a moment; outlives the drag (#2761).
    RestoreSettle m_RestoreSettle;

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
    std::function<std::tuple<int, int, SDL_MouseButtonFlags>()> m_MouseStateQueryForTesting;

    SDL_Cursor* m_DefaultCursor = nullptr;
    SDL_Cursor* m_NsResizeCursor = nullptr;
    SDL_Cursor* m_EwResizeCursor = nullptr;
    SDL_Cursor* m_NeswResizeCursor = nullptr;
    SDL_Cursor* m_NwseResizeCursor = nullptr;
};

} // namespace App
