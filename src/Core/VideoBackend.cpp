#include "Core/VideoBackend.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <string_view>

namespace Core
{

VideoBackend::Backend VideoBackend::s_Backend = VideoBackend::Backend::Unknown;
bool VideoBackend::s_Initialized = false;
std::string_view VideoBackend::s_DriverName = "";

namespace
{

/// Detect the backend from the SDL video driver name and environment.
[[nodiscard]] VideoBackend::Backend detectBackend() noexcept
{
    const char* driverNameCStr = SDL_GetCurrentVideoDriver();
    if (driverNameCStr == nullptr)
    {
        spdlog::warn("SDL_GetCurrentVideoDriver returned nullptr");
        return VideoBackend::Backend::Unknown;
    }

    const std::string_view driverName = driverNameCStr;

    // Check for native Wayland vs XWayland
    if (driverName == "wayland")
    {
        // Detect XWayland by checking the WAYLAND_DISPLAY and DISPLAY environment variables.
        // XWayland sets both; native Wayland only sets WAYLAND_DISPLAY.
        const char* waylandDisplay = SDL_getenv("WAYLAND_DISPLAY");
        const char* xDisplay = SDL_getenv("DISPLAY");

        if (xDisplay != nullptr && waylandDisplay != nullptr)
        {
            // Both set => XWayland fallback
            spdlog::info("Detected XWayland fallback (WAYLAND_DISPLAY={}, DISPLAY={})", waylandDisplay, xDisplay);
            return VideoBackend::Backend::XWaylandFallback;
        }

        // Only WAYLAND_DISPLAY set => native Wayland
        spdlog::info("Detected native Wayland (WAYLAND_DISPLAY={})", waylandDisplay != nullptr ? waylandDisplay : "<unset>");
        return VideoBackend::Backend::Wayland;
    }

    if (driverName == "x11")
    {
        spdlog::info("Detected X11 video driver");
        return VideoBackend::Backend::X11;
    }

#ifdef _WIN32
    if (driverName == "windows")
    {
        spdlog::info("Detected Windows video driver");
        return VideoBackend::Backend::Windows;
    }
#endif

    spdlog::warn("Unknown video driver: {}", driverName);
    return VideoBackend::Backend::Unknown;
}

} // namespace

void VideoBackend::initialize()
{
    if (s_Initialized)
    {
        return;
    }

    s_Backend = detectBackend();
    const char* driverNameCStr = SDL_GetCurrentVideoDriver();
    s_DriverName = driverNameCStr != nullptr ? driverNameCStr : "";

    s_Initialized = true;

    spdlog::info("VideoBackend initialized: backend={} driver={}", static_cast<int>(s_Backend), s_DriverName);
}

bool VideoBackend::isWayland() noexcept
{
    return s_Backend == Backend::Wayland;
}

bool VideoBackend::isX11() noexcept
{
    return s_Backend == Backend::X11;
}

bool VideoBackend::isXWaylandFallback() noexcept
{
    return s_Backend == Backend::XWaylandFallback;
}

bool VideoBackend::isWindows() noexcept
{
    return s_Backend == Backend::Windows;
}

bool VideoBackend::supportsClientSideMaximize() noexcept
{
    // Native Wayland doesn't support client-side maximize/restore reliably;
    // prefer compositor-managed state. X11, XWayland, and Windows do support it.
    return s_Backend != Backend::Wayland;
}

bool VideoBackend::supportsGlobalMouseState() noexcept
{
    // All backends support it, but on Wayland it may be unreliable during drag operations.
    // This is primarily informational for the drag/resize layer.
    return true;
}

std::string_view VideoBackend::driverName() noexcept
{
    return s_DriverName;
}

} // namespace Core
