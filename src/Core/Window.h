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
    /// rectangle it will restore to when maximized (#1121). std::nullopt when it is maximized and
    /// that rectangle is unknown -- maximized by the compositor or OS rather than by maximize() --
    /// in which case the caller should keep whatever normal geometry it saved before.
    [[nodiscard]] auto getNormalGeometry() const -> std::optional<WindowGeometry::Rect>;

    /// Apply saved geometry at startup: clamp the size to the display, move the window to
    /// @p position when positioning is supported and the position is reachable on a connected
    /// display (otherwise centre it on the primary display, #1128), then maximize if @p maximized.
    /// The normal rectangle is applied before maximizing so it becomes the restore target (#1121).
    /// Startup-only: before maximizing it waits for the window manager (SDL_SyncWindow), like
    /// setSize(), so never call it from the render loop.
    void applySavedGeometry(std::optional<std::pair<int, int>> position, bool maximized);

    [[nodiscard]] bool isMaximized() const;
    [[nodiscard]] bool isMinimized() const noexcept;
    /// Whether the window is fully covered or otherwise not visible (SDL_WINDOW_OCCLUDED): set when
    /// SDL reports SDL_EVENT_WINDOW_OCCLUDED (X11 and Wayland do; Windows does not), cleared on the
    /// next SDL_EVENT_WINDOW_EXPOSED. The frame loop treats it like minimized (#1125).
    [[nodiscard]] bool isOccluded() const noexcept;
    /// The refresh rate of the display the window is on, in Hz, or 0 when SDL does not know it.
    /// Queries SDL; Application caches it and re-reads it when the window changes display (#1126).
    [[nodiscard]] double getDisplayRefreshRate() const noexcept;
    void maximize();
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
    // Record the current rectangle as the restore target, unless the window is already maximized.
    void rememberRestoreRect();

    WindowSpecification m_Spec;
    SDL_Window* m_Handle = nullptr;
    SDL_GLContext m_GLContext = nullptr;
    bool m_ShouldClose = false;

    // For borderless window maximize/restore tracking
    bool m_IsMaximizedBorderless = false;
    // Whether m_Restore* hold the rectangle the window had when maximize() last maximized it, on
    // any path (client-side, compositor or SDL_MaximizeWindow). Cleared by restore().
    bool m_HasRestoreRect = false;
    int m_RestoreX = 0;
    int m_RestoreY = 0;
    int m_RestoreWidth = 0;
    int m_RestoreHeight = 0;

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
