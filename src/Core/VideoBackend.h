#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace Core
{

/// Video driver backend detection and capability queries.
/// Centralizes SDL_GetCurrentVideoDriver() calls and exposes semantic flags
/// for platform-specific behavior (maximize/restore, drag, etc).
class VideoBackend
{
  public:
    enum class Backend : std::uint8_t
    {
        Unknown,
        Wayland,
        X11,
        XWaylandFallback,
        Windows
    };

    /// Pure classification policy, with no SDL/environment calls of its own, so it can be
    /// exhaustively table-tested (Wayland, X11, XWayland, Windows, unknown, and unset/empty
    /// driver name) without depending on whichever real or headless SDL video driver happens
    /// to be active in the process running the test. `driverName` is SDL_GetCurrentVideoDriver()'s
    /// return value; `waylandDisplaySet` is whether the WAYLAND_DISPLAY environment variable was
    /// non-null. On non-Windows builds, `driverName == "windows"` never classifies as Backend::Windows
    /// (that check is compiled out), matching detectBackend()'s real behavior on those platforms.
    [[nodiscard]] static Backend classifyBackend(std::string_view driverName, bool waylandDisplaySet) noexcept;

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

    /// Pure policy: should the window use the custom borderless title bar (true), or native
    /// OS/compositor decorations (false)? Parameterized rather than reading live isWayland()
    /// state, so it's directly unit-testable without live SDL video-backend state. See #745:
    /// forceNativeDecorationsOnWayland is a user opt-in that only has an effect on native
    /// Wayland; every other backend always stays borderless (custom title bar).
    [[nodiscard]] static bool shouldUseBorderlessTitleBar(bool forceNativeDecorationsOnWayland, bool isWayland) noexcept;

    /// Return the current video driver name (for logging/debugging). Returns an owning copy,
    /// not a view into s_DriverName: a view could be invalidated by a later resetForTesting()/
    /// initialize() call that mutates s_DriverName out from under it (#789 follow-up review).
    /// Not noexcept: the copy can allocate and throw std::bad_alloc.
    [[nodiscard]] static std::string driverName();

    /// Test-only: clear the cached backend so the next initialize() call re-detects from the
    /// live SDL state instead of being a no-op. Without this, a test that forces a specific
    /// SDL_HINT_VIDEO_DRIVER and constructs a second Application/Window in the same test
    /// binary would silently observe whichever backend the first initialize() call detected.
    static void resetForTesting() noexcept;

  private:
    /// Detect the backend from the SDL video driver name and environment.
    [[nodiscard]] static Backend detectBackend();

    // Guards all three statics below. Every caller today runs on the main/render thread by
    // convention only, not by design (initialize() is called once from Application's
    // constructor); the mutex makes that an actual guarantee rather than a hazard waiting for
    // a future caller (e.g. a background-sampler-thread probe) to violate it.
    static std::mutex s_Mutex;
    static Backend s_Backend;
    static bool s_Initialized;
    // Owning storage, not a view: SDL_GetCurrentVideoDriver()'s returned pointer is only valid
    // until SDL_Quit(), and nothing in production code re-detects or clears this around that
    // call, so a std::string_view here could dangle after shutdown in any process with more
    // than one Application/Window lifecycle (the test suite already exercises that via the
    // stack-allocated Application path) -- #780.
    static std::string s_DriverName;
};

} // namespace Core
