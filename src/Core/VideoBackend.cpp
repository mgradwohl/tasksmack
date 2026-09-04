#include "Core/VideoBackend.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <string>
#include <string_view>

namespace Core
{

std::mutex VideoBackend::s_Mutex;
VideoBackend::Backend VideoBackend::s_Backend = VideoBackend::Backend::Unknown;
bool VideoBackend::s_Initialized = false;
std::string VideoBackend::s_DriverName;

VideoBackend::Backend VideoBackend::classifyBackend(const std::string_view driverName, const bool waylandDisplaySet) noexcept
{
    if (driverName.empty())
    {
        return Backend::Unknown;
    }

    // Native Wayland.
    if (driverName == "wayland")
    {
        return Backend::Wayland;
    }

    // X11 or XWayland (X11 apps on a Wayland desktop). Native Wayland is caught by the "wayland"
    // check above; here SDL reports "x11" because we're either on a real X11 session, or using
    // XWayland's X11 server (WAYLAND_DISPLAY set) as a compatibility layer on a Wayland desktop.
    if (driverName == "x11")
    {
        return waylandDisplaySet ? Backend::XWaylandFallback : Backend::X11;
    }

#ifdef _WIN32
    if (driverName == "windows")
    {
        return Backend::Windows;
    }
#endif

    return Backend::Unknown;
}

// Not noexcept: its only caller, initialize(), isn't noexcept either, so there's no reason to
// convert a (very unlikely) spdlog exception here into a hard std::terminate() instead of letting
// it propagate normally.
VideoBackend::Backend VideoBackend::detectBackend()
{
    const char* driverNameCStr = SDL_GetCurrentVideoDriver();
    if (driverNameCStr == nullptr)
    {
        spdlog::warn("SDL_GetCurrentVideoDriver returned nullptr");
        return Backend::Unknown;
    }

    const std::string_view driverName = driverNameCStr;
    const bool waylandDisplaySet = SDL_getenv("WAYLAND_DISPLAY") != nullptr;
    const Backend backend = classifyBackend(driverName, waylandDisplaySet);

    switch (backend)
    {
    case Backend::Wayland:
        spdlog::info("Detected native Wayland");
        break;
    case Backend::X11:
        spdlog::info("Detected X11 video driver");
        break;
    case Backend::XWaylandFallback:
        spdlog::info("Detected XWayland fallback (WAYLAND_DISPLAY set)");
        break;
    case Backend::Windows:
        spdlog::info("Detected Windows video driver");
        break;
    case Backend::Unknown:
    default:
        spdlog::warn("Unknown video driver: {}", driverName);
        break;
    }

    return backend;
}

void VideoBackend::initialize()
{
    const std::scoped_lock lock(s_Mutex);
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

void VideoBackend::resetForTesting() noexcept
{
    const std::scoped_lock lock(s_Mutex);
    s_Backend = Backend::Unknown;
    s_Initialized = false;
    s_DriverName.clear();
}

bool VideoBackend::isWayland() noexcept
{
    const std::scoped_lock lock(s_Mutex);
    return s_Backend == Backend::Wayland;
}

bool VideoBackend::isX11() noexcept
{
    const std::scoped_lock lock(s_Mutex);
    return s_Backend == Backend::X11;
}

bool VideoBackend::isXWaylandFallback() noexcept
{
    const std::scoped_lock lock(s_Mutex);
    return s_Backend == Backend::XWaylandFallback;
}

bool VideoBackend::isWindows() noexcept
{
    const std::scoped_lock lock(s_Mutex);
    return s_Backend == Backend::Windows;
}

bool VideoBackend::supportsClientSideMaximize() noexcept
{
    // Native Wayland doesn't support client-side maximize/restore reliably;
    // prefer compositor-managed state. X11, XWayland, and Windows do support it.
    const std::scoped_lock lock(s_Mutex);
    return s_Backend != Backend::Wayland;
}

bool VideoBackend::supportsGlobalMouseState() noexcept
{
    // SDL_GetGlobalMouseState() is unreliable on native Wayland; the drag/resize layer must use
    // the window-local query there instead. X11, XWayland, and Windows all support it.
    const std::scoped_lock lock(s_Mutex);
    return s_Backend != Backend::Wayland;
}

std::string_view VideoBackend::driverName() noexcept
{
    const std::scoped_lock lock(s_Mutex);
    return s_DriverName;
}

bool VideoBackend::shouldUseBorderlessTitleBar(const bool forceNativeDecorationsOnWayland, const bool isWayland) noexcept
{
    return !(forceNativeDecorationsOnWayland && isWayland);
}

} // namespace Core
