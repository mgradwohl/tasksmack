#include "Window.h"

#include "Core/ResizePerfOperation.h"
#include "Core/VideoBackend.h"
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Core
{

namespace
{
/// How far from 1 a pixel density may be and still mean window units are physical pixels (#1168).
constexpr float PIXEL_DENSITY_EPSILON = 1e-3F;

[[nodiscard]] int clampWindowDimension(const int value) noexcept
{
    return std::clamp(value, WINDOW_MIN_DIMENSION, WINDOW_MAX_DIMENSION);
}

[[nodiscard]] std::string_view glString(GLenum name)
{
    const auto* bytes = glGetString(name);
    if (bytes == nullptr)
    {
        return "<unknown>";
    }

    // OpenGL returns a byte pointer (GLubyte*); treat it as a C-string for logging only.
    const auto* chars = reinterpret_cast<const char*>(bytes); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return {chars};
}
} // namespace

#ifdef _WIN32
namespace
{
// Set window icon from embedded resource on Windows
// This sets both the small icon (title bar, Alt+Tab) and large icon (taskbar)
[[nodiscard]] auto loadIconFromResource(HINSTANCE instance, int width, int height) -> HANDLE
{
    // LoadImage returns HANDLE; with IMAGE_ICON the returned handle is an icon handle.
    return LoadImage(instance, MAKEINTRESOURCE(1), IMAGE_ICON, width, height, LR_DEFAULTCOLOR);
}

void setWindowIcon(HWND hwnd, WPARAM iconType, HANDLE icon)
{
    // Win32 SendMessage takes LPARAM; HICON is pointer-sized and is passed opaquely
    // NOLINT: Required cast for Win32 API - HICON and LPARAM have compatible sizes
    SendMessage(hwnd,
                WM_SETICON,
                iconType,
                reinterpret_cast<LPARAM>(icon)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-ptr-to-int)
}

// Returns the {small, large} icon handles so the caller (Window) can own and eventually
// DestroyIcon() them; LoadImage(..., IMAGE_ICON, ...) without LR_SHARED returns handles
// the caller is responsible for freeing, unlike icons loaded via LoadIcon().
[[nodiscard]] std::pair<HANDLE, HANDLE> setWindowIconFromResource(SDL_Window* window)
{
    // SDL returns HWND as void* per its property API contract; direct cast is required and safe
    HWND hwnd = reinterpret_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                              SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                                              nullptr)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (hwnd == nullptr)
    {
        spdlog::warn("Failed to get Win32 window handle for icon");
        return {nullptr, nullptr};
    }

    // Get the module handle for this executable
    HINSTANCE hInstance = GetModuleHandle(nullptr);

    // Get icon dimensions from system metrics (with validation)
    const int smallIconWidth = GetSystemMetrics(SM_CXSMICON);
    const int smallIconHeight = GetSystemMetrics(SM_CYSMICON);
    const int largeIconWidth = GetSystemMetrics(SM_CXICON);
    const int largeIconHeight = GetSystemMetrics(SM_CYICON);

    // Validate dimensions (GetSystemMetrics can return 0 on failure)
    if (smallIconWidth <= 0 || smallIconHeight <= 0 || largeIconWidth <= 0 || largeIconHeight <= 0)
    {
        spdlog::warn("Invalid icon dimensions from GetSystemMetrics (small={}x{}, large={}x{})",
                     smallIconWidth,
                     smallIconHeight,
                     largeIconWidth,
                     largeIconHeight);
        return {nullptr, nullptr};
    }

    // Load small icon (16x16) for title bar and Alt+Tab
    // MAKEINTRESOURCE(1) refers to IDI_ICON1 (resource ID 1) defined in the .rc file
    HANDLE hIconSmall = loadIconFromResource(hInstance, smallIconWidth, smallIconHeight);

    // Load large icon (32x32 or larger) for taskbar
    HANDLE hIconBig = loadIconFromResource(hInstance, largeIconWidth, largeIconHeight);

    if (hIconSmall != nullptr)
    {
        setWindowIcon(hwnd, ICON_SMALL, hIconSmall);
        spdlog::debug("Set small window icon ({}x{})", smallIconWidth, smallIconHeight);
    }
    else
    {
        spdlog::warn("Failed to load small icon from resource");
    }

    if (hIconBig != nullptr)
    {
        setWindowIcon(hwnd, ICON_BIG, hIconBig);
        spdlog::debug("Set large window icon ({}x{})", largeIconWidth, largeIconHeight);
    }
    else
    {
        spdlog::warn("Failed to load large icon from resource");
    }

    return {hIconSmall, hIconBig};
}
} // namespace
#endif

Window::Window(WindowSpecification spec) : m_Spec(std::move(spec))
{
    spdlog::info("Creating window: {} ({}x{})", m_Spec.Title, m_Spec.Width, m_Spec.Height);

    m_Spec.Width = clampWindowDimension(m_Spec.Width);
    m_Spec.Height = clampWindowDimension(m_Spec.Height);

    // Set OpenGL attributes before creating window
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifndef NDEBUG
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
    // Explicitly request double buffering. Depth and stencil buffers are not used
    // by the 2-D ImGui render path; requesting 0 bits reduces framebuffer memory
    // and eliminates any driver-side depth/stencil pipeline overhead.
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    // Create window flags
    SDL_WindowFlags windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (m_Spec.Borderless)
    {
        windowFlags |= SDL_WINDOW_BORDERLESS;
    }

    m_Handle = SDL_CreateWindow(m_Spec.Title.c_str(), m_Spec.Width, m_Spec.Height, windowFlags);

    if (m_Handle == nullptr)
    {
        spdlog::critical("Failed to create SDL window: {}", SDL_GetError());
        throw std::runtime_error("Failed to create SDL window");
    }

    m_GLContext = SDL_GL_CreateContext(m_Handle);
    if (m_GLContext == nullptr)
    {
        spdlog::critical("Failed to create OpenGL context: {}", SDL_GetError());
        SDL_DestroyWindow(m_Handle);
        throw std::runtime_error("Failed to create OpenGL context");
    }

    if (!SDL_GL_MakeCurrent(m_Handle, m_GLContext))
    {
        spdlog::critical("Failed to make OpenGL context current: {}", SDL_GetError());
        SDL_GL_DestroyContext(m_GLContext);
        SDL_DestroyWindow(m_Handle);
        throw std::runtime_error("Failed to make OpenGL context current");
    }

    // Load OpenGL functions using GLAD with SDL's GetProcAddress
    // NOLINT justification: SDL_FunctionPointer and GLADloadfunc have compatible signatures;
    // this is the standard pattern for loading OpenGL functions with SDL3
    const int version =
        gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (version == 0)
    {
        spdlog::critical("Failed to initialize GLAD");
        SDL_GL_DestroyContext(m_GLContext);
        SDL_DestroyWindow(m_Handle);
        throw std::runtime_error("Failed to initialize GLAD");
    }

    spdlog::info("OpenGL Info:");
    spdlog::info("  Vendor: {}", glString(GL_VENDOR));
    spdlog::info("  Renderer: {}", glString(GL_RENDERER));
    spdlog::info("  Version: {}", glString(GL_VERSION));

    if (m_Spec.VSync)
    {
        // Prefer adaptive vsync: presents immediately when a frame is late instead of
        // stalling until the next vblank. Falls back to regular vsync if the driver
        // does not support GLX_EXT_swap_control_tear / WGL_EXT_swap_control_tear.
        if (!traceResizePerfSDL(ResizePerfOperation::SwapInterval, -1, 0, [] { return SDL_GL_SetSwapInterval(-1); }))
        {
            traceResizePerfSDL(ResizePerfOperation::SwapInterval, 1, 0, [] { return SDL_GL_SetSwapInterval(1); });
        }
    }
    else
    {
        traceResizePerfSDL(ResizePerfOperation::SwapInterval, 0, 0, [] { return SDL_GL_SetSwapInterval(0); });
    }

#ifdef _WIN32
    // Set window icon from embedded resource (title bar and taskbar). The returned
    // handles are owned by this Window and released in ~Window().
    const auto [iconSmall, iconBig] = setWindowIconFromResource(m_Handle);
    m_IconSmall = iconSmall;
    m_IconBig = iconBig;
#endif
}

Window::~Window()
{
#ifdef _WIN32
    if (m_IconSmall != nullptr)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - HANDLE and HICON are both opaque Win32 handle types
        DestroyIcon(reinterpret_cast<HICON>(m_IconSmall));
        m_IconSmall = nullptr;
    }
    if (m_IconBig != nullptr)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - HANDLE and HICON are both opaque Win32 handle types
        DestroyIcon(reinterpret_cast<HICON>(m_IconBig));
        m_IconBig = nullptr;
    }
#endif

    if (m_GLContext != nullptr)
    {
        SDL_GL_DestroyContext(m_GLContext);
        m_GLContext = nullptr;
    }

    if (m_Handle != nullptr)
    {
        SDL_DestroyWindow(m_Handle);
        m_Handle = nullptr;
    }
}

void Window::swapBuffers() const
{
    SDL_GL_SwapWindow(m_Handle);
}

void Window::setVSync(bool enabled)
{
    if (enabled)
    {
        // Mirror the logic used during initialisation: prefer adaptive vsync
        // (swap-interval -1) and fall back to regular vsync (1) if unsupported.
        if (!traceResizePerfSDL(ResizePerfOperation::SwapInterval, -1, 0, [] { return SDL_GL_SetSwapInterval(-1); }))
        {
            traceResizePerfSDL(ResizePerfOperation::SwapInterval, 1, 0, [] { return SDL_GL_SetSwapInterval(1); });
        }
    }
    else
    {
        traceResizePerfSDL(ResizePerfOperation::SwapInterval, 0, 0, [] { return SDL_GL_SetSwapInterval(0); });
    }
}

bool Window::shouldClose() const noexcept
{
    return m_ShouldClose;
}

void Window::requestClose() noexcept
{
    m_ShouldClose = true;
}

void Window::clearCloseRequest() noexcept
{
    m_ShouldClose = false;
}

void Window::setPosition(int x, int y) const
{
    if (m_Handle == nullptr)
    {
        return;
    }
    // Asynchronous: the custom title bar calls this on every drag and left/top-resize step, so it
    // must not wait for the window manager. applySavedGeometry() syncs once where it needs to.
    SDL_SetWindowPosition(m_Handle, x, y);
}

auto Window::getPosition() const -> std::pair<int, int>
{
    if (m_Handle == nullptr)
    {
        return {0, 0};
    }

    int x = 0;
    int y = 0;
    SDL_GetWindowPosition(m_Handle, &x, &y);
    return {x, y};
}

bool Window::supportsPositioning() noexcept
{
    // Routed through VideoBackend rather than querying SDL_GetCurrentVideoDriver() directly here,
    // per AGENTS.md's rule that backend decisions go through the cached VideoBackend abstraction.
    // Safe: every caller runs after Application's constructor, which calls VideoBackend::initialize()
    // right after SDL_Init.
    //
    // driverName().empty() is the "SDL_GetCurrentVideoDriver() returned nullptr" case
    // (VideoBackend::detectBackend() maps that to Backend::Unknown, same as any unrecognized
    // driver name) -- explicitly treated as unsupported here, matching the pre-VideoBackend
    // behavior, since applying a persisted window position with no confirmed driver is unsafe.
    return !VideoBackend::driverName().empty() && !VideoBackend::isWayland();
}

void Window::setSize(int width, int height)
{
    if (m_Handle == nullptr)
    {
        return;
    }

    const int clampedWidth = clampWindowDimension(width);
    const int clampedHeight = clampWindowDimension(height);
    traceResizePerfSDL(ResizePerfOperation::WindowSize,
                       clampedWidth,
                       clampedHeight,
                       [&] { return SDL_SetWindowSize(m_Handle, clampedWidth, clampedHeight); });
    // Block until the OS has applied the resize so that subsequent SDL_GetWindowSize
    // calls return the new dimensions immediately. On asynchronous windowing systems
    // (X11, Wayland) this pumps X11 events internally and waits for the ConfigureNotify.
    // NOTE: Do not call setSize() from the render loop or from any hot path — this call
    // can block for the duration of a window-manager animation on async platforms.
    traceResizePerfSDL(ResizePerfOperation::WindowSync, clampedWidth, clampedHeight, [&] { return SDL_SyncWindow(m_Handle); });
    m_Spec.Width = clampedWidth;
    m_Spec.Height = clampedHeight;
}

auto Window::getSize() const noexcept -> std::pair<int, int>
{
    if (m_Handle == nullptr)
    {
        return {m_Spec.Width, m_Spec.Height};
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(m_Handle, &width, &height);
    return {width, height};
}

int Window::getWidth() const noexcept
{
    return getSize().first;
}

int Window::getHeight() const noexcept
{
    return getSize().second;
}

auto Window::getSizeInPixels() const noexcept -> std::pair<int, int>
{
    if (m_Handle == nullptr)
    {
        return {m_Spec.Width, m_Spec.Height};
    }

    int pixelW = 0;
    int pixelH = 0;
    SDL_GetWindowSizeInPixels(m_Handle, &pixelW, &pixelH);
    return {pixelW, pixelH};
}

auto Window::getNormalGeometry() const -> std::optional<WindowGeometry::Rect>
{
    return WindowGeometry::selectNormalGeometry(isMaximized(), liveRect(), m_Geometry.restoreTarget());
}

auto Window::getNormalGeometryScale() const -> float
{
    return WindowGeometry::selectNormalGeometryScale(
        isMaximized(), m_Geometry.restoreTarget().has_value(), m_Geometry.restoreScale(), getUnitScale());
}

auto Window::liveRect() const -> WindowGeometry::Rect
{
    const auto [x, y] = getPosition();
    const auto [width, height] = getSize();
    return WindowGeometry::Rect{.x = x, .y = y, .width = width, .height = height};
}

bool Window::isNormalNow() const
{
    if (m_Handle == nullptr)
    {
        return false;
    }
    // SDL_WINDOW_MAXIMIZED as well as isMaximized(): on a client-side backend isMaximized() reads only
    // the tracker, which has not yet heard of a window-manager maximize while its resize events are
    // being drained.
    constexpr SDL_WindowFlags NOT_NORMAL = SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN;
    return !isMaximized() && (SDL_GetWindowFlags(m_Handle) & NOT_NORMAL) == 0;
}

void Window::handleGeometryChanged()
{
    if (m_Handle == nullptr)
    {
        return;
    }
    m_Geometry.observe(liveRect(), getUnitScale(), isNormalNow());
}

void Window::applySavedGeometry(std::optional<std::pair<int, int>> position, bool maximized, std::optional<float> savedScale)
{
    if (m_Handle == nullptr)
    {
        return;
    }

    // Usable bounds of every connected display, and which of them is primary.
    std::vector<WindowGeometry::Rect> displays;
    std::vector<SDL_DisplayID> displayIdsByIndex;
    std::size_t primaryIndex = 0;
    int displayCount = 0;
    SDL_DisplayID* displayIds = SDL_GetDisplays(&displayCount);
    if (displayIds != nullptr)
    {
        const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
        const std::span<const SDL_DisplayID> ids(displayIds, static_cast<std::size_t>(std::max(displayCount, 0)));
        for (const SDL_DisplayID id : ids)
        {
            SDL_Rect bounds{};
            if (!SDL_GetDisplayUsableBounds(id, &bounds) || bounds.w <= 0 || bounds.h <= 0)
            {
                continue;
            }
            if (id == primary)
            {
                primaryIndex = displays.size();
            }
            displays.push_back(WindowGeometry::Rect{.x = bounds.x, .y = bounds.y, .width = bounds.w, .height = bounds.h});
            displayIdsByIndex.push_back(id);
        }
        SDL_free(displayIds);
    }
    if (displays.empty())
    {
        spdlog::warn("Window::applySavedGeometry: no display bounds available ({}); applying saved geometry unchecked", SDL_GetError());
    }

    const auto [width, height] = getSize();
    const bool canPosition = supportsPositioning() && position.has_value();
    WindowGeometry::Rect saved{.x = 0, .y = 0, .width = width, .height = height};
    if (canPosition)
    {
        saved.x = position->first;
        saved.y = position->second;
    }
    else
    {
        // No position to restore (or none can be applied, as on native Wayland): only the size is
        // checked, against the display the window was created on.
        const auto [currentX, currentY] = getPosition();
        saved.x = currentX;
        saved.y = currentY;
    }

    // Convert the saved size to the scale of the display the window opens on, so it keeps its
    // apparent size when that display (or its scale) differs from the one it was saved on (#1168).
    if (savedScale.has_value())
    {
        float targetScale = getUnitScale();
        // Where window units are physical pixels (Windows, X11: pixel density 1), a display's unit
        // scale is its content scale, which can be read before the window is moved there. Elsewhere
        // window units are logical and the window's own scale is the one to keep.
        const float pixelDensity = SDL_GetWindowPixelDensity(m_Handle);
        const auto target = WindowGeometry::targetDisplayIndex(saved, displays, primaryIndex, WindowGeometry::MIN_VISIBLE_EXTENT);
        if (canPosition && target.has_value() && std::abs(pixelDensity - 1.0F) < PIXEL_DENSITY_EPSILON)
        {
            const float contentScale = SDL_GetDisplayContentScale(displayIdsByIndex[*target]);
            if (WindowGeometry::isUsableWindowScale(contentScale))
            {
                targetScale = contentScale;
            }
        }
        const auto [scaledWidth, scaledHeight] = WindowGeometry::rescaleWindowSize(saved.width, saved.height, savedScale, targetScale);
        if (scaledWidth != saved.width || scaledHeight != saved.height)
        {
            spdlog::info("Window::applySavedGeometry: saved size {}x{} at scale {:.2f} is {}x{} at scale {:.2f}",
                         saved.width,
                         saved.height,
                         *savedScale,
                         scaledWidth,
                         scaledHeight,
                         targetScale);
        }
        saved.width = scaledWidth;
        saved.height = scaledHeight;
    }

    const WindowGeometry::Rect fitted =
        WindowGeometry::fitRectToDisplays(saved, displays, primaryIndex, WindowGeometry::MIN_VISIBLE_EXTENT);
    if (fitted.width != saved.width || fitted.height != saved.height)
    {
        spdlog::info("Window::applySavedGeometry: saved size {}x{} shrunk to {}x{} to fit the display",
                     saved.width,
                     saved.height,
                     fitted.width,
                     fitted.height);
    }
    if (fitted.width != width || fitted.height != height)
    {
        setSize(fitted.width, fitted.height);
    }
    if (canPosition)
    {
        if (fitted.x != saved.x || fitted.y != saved.y)
        {
            spdlog::info("Window::applySavedGeometry: saved position ({}, {}) is not on a connected display; moved to ({}, {})",
                         saved.x,
                         saved.y,
                         fitted.x,
                         fitted.y);
        }
        setPosition(fitted.x, fitted.y);
    }

    // The normal geometry a later maximize from outside the app restores to, until the window is
    // moved or resized (#1250).
    handleGeometryChanged();

    if (maximized)
    {
        // On asynchronous windowing systems (X11) a move or resize is only a request until the
        // window manager answers, and SDL_GetWindowPosition keeps returning the old position until
        // then. maximize() records the current rectangle as the restore target, so wait for the
        // normal rectangle to land first (#1121). Once, at startup -- never on a hot path.
        SDL_SyncWindow(m_Handle);
        maximize();
    }
}

auto Window::getUnitScale() const noexcept -> float
{
    if (m_Handle == nullptr)
    {
        return 0.0F;
    }
    const float scale = WindowGeometry::windowUnitScale(SDL_GetWindowDisplayScale(m_Handle), SDL_GetWindowPixelDensity(m_Handle));
    return WindowGeometry::isUsableWindowScale(scale) ? scale : 0.0F;
}

auto Window::getDisplayId() const noexcept -> SDL_DisplayID
{
    return m_Handle != nullptr ? SDL_GetDisplayForWindow(m_Handle) : 0;
}

auto Window::getUsableDisplaySize() const -> std::optional<std::pair<int, int>>
{
    const SDL_DisplayID displayID = getDisplayId();
    SDL_Rect usableBounds{};
    if (displayID == 0 || !SDL_GetDisplayUsableBounds(displayID, &usableBounds) || usableBounds.w <= 0 || usableBounds.h <= 0)
    {
        return std::nullopt;
    }
    return std::pair{usableBounds.w, usableBounds.h};
}

bool Window::isMaximized() const
{
    if (m_Handle == nullptr)
    {
        return false;
    }

    // For borderless windows, X11/XWayland/Windows fake maximize by resizing to the usable
    // display bounds, so SDL_WINDOW_MAXIMIZED never gets set there and our tracked state is
    // the only source of truth. Native Wayland is different: maximize()/restore() delegate to
    // the compositor via SDL_MaximizeWindow()/SDL_RestoreWindow(), so SDL_WINDOW_MAXIMIZED is a
    // real, live signal there -- querying it instead of the cached bool keeps this correct when
    // the compositor changes maximize state outside the app (tiling shortcut, etc.), which the
    // cached bool alone can't observe.
    if ((SDL_GetWindowFlags(m_Handle) & SDL_WINDOW_BORDERLESS) != 0)
    {
        if (!VideoBackend::supportsClientSideMaximize())
        {
            return (SDL_GetWindowFlags(m_Handle) & SDL_WINDOW_MAXIMIZED) != 0;
        }
        return m_Geometry.isMaximized();
    }

    return (SDL_GetWindowFlags(m_Handle) & SDL_WINDOW_MAXIMIZED) != 0;
}

void Window::maximize()
{
    if (m_Handle == nullptr)
    {
        return;
    }

    // The rectangle being left, taken before anything moves the window. It becomes the restore
    // target only when the window is not already maximized: a second maximize() must not replace the
    // normal rectangle with the maximized one (NormalGeometryTracker::maximizing()).
    const WindowGeometry::Rect live = liveRect();
    const float liveScale = getUnitScale();
    const bool normalNow = !isMaximized();

    // For borderless windows, use backend-gated behavior.
    // On native Wayland, prefer compositor maximize (avoids client-side geometry issues).
    // On X11, XWayland, and Windows, use manual client-side positioning for compatibility.
    if ((SDL_GetWindowFlags(m_Handle) & SDL_WINDOW_BORDERLESS) != 0)
    {
        if (!VideoBackend::supportsClientSideMaximize())
        {
            // Native Wayland: use compositor-managed maximize via SDL_MaximizeWindow
            // This avoids the unreliability of client-side usable-bounds queries on Wayland.
            spdlog::debug("Window::maximize: Native Wayland detected; using compositor-managed maximize");
            SDL_MaximizeWindow(m_Handle);
            m_Geometry.maximizing(live, liveScale, normalNow, WindowGeometry::MaximizeState::System);
            return;
        }

        // X11, XWayland, Windows: use client-side maximize with manual positioning

        const SDL_DisplayID displayID = SDL_GetDisplayForWindow(m_Handle);
        if (displayID != 0)
        {
            SDL_Rect usableBounds{};
            if (SDL_GetDisplayUsableBounds(displayID, &usableBounds))
            {
                // Position window at the usable area origin
                SDL_SetWindowPosition(m_Handle, usableBounds.x, usableBounds.y);
                // Size window to fill the usable area
                SDL_SetWindowSize(m_Handle, usableBounds.w, usableBounds.h);
                m_Geometry.maximizing(live, liveScale, normalNow, WindowGeometry::MaximizeState::ClientSide);
                return;
            }
            // If SDL_GetDisplayUsableBounds fails, fall through to SDL_MaximizeWindow
            spdlog::warn("Failed to get display usable bounds for borderless maximize");
        }
    }

    // Fall back to SDL's built-in maximize for non-borderless windows or on error. Tracked as a
    // System maximize: for a borderless window on a client-side-maximize backend that got here (the
    // display bounds were unavailable) isMaximized() reads only the tracker, which must therefore
    // know, or getNormalGeometry() would save this maximized rectangle as the normal one (#1121).
    if (SDL_MaximizeWindow(m_Handle))
    {
        m_Geometry.maximizing(live, liveScale, normalNow, WindowGeometry::MaximizeState::System);
    }
}

bool Window::adoptSystemMaximize()
{
    if (m_Handle == nullptr)
    {
        return false;
    }

    // Read the live flags, not the event: a queued MAXIMIZED can be handled after a later OS restore
    // or minimize, which adopting it would undo (#1208).
    const SDL_WindowFlags flags = SDL_GetWindowFlags(m_Handle);
    const bool borderless = (flags & SDL_WINDOW_BORDERLESS) != 0;
    const bool stillMaximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    const bool minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
    const bool clientSideBackend = VideoBackend::supportsClientSideMaximize();
    SDL_Rect usableBounds{};
    const SDL_DisplayID displayID = SDL_GetDisplayForWindow(m_Handle);
    const bool usableBoundsKnown = displayID != 0 && SDL_GetDisplayUsableBounds(displayID, &usableBounds);
    if (!WindowGeometry::shouldAdoptSystemMaximize(borderless, clientSideBackend, usableBoundsKnown, stillMaximized, minimized))
    {
        return false;
    }

    // Undo the OS maximize first, so the window is back at its normal rectangle: that is what
    // maximize() records as the restore target (when not already maximized client-side), and it
    // clears the OS's maximized state so a later restore() or SDL_SetWindowSize() is not fighting
    // it. Waiting for the restore keeps the recorded rectangle the real one on asynchronous
    // windowing systems. Once per OS maximize, never per frame.
    spdlog::debug("Window::adoptSystemMaximize: replacing an OS maximize with the client-side one (#1208)");
    SDL_RestoreWindow(m_Handle);
    SDL_SyncWindow(m_Handle);
    maximize();
    return true;
}

void Window::handleSystemMaximized(bool adopt)
{
    if (m_Handle == nullptr || (adopt && adoptSystemMaximize()))
    {
        return;
    }
    // Not replaced by a client-side maximize (Linux, where the window manager or compositor sizes the
    // window itself; a framed window; or adoption not possible): keep the OS maximize as it is, but
    // know about it, so isMaximized() reports it and getNormalGeometry() returns the last normal
    // geometry rather than the maximized one (#1250). The live flags, not the event, decide.
    const SDL_WindowFlags flags = SDL_GetWindowFlags(m_Handle);
    m_Geometry.systemMaximized((flags & SDL_WINDOW_MAXIMIZED) != 0, (flags & SDL_WINDOW_MINIMIZED) != 0);
}

void Window::handleSystemRestored()
{
    if (m_Handle == nullptr)
    {
        return;
    }
    const SDL_WindowFlags flags = SDL_GetWindowFlags(m_Handle);
    m_Geometry.systemRestored((flags & SDL_WINDOW_MAXIMIZED) != 0, (flags & SDL_WINDOW_MINIMIZED) != 0);
}

void Window::restore()
{
    if (m_Handle == nullptr)
    {
        return;
    }

    // On asynchronous windowing systems (X11) the restore is only a request until the window manager
    // answers. Wait for it before forgetting the normal rectangle: closed in between,
    // getNormalGeometry() would read the still-maximized live geometry and save it (#1121). Once per
    // user action (button, double-click, the start of a drag from maximized), never per frame; the
    // drag caller reads the restored size right after, which this also makes reliable.
    const auto syncRestore = [this]
    {
        SDL_SyncWindow(m_Handle);
    };

    // For borderless windows, use backend-gated behavior.
    // On native Wayland, rely on compositor-managed restore.
    // On X11, XWayland, and Windows, restore to manually-saved position/size.
    if ((SDL_GetWindowFlags(m_Handle) & SDL_WINDOW_BORDERLESS) != 0)
    {
        if (!VideoBackend::supportsClientSideMaximize())
        {
            // Native Wayland: use compositor-managed restore via SDL_RestoreWindow
            spdlog::debug("Window::restore: Native Wayland detected; using compositor-managed restore");
            SDL_RestoreWindow(m_Handle);
            syncRestore();
            m_Geometry.restored();
            return;
        }

        // X11, XWayland, Windows: restore to manually-saved position and size
        if (const std::optional<WindowGeometry::Rect> target = m_Geometry.restoreTarget(); m_Geometry.isMaximized() && target.has_value())
        {
            if ((SDL_GetWindowFlags(m_Handle) & SDL_WINDOW_MAXIMIZED) != 0)
            {
                // Maximized through the SDL fallback in maximize(), or by the window manager (#1250)
                SDL_RestoreWindow(m_Handle);
            }
            SDL_SetWindowPosition(m_Handle, target->x, target->y);
            SDL_SetWindowSize(m_Handle, target->width, target->height);
            syncRestore();
            m_Geometry.restored();
            return;
        }
    }

    SDL_RestoreWindow(m_Handle);
    syncRestore();
    m_Geometry.restored();
}

void Window::minimize() const
{
    if (m_Handle == nullptr)
    {
        return;
    }

    SDL_MinimizeWindow(m_Handle);
}

bool Window::isMinimized() const noexcept
{
    if (m_Handle == nullptr)
    {
        return false;
    }
    return (SDL_GetWindowFlags(m_Handle) & SDL_WINDOW_MINIMIZED) != 0;
}

bool Window::isOccluded() const noexcept
{
    if (m_Handle == nullptr)
    {
        return false;
    }
    // SDL sets the flag on SDL_EVENT_WINDOW_OCCLUDED and clears it on SDL_EVENT_WINDOW_EXPOSED.
    return (SDL_GetWindowFlags(m_Handle) & SDL_WINDOW_OCCLUDED) != 0;
}

double Window::getDisplayRefreshRate() const noexcept
{
    if (m_Handle == nullptr)
    {
        return 0.0;
    }
    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_Handle);
    if (display == 0)
    {
        return 0.0;
    }
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    if (mode == nullptr)
    {
        return 0.0;
    }
    // The exact rational rate when SDL has it (59.94 Hz is 60000/1001), else the rounded float.
    if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
    {
        return static_cast<double>(mode->refresh_rate_numerator) / static_cast<double>(mode->refresh_rate_denominator);
    }
    return static_cast<double>(mode->refresh_rate);
}

void Window::setHitTestCallback(SDL_HitTest callback, void* callbackData) const
{
    if (m_Handle == nullptr)
    {
        return;
    }

    if (!SDL_SetWindowHitTest(m_Handle, callback, callbackData))
    {
        spdlog::warn("Failed to set window hit test callback: {}", SDL_GetError());
    }
}

} // namespace Core
