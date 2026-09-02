#pragma once

#include <cstdint>
#include <string_view>

namespace Core
{

/// Video driver backend detection and capability queries.
/// Centralizes SDL_GetCurrentVideoDriver() calls and exposes semantic flags
/// for platform-specific behavior (maximize/restore, drag, etc).
class VideoBackend
{
  public:
    /// Detect the current video backend and return its capabilities.
    /// This is typically called once at startup and cached.
    static void initialize();

    /// Return true if the current backend is native Wayland (not XWayland).
    [[nodiscard]] static bool isWayland() noexcept;

    /// Return true if the current backend is X11.
    [[nodiscard]] static bool isX11() noexcept;

    /// Return true if the current backend is XWayland (X11 compatibility layer on Wayland).
    [[nodiscard]] static bool isXWaylandFallback() noexcept;

    /// Return true if the current backend is Windows.
    [[nodiscard]] static bool isWindows() noexcept;

    /// Return true if the current backend supports client-side positioning for maximize/restore.
    /// On native Wayland, this returns false; prefer compositor maximize/restore.
    /// On X11, XWayland, and Windows, this returns true.
    [[nodiscard]] static bool supportsClientSideMaximize() noexcept;

    /// Return true if the current backend supports global mouse state queries.
    /// Used for drag/resize operations; on Wayland this may be unreliable.
    [[nodiscard]] static bool supportsGlobalMouseState() noexcept;

    /// Return the current video driver name (for logging/debugging).
    [[nodiscard]] static std::string_view driverName() noexcept;

  private:
    enum class Backend : std::uint8_t
    {
        Unknown,
        Wayland,
        X11,
        XWaylandFallback,
        Windows
    };

    /// Detect the backend from the SDL video driver name and environment.
    [[nodiscard]] static Backend detectBackend() noexcept;

    static Backend s_Backend;
    static bool s_Initialized;
    static std::string_view s_DriverName;
};

} // namespace Core
