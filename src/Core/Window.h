#pragma once

#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"

#include <SDL3/SDL_video.h>

#include <optional>
#include <string>
#include <utility>

namespace Core
{

struct WindowSpecification
{
    std::string Title = "Window";
    int Width = 1280;
    int Height = 720;
    bool VSync = true;
    bool Borderless = true; // Enable custom title bar by default
};

class Window
{
  public:
    explicit Window(WindowSpecification spec = WindowSpecification());
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    void swapBuffers() const;

    /// Enable or disable vertical sync at runtime.
    /// Pass true to restore adaptive vsync (SDL_GL_SetSwapInterval(-1) with
    /// fallback to 1). Pass false to disable vsync (interval 0), which reduces
    /// compositor-stall coupling on Wayland during interactive resize.
    static void setVSync(bool enabled);

    [[nodiscard]] bool shouldClose() const noexcept;
    /// Ask to close the window. Application::run() picks this up after the event drain and raises
    /// WindowCloseEvent, so a layer can veto it like an OS close request (#1077).
    void requestClose() noexcept;
    void clearCloseRequest() noexcept; // Reset the close flag once run() has raised WindowCloseEvent

    /// Return the current logical width in screen coordinates.
    /// Queries SDL directly so the value stays accurate after user-initiated
    /// drag-resizes.
    [[nodiscard]] int getWidth() const noexcept;

    /// Return the current logical height in screen coordinates.
    /// Queries SDL directly so the value stays accurate after user-initiated
    /// drag-resizes.
    [[nodiscard]] int getHeight() const noexcept;

    /// Return the current framebuffer size in physical pixels.
    /// Use this for OpenGL calls (e.g. glViewport) on HiDPI displays.
    [[nodiscard]] auto getSizeInPixels() const noexcept -> std::pair<int, int>;

    [[nodiscard]] SDL_Window* getHandle() const noexcept
    {
        return m_Handle;
    }

    [[nodiscard]] SDL_GLContext getGLContext() const noexcept
    {
        return m_GLContext;
    }

    /// Move the window. Asynchronous (no SDL_SyncWindow): the custom title bar calls it on every
    /// drag and left/top-resize step. On X11 getPosition() may report the old position until the
    /// window manager applies the move.
    void setPosition(int x, int y) const;
    [[nodiscard]] auto getPosition() const -> std::pair<int, int>;
    [[nodiscard]] static bool supportsPositioning() noexcept;

    void setSize(int width, int height);
    /// Return the current logical size as {width, height} in screen coordinates.
    /// Returns {m_Spec.Width, m_Spec.Height} when the window handle has not yet
    /// been created. Callers that need both dimensions should prefer this over
    /// separate getWidth()/getHeight() calls to avoid issuing two SDL queries.
    [[nodiscard]] auto getSize() const noexcept -> std::pair<int, int>;

    /// The window's normal (restored) rectangle: its live geometry when not maximized, or the
    /// rectangle it will restore to when maximized (#1121), whoever maximized it (#1250). std::nullopt
    /// when it is maximized and that rectangle is unknown -- maximized from outside the app before it
    /// was ever seen at a normal size -- in which case the caller should keep whatever normal geometry
    /// it saved before.
    [[nodiscard]] auto getNormalGeometry() const -> std::optional<WindowGeometry::Rect>;
    /// The window scale getNormalGeometry()'s rectangle was measured at (#1168): the live scale when
    /// not maximized, the scale captured with the restore rectangle when maximized; 0 if unknown.
    [[nodiscard]] auto getNormalGeometryScale() const -> float;

    /// Apply saved geometry at startup: clamp the size to the display, move the window to
    /// @p position when positioning is supported and the position is reachable on a connected
    /// display (otherwise centre it on the primary display, #1128), then maximize if @p maximized.
    /// The normal rectangle is applied before maximizing so it becomes the restore target (#1121).
    /// Startup-only: before maximizing it waits for the window manager (SDL_SyncWindow), like
    /// setSize(), so never call it from the render loop.
    /// @p savedScale is the window scale the size was saved at (getUnitScale()), if known: the size
    /// is converted to the scale of the display the window opens on, so it keeps its apparent size
    /// across displays and scale changes (#1168). Without it the size is applied as saved.
    void applySavedGeometry(std::optional<std::pair<int, int>> position, bool maximized, std::optional<float> savedScale);

    /// The window's UI scale in window units (WindowGeometry::windowUnitScale(): display scale over
    /// pixel density) on the display it is on now; 1.0 at 100 % on Windows. 0 when unknown.
    [[nodiscard]] auto getUnitScale() const noexcept -> float;

    /// The display the window is on, or 0 when unknown. Cheap enough to poll every frame, so a
    /// caller can tell when to re-read getUsableDisplaySize().
    [[nodiscard]] auto getDisplayId() const noexcept -> SDL_DisplayID;

    /// Size of the usable bounds (work area) of the display the window is on, in window
    /// coordinates like getSize(), or std::nullopt when unknown. Asks the windowing system (a
    /// server round trip on X11), so read it when the display changes, not every frame (#1207).
    [[nodiscard]] auto getUsableDisplaySize() const -> std::optional<std::pair<int, int>>;

    [[nodiscard]] bool isMaximized() const;
    [[nodiscard]] bool isMinimized() const noexcept;
    /// Whether the window is fully covered or otherwise not visible (SDL_WINDOW_OCCLUDED): set when
    /// SDL reports SDL_EVENT_WINDOW_OCCLUDED, cleared on the next SDL_EVENT_WINDOW_EXPOSED. The frame
    /// loop treats it like minimized (#1125). Only Wayland reports an ordinarily covered window: the
    /// pinned SDL's X11 backend sets it only together with minimized (_NET_WM_STATE_HIDDEN), and
    /// Windows doesn't report it, so there it adds nothing beyond isMinimized().
    [[nodiscard]] bool isOccluded() const noexcept;
    /// The refresh rate of the display the window is on, in Hz, or 0 when SDL does not know it.
    /// Queries SDL; Application caches it and re-reads it when the window changes display (#1126).
    [[nodiscard]] double getDisplayRefreshRate() const noexcept;
    void maximize();
    /// Replace an OS-initiated maximize (SDL_EVENT_WINDOW_MAXIMIZED: Win+Up, snap to the top edge,
    /// ShowWindow(SW_MAXIMIZE)) with maximize()'s client-side one, which fills the current display's
    /// usable bounds and records the real normal rectangle as the restore target (#1208). Does
    /// nothing where WindowGeometry::shouldAdoptSystemMaximize() says the OS maximize is right.
    /// @return Whether the OS maximize was adopted.
    bool adoptSystemMaximize();
    /// Handle SDL_EVENT_WINDOW_MAXIMIZED, whoever maximized the window (#1250): when @p adopt, first
    /// try adoptSystemMaximize(); otherwise, or when it is not adopted, record that the window is
    /// maximized, with the last geometry it had while normal as the restore target, so that geometry
    /// -- not the maximized one -- is saved as the normal size on exit. Reads the live flags, so a
    /// queued event the window has since moved on from changes nothing.
    void handleSystemMaximized(bool adopt);
    /// Handle SDL_EVENT_WINDOW_RESTORED: ends a maximize the OS, window manager or compositor can end
    /// (WindowGeometry::MaximizeState::System) when the live flags say the window is no longer
    /// maximized (#1250). Never ends a client-side maximize.
    void handleSystemRestored();
    /// Handle SDL_EVENT_WINDOW_MOVED / _RESIZED: remember the live geometry as the last normal one
    /// while the window is normal (not maximized, minimized or fullscreen), as the restore target for
    /// a maximize from outside the app (#1250). A few cached SDL queries, cheap enough per event.
    void handleGeometryChanged();
    void restore();
    void minimize() const;

    // Custom title bar support
    [[nodiscard]] bool isBorderless() const noexcept
    {
        return m_Spec.Borderless;
    }
    // Set a hit test callback for custom window dragging/resizing
    void setHitTestCallback(SDL_HitTest callback, void* callbackData) const;

  private:
    // The live position and size.
    [[nodiscard]] auto liveRect() const -> WindowGeometry::Rect;
    // Whether the window is at its normal geometry right now: not maximized (on any route),
    // minimized or fullscreen.
    [[nodiscard]] bool isNormalNow() const;

    WindowSpecification m_Spec;
    SDL_Window* m_Handle = nullptr;
    SDL_GLContext m_GLContext = nullptr;
    bool m_ShouldClose = false;

    // How the window is maximized and the rectangle it restores to, through every maximize and
    // restore whoever starts it (#1250). Its state is the only maximized signal for a borderless
    // window on a client-side-maximize backend (see isMaximized()).
    WindowGeometry::NormalGeometryTracker m_Geometry;

#ifdef _WIN32
    // Owned title-bar/taskbar icon handles (opaque void* here so <windows.h> stays out of
    // this cross-platform header; the .cpp casts to HICON to destroy them). Set from the
    // embedded resource in the constructor, released in ~Window() via DestroyIcon() since
    // LoadImage(..., IMAGE_ICON, ...) without LR_SHARED returns handles the caller owns.
    void* m_IconSmall = nullptr;
    void* m_IconBig = nullptr;
#endif
};

} // namespace Core
