#pragma once

#include "Core/GraphicsHostInfo.h"
#include "Core/WindowGeometry.h"
#include "Core/WindowVisibility.h"

#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_video.h>

#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <chrono> // m_LastCloakCheck
#endif

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
    /// The OS window title (taskbar, window switcher), which the custom title bar draws too (#2728).
    void setTitle(const std::string& title);
    [[nodiscard]] const std::string& title() const noexcept
    {
        return m_Spec.Title;
    }

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
    /// Set the smallest size the user can resize the window to, in window coordinates. With
    /// MinimumSizeChange::AllowGrow a smaller window grows to it, as with SDL_SetWindowMinimumSize();
    /// with KeepSize a side the window is smaller than keeps its size until the user makes it at least
    /// that big (WindowGeometry::minimumKeepingSize(), #1945). Either way a tiled native Wayland window
    /// keeps the size the compositor gave it rather than SDL's stale floating size (#1777). False,
    /// with the SDL error logged, when SDL rejects the minimum. Cheap: no SDL_SyncWindow().
    bool setMinimumSize(int width, int height, WindowGeometry::MinimumSizeChange change);
    /// The minimum size handed to SDL by setMinimumSize() (and, after a KeepSize change, raised by
    /// handleGeometryChanged() as the window grows to the wanted one), in window coordinates; {0, 0} before any. A client-side
    /// resize holds the window to it, as the OS does for a native one.
    [[nodiscard]] auto getMinimumSize() const noexcept -> std::pair<int, int>
    {
        return m_AppliedMinimum;
    }
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
    /// Whether the window is at its normal geometry right now: not maximized (on any route, including
    /// a window-manager maximize the tracker hasn't heard of yet), minimized or fullscreen.
    [[nodiscard]] bool isNormalNow() const;

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
    /// pixel density) on the display it is on now; 1.0 at 100 % on Windows. 0 when unknown, and while
    /// the window is minimized, whose pixel density measures no real size (#3150).
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
    /// Windows doesn't report it, so there it adds nothing beyond isMinimized(); see isHidden().
    [[nodiscard]] bool isOccluded() const noexcept;
    /// Whether nothing of the window can be seen, so the frame loop draws no frames (#1125, #3156):
    /// minimized, occluded, or, on Windows, cloaked (another virtual desktop), behind a locked or
    /// disconnected session, or on a display that is off (#3152). WindowVisibility::isHidden().
    [[nodiscard]] bool isHidden() const noexcept;
    /// How the hidden window will be shown again, for how the frame loop waits while it draws
    /// nothing (#3156): WindowVisibility::classifyHidden(). Shown exactly when isHidden() is false.
    [[nodiscard]] WindowVisibility::HiddenState hiddenState() const noexcept;
    /// Report the window hidden (HiddenState::Hidden) whatever its real state, until called with
    /// false. A test seam (#3156): Xvfb has no window manager to minimize the window, so on Linux
    /// CI the frame loop's hidden path is reached only this way.
    void setHiddenForTesting(bool hidden) noexcept
    {
        m_HiddenForTesting = hidden;
    }
    /// Re-check what no window message reports, once per frame-loop iteration: on Windows, whether
    /// DWM has cloaked the window, when WindowVisibility::cloakRecheckDue() says so. Does nothing
    /// elsewhere. Cheap: at most one DwmGetWindowAttribute() call.
    void pollVisibility();
    /// The refresh rate of the display the window is on, in Hz, or 0 when SDL does not know it.
    /// Queries SDL; Application caches it and re-reads it when the window changes display (#1126).
    [[nodiscard]] double getDisplayRefreshRate() const noexcept;
    /// TaskSmack's OpenGL context (captured once, when it was created) and every display's name, bounds,
    /// mode, scale, HDR state and whether it is primary, as plain data for the System Information page
    /// (#1519). Queries SDL: call it on the main thread, once per System Information read, not per frame.
    [[nodiscard]] GraphicsHostInfo queryGraphicsHostInfo() const;
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
    /// a maximize from outside the app (#1250), and raise the minimum size toward the wanted one on a
    /// side the window has grown to it (#1945). A few cached SDL queries, cheap enough per event.
    void handleGeometryChanged();
    /// A minimize from the shell reached the window (#1279): Win32 WM_SYSCOMMAND SC_MINIMIZE, before
    /// it is carried out, or SDL_EVENT_WINDOW_MINIMIZED, for a minimize that did not come that way.
    /// When WindowGeometry::shellMinimizeRestores() says it is Win+Down on a client-side maximized
    /// window, restores the window to its normal rectangle -- un-minimizing it first if the minimize
    /// already happened -- as the first Win+Down does for a natively maximized window. Reads the
    /// keyboard state on Windows; does nothing elsewhere.
    /// @return Whether the window was restored, so a minimize not yet carried out should be dropped.
    bool restoreForShellMinimize();
#ifdef _WIN32
    /// A window command from the shell reached the window (Win32 WM_SYSCOMMAND), before it is carried
    /// out. Minimize is restoreForShellMinimize()'s (#1279). Restore of a client-side maximized window
    /// and Maximize of one the OS hasn't maximized are carried out here, as restore() and maximize(),
    /// since the OS doesn't know about the client-side maximize (#3161); see
    /// WindowGeometry::shellCommandCarriedOutClientSide(). Everything else is left to the OS.
    /// @return Whether the command was carried out here, so the OS must not carry it out too.
    /// Windows only: only the borderless window's WM_SYSCOMMAND subclass calls it.
    bool handleShellCommand(WindowGeometry::SystemCommand command);
    /// The window's visibility changed (#3152), as the subclass procedure decoded it from
    /// WM_WTSSESSION_CHANGE and WM_POWERBROADCAST: the session was locked, unlocked, disconnected or
    /// connected, or its display turned off or on. Windows only, like the subclass that calls them.
    void handleSessionChange(WindowVisibility::SessionChange change);
    void handleDisplayStateChange(WindowVisibility::DisplayState state);
    /// Ask DWM whether the window is cloaked (DWMWA_CLOAKED), as on activation and focus changes and
    /// from pollVisibility() (#3152). Windows only.
    void refreshCloaked();
#endif
    /// Reads whether {a Windows key, the Down arrow} are held, for restoreForShellMinimize().
    using ShellRestoreKeysReader = auto (*)() noexcept -> std::pair<bool, bool>;
    /// Replace how this window reads the Win+Down key state; nullptr restores the real keyboard
    /// state. A test seam: synthetic input cannot hold keys on a locked or headless desktop (#1279).
    void setShellRestoreKeysReader(ShellRestoreKeysReader reader) noexcept
    {
        m_ShellRestoreKeysReader = reader;
    }
    /// Reads a display's usable bounds, for a client-side maximize: SDL_GetDisplayUsableBounds()'s shape.
    using UsableBoundsReader = auto (*)(SDL_DisplayID display, SDL_Rect* bounds) noexcept -> bool;
    /// Replace how this window reads the display's usable bounds; nullptr restores SDL's. A test seam:
    /// no real display fails that read on demand (#2722).
    void setUsableBoundsReader(UsableBoundsReader reader) noexcept
    {
        m_UsableBoundsReader = reader;
    }
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
    // Hand @p minimum to SDL as the window's minimum size, putting back the size SDL would replace
    // with its stale floating size on native Wayland (#1777). False when SDL rejects it.
    bool applyMinimumSize(std::pair<int, int> minimum);
#ifdef _WIN32
    // Take @p signals as the window's visibility signals. When that shows a hidden window again, push
    // an SDL_EVENT_WINDOW_EXPOSED, so a frame loop waiting at the hidden rate wakes and draws at once.
    void setVisibilitySignals(WindowVisibility::Signals signals);
#endif

    WindowSpecification m_Spec;
    SDL_Window* m_Handle = nullptr;
    SDL_GLContext m_GLContext = nullptr;
    GraphicsHostInfo m_GLInfo; // the OpenGL strings, captured once the context exists
    bool m_ShouldClose = false;
    bool m_HiddenForTesting = false; // see setHiddenForTesting()
    // The Windows signals isHidden() adds to minimized and occluded (#3152); all false elsewhere.
    WindowVisibility::Signals m_VisibilitySignals;

    // How the window is maximized and the rectangle it restores to, through every maximize and
    // restore whoever starts it (#1250). Its state is the only maximized signal for a borderless
    // window on a client-side-maximize backend (see isMaximized()).
    WindowGeometry::NormalGeometryTracker m_Geometry;
    // The minimum size the app wants (setMinimumSize()) and the one handed to SDL, which after a
    // KeepSize change is smaller on a side the window has not yet been made that big (#1945).
    // {0, 0} before any.
    std::pair<int, int> m_WantedMinimum{0, 0};
    std::pair<int, int> m_AppliedMinimum{0, 0};
    // See setShellRestoreKeysReader(); nullptr reads the keyboard.
    ShellRestoreKeysReader m_ShellRestoreKeysReader = nullptr;
    // See setUsableBoundsReader(); nullptr asks SDL.
    UsableBoundsReader m_UsableBoundsReader = nullptr;

#ifdef _WIN32
    // Owned title-bar/taskbar icon handles (opaque void* here so <windows.h> stays out of
    // this cross-platform header; the .cpp casts to HICON to destroy them). Set from the
    // embedded resource in the constructor, released in ~Window() via DestroyIcon() since
    // LoadImage(..., IMAGE_ICON, ...) without LR_SHARED returns handles the caller owns.
    void* m_IconSmall = nullptr;
    void* m_IconBig = nullptr;
    // When refreshCloaked() last asked DWM, for pollVisibility()'s WINDOW_CLOAK_RECHECK_INTERVAL.
    std::chrono::steady_clock::time_point m_LastCloakCheck;
#endif
};

} // namespace Core
