#include "Window.h"

#include "Core/VideoBackend.h"
#include "Core/WindowConstants.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string_view>
#include <utility>

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
        if (!SDL_GL_SetSwapInterval(-1))
        {
            SDL_GL_SetSwapInterval(1);
        }
    }
    else
    {
        SDL_GL_SetSwapInterval(0);
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
        if (!SDL_GL_SetSwapInterval(-1))
        {
            SDL_GL_SetSwapInterval(1);
        }
    }
    else
    {
        SDL_GL_SetSwapInterval(0);
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
    return !VideoBackend::isWayland();
}

void Window::setSize(int width, int height)
{
    if (m_Handle == nullptr)
    {
        return;
    }

    const int clampedWidth = clampWindowDimension(width);
    const int clampedHeight = clampWindowDimension(height);
    SDL_SetWindowSize(m_Handle, clampedWidth, clampedHeight);
    // Block until the OS has applied the resize so that subsequent SDL_GetWindowSize
    // calls return the new dimensions immediately. On asynchronous windowing systems
    // (X11, Wayland) this pumps X11 events internally and waits for the ConfigureNotify.
    // NOTE: Do not call setSize() from the render loop or from any hot path — this call
    // can block for the duration of a window-manager animation on async platforms.
    SDL_SyncWindow(m_Handle);
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
        return m_IsMaximizedBorderless;
    }

    return (SDL_GetWindowFlags(m_Handle) & SDL_WINDOW_MAXIMIZED) != 0;
}

void Window::maximize()
{
    if (m_Handle == nullptr)
    {
        return;
    }

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
            m_IsMaximizedBorderless = true;
            return;
        }

        // X11, XWayland, Windows: use client-side maximize with manual positioning
        // Only save restore dimensions if not already maximized
        // This prevents saving maximized dimensions as the restore target
        if (!m_IsMaximizedBorderless)
        {
            SDL_GetWindowPosition(m_Handle, &m_RestoreX, &m_RestoreY);
            SDL_GetWindowSize(m_Handle, &m_RestoreWidth, &m_RestoreHeight);
        }

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
                m_IsMaximizedBorderless = true;
                return;
            }
            // If SDL_GetDisplayUsableBounds fails, fall through to SDL_MaximizeWindow
            spdlog::warn("Failed to get display usable bounds for borderless maximize");
        }
    }

    // Fall back to SDL's built-in maximize for non-borderless windows or on error
    SDL_MaximizeWindow(m_Handle);
}

void Window::restore()
{
    if (m_Handle == nullptr)
    {
        return;
    }

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
            m_IsMaximizedBorderless = false;
            return;
        }

        // X11, XWayland, Windows: restore to manually-saved position and size
        if (m_IsMaximizedBorderless && m_RestoreWidth > 0 && m_RestoreHeight > 0)
        {
            SDL_SetWindowPosition(m_Handle, m_RestoreX, m_RestoreY);
            SDL_SetWindowSize(m_Handle, m_RestoreWidth, m_RestoreHeight);
            m_IsMaximizedBorderless = false;
            return;
        }
    }

    SDL_RestoreWindow(m_Handle);
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
