#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/VideoBackend.h"
#include "Core/Window.h"
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{

bool isOffscreenVideoDriver()
{
#ifdef _WIN32
    return false;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay) - read-only env access during test execution
    const char* videoDriver = std::getenv("SDL_VIDEODRIVER");
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    return videoDriver != nullptr && std::string_view(videoDriver) == "offscreen";
#endif
}

bool detectDisplay()
{
#ifdef _WIN32
    // Check for CI environment - headless Windows CI runners cannot create windows.
    // Mirror the guard used in test_Application.cpp: use _dupenv_s to avoid the
    // MSVC CRT deprecation warning on std::getenv that is treated as an error under /WX.
    char* ciEnv = nullptr;
    std::size_t len = 0;
    _dupenv_s(&ciEnv, &len, "CI");
    const bool isCI = (ciEnv != nullptr && std::string_view(ciEnv) == "true");
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) - _dupenv_s allocates with malloc; must free with free()
    free(ciEnv);
    if (isCI)
    {
        return false;
    }
    return true;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    if ((display != nullptr && display[0] != '\0') || (waylandDisplay != nullptr && waylandDisplay[0] != '\0'))
    {
        // Xvfb and other virtual displays expose DISPLAY but may lack a usable GL
        // stack. Probe GL capability before committing to the real-display path so
        // tests fall through to the offscreen fallback rather than hitting FAIL().
        if (TestSupport::probeGLCapability())
        {
            return true;
        }
    }

    return TestSupport::tryEnableOffscreenVideoDriver();
#endif
}

// Every display check goes through here so TASKSMACK_REQUIRE_DISPLAY=1 (set by Linux CI) turns a
// missing display into a failure instead of a skip.
bool hasDisplay()
{
    return TestSupport::enforceDisplayRequirement(detectDisplay());
}

} // namespace

namespace Core
{
namespace
{

// Window::setSize() calls SDL_SyncWindow(), but that only waits a bounded time (100ms on X11),
// and on a real display the resize is asynchronous (#1309): SDL only updates the size
// getWidth()/getHeight() report when it processes the resulting configure event, which can come
// later than that under load or under a compositing window manager (WSLg). Pump events until the
// window reports the expected size or the deadline passes; the caller still asserts the size, so
// a resize that never lands still fails.
void waitForWindowSize(const Window& window, int width, int height)
{
    constexpr auto RESIZE_TIMEOUT = std::chrono::seconds{5};
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds{10};
    const auto deadline = std::chrono::steady_clock::now() + RESIZE_TIMEOUT;
    while (window.getSize() != std::pair{width, height} && std::chrono::steady_clock::now() < deadline)
    {
        SDL_PumpEvents();
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
}

// True when the window's display is smaller than width x height on either axis.
bool displaySmallerThan(const Window& window, int width, int height)
{
    SDL_Rect bounds{};
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window.getHandle());
    return display != 0 && SDL_GetDisplayBounds(display, &bounds) && (bounds.w < width || bounds.h < height);
}

// Fixture that mirrors the Application constructor/destructor SDL lifecycle.
// SDL_Init(SDL_INIT_VIDEO) must be called before SDL_CreateWindow; without it
// Window construction fails and tests are silently skipped rather than exercising
// the Window methods they intend to cover.
class WindowTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!hasDisplay())
        {
            GTEST_SKIP() << "No display available (headless environment)";
        }
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            if (TestSupport::displayRequired())
            {
                FAIL() << "SDL_Init(SDL_INIT_VIDEO) failed with TASKSMACK_REQUIRE_DISPLAY=1: " << SDL_GetError();
            }
            GTEST_SKIP() << "SDL_Init(SDL_INIT_VIDEO) failed: " << SDL_GetError();
        }
        m_SdlInitialized = true;
    }

    void TearDown() override
    {
        if (m_SdlInitialized)
        {
            SDL_Quit();
            m_SdlInitialized = false;
        }
    }

    bool m_SdlInitialized = false;
};

TEST_F(WindowTest, CloseRequestLifecycle)
{
    try
    {
        Window window(WindowSpecification{.Title = "WindowCloseTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        EXPECT_FALSE(window.shouldClose());
        window.requestClose();
        EXPECT_TRUE(window.shouldClose());
        window.clearCloseRequest();
        EXPECT_FALSE(window.shouldClose());
    }
    catch (const std::exception& e)
    {
        // Offscreen SDL driver does not support OpenGL context creation; skip.
        // On a real display this is an unexpected failure — report it.
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

TEST_F(WindowTest, GetGLContextReturnsNonNullAfterConstruction)
{
    try
    {
        Window window(WindowSpecification{.Title = "WindowGLContextTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        EXPECT_NE(window.getGLContext(), nullptr);
    }
    catch (const std::exception& e)
    {
        // Offscreen SDL driver does not support OpenGL context creation; skip.
        // On a real display this is an unexpected failure — report it.
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

TEST_F(WindowTest, SetSizeClampsToExpectedBounds)
{
    try
    {
        Window window(WindowSpecification{.Title = "WindowClampTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        window.setSize(0, -10);
        waitForWindowSize(window, WINDOW_MIN_DIMENSION, WINDOW_MIN_DIMENSION);
        EXPECT_EQ(window.getWidth(), WINDOW_MIN_DIMENSION);
        EXPECT_EQ(window.getHeight(), WINDOW_MIN_DIMENSION);

        const auto sizeBefore = window.getSize();
        window.setSize(20000, 50000);
        waitForWindowSize(window, WINDOW_MAX_DIMENSION, WINDOW_MAX_DIMENSION);
        // A window manager may refuse to grow a window past the display: WSLg's (Weston's XWM)
        // leaves this 200x200 window at 200x200 in roughly a third of runs, even when asked again
        // and again, while the X server alone (Xvfb in CI, no window manager) always applies it.
        // That's the environment declining, not the clamp misbehaving -- skip, as
        // SetAndGetPositionRoundTrip does for a declined move. TASKSMACK_REQUIRE_DISPLAY=1 (CI)
        // keeps the strict assertion.
        if (window.getSize() == sizeBefore && sizeBefore != std::pair{WINDOW_MAX_DIMENSION, WINDOW_MAX_DIMENSION} &&
            displaySmallerThan(window, WINDOW_MAX_DIMENSION, WINDOW_MAX_DIMENSION) && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window manager declined to grow the window past the display; the minimum clamp was verified";
        }
        EXPECT_EQ(window.getWidth(), WINDOW_MAX_DIMENSION);
        EXPECT_EQ(window.getHeight(), WINDOW_MAX_DIMENSION);
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

TEST_F(WindowTest, GetWidthAndHeightReflectCurrentSDLSize)
{
    // Resize through SDL directly so a cached WindowSpecification cannot satisfy the assertions.
    try
    {
        Window window(WindowSpecification{.Title = "WindowLiveSizeTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        EXPECT_EQ(window.getWidth(), 640);
        EXPECT_EQ(window.getHeight(), 480);

        if (!SDL_SetWindowSize(window.getHandle(), 512, 384) || !SDL_SyncWindow(window.getHandle()))
        {
            GTEST_SKIP() << "Display server could not apply the first window resize: " << SDL_GetError();
        }
        EXPECT_EQ(window.getWidth(), 512);
        EXPECT_EQ(window.getHeight(), 384);

        if (!SDL_SetWindowSize(window.getHandle(), 576, 432) || !SDL_SyncWindow(window.getHandle()))
        {
            GTEST_SKIP() << "Display server could not apply the second window resize: " << SDL_GetError();
        }
        EXPECT_EQ(window.getWidth(), 576);
        EXPECT_EQ(window.getHeight(), 432);
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

TEST_F(WindowTest, GetSizeInPixelsReturnsPositiveDimensions)
{
    try
    {
        Window window(WindowSpecification{.Title = "WindowPixelSizeTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        const auto [pixelW, pixelH] = window.getSizeInPixels();
        // Physical pixel size must be at least as large as logical size on any display.
        EXPECT_GE(pixelW, 640);
        EXPECT_GE(pixelH, 480);
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

TEST_F(WindowTest, SetAndGetPositionRoundTrip)
{
    if (isOffscreenVideoDriver())
    {
        GTEST_SKIP() << "Offscreen SDL driver does not guarantee window position semantics";
    }

    try
    {
        Window window(WindowSpecification{.Title = "WindowPositionTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        window.setPosition(40, 60);
        const auto [x, y] = window.getPosition();

        // Most compositors and virtual framebuffers (Xvfb, llvmpipe) silently
        // ignore or remap position requests. Skip rather than fail in those cases.
        if (x != 40 || y != 60)
        {
            GTEST_SKIP() << "Window manager did not honour setPosition() — skipping on this display";
        }
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

TEST_F(WindowTest, WindowStateControlMethodsDoNotThrow)
{
    try
    {
        Window window(WindowSpecification{.Title = "WindowStateTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        EXPECT_NO_THROW(window.minimize());
        EXPECT_NO_THROW(window.maximize());
        EXPECT_NO_THROW(window.restore());
        EXPECT_NO_THROW(window.swapBuffers());
        EXPECT_TRUE(window.isBorderless());
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

TEST_F(WindowTest, IsMaximizedTracksStateForBorderlessWindow)
{
    try
    {
        Window window(WindowSpecification{.Title = "WindowMaximizeTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        // A freshly-created borderless window should not be maximized.
        EXPECT_FALSE(window.isMaximized());

        // maximize() sets m_IsMaximizedBorderless only when both
        // SDL_GetDisplayForWindow() and SDL_GetDisplayUsableBounds() succeed.
        // In headless / offscreen environments those calls fail and the flag
        // stays false. Skip rather than fail in that case.
        window.maximize();
        if (!window.isMaximized())
        {
            GTEST_SKIP() << "SDL display-usable-bounds path unavailable; "
                            "borderless maximize flag not set (headless environment)";
        }

        // After restore(), the flag must be cleared.
        window.restore();
        EXPECT_FALSE(window.isMaximized());
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

TEST_F(WindowTest, NormalGeometryIsTheLiveGeometryWhenNotMaximized)
{
    try
    {
        const Window window(
            WindowSpecification{.Title = "NormalGeometryTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        const auto normalGeometry = window.getNormalGeometry();
        ASSERT_TRUE(normalGeometry.has_value());
        const WindowGeometry::Rect normal = normalGeometry.value_or(WindowGeometry::Rect{});
        const auto [width, height] = window.getSize();
        EXPECT_EQ(normal.width, width);
        EXPECT_EQ(normal.height, height);
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

// #1121: the geometry saved on exit while maximized must be the size the window restores to, not
// the maximized size.
TEST_F(WindowTest, NormalGeometrySurvivesMaximize)
{
    try
    {
        Window window(
            WindowSpecification{.Title = "NormalGeometryMaxTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        const auto beforeGeometry = window.getNormalGeometry();
        ASSERT_TRUE(beforeGeometry.has_value());
        const WindowGeometry::Rect before = beforeGeometry.value_or(WindowGeometry::Rect{});

        window.maximize();
        if (!window.isMaximized())
        {
            GTEST_SKIP() << "Maximize unavailable on this display (headless environment)";
        }

        const auto normalGeometry = window.getNormalGeometry();
        if (!normalGeometry.has_value())
        {
            GTEST_SKIP() << "Compositor-managed maximize did not leave a known restore rectangle";
        }
        const WindowGeometry::Rect normal = normalGeometry.value_or(WindowGeometry::Rect{});
        EXPECT_EQ(normal.width, before.width);
        EXPECT_EQ(normal.height, before.height);

        // A second maximize() must not replace the restore rectangle with the maximized one.
        window.maximize();
        const auto againGeometry = window.getNormalGeometry();
        ASSERT_TRUE(againGeometry.has_value());
        const WindowGeometry::Rect again = againGeometry.value_or(WindowGeometry::Rect{});
        EXPECT_EQ(again.width, before.width);
        EXPECT_EQ(again.height, before.height);
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

// #1128: a saved position on no connected display must not be applied as-is.
TEST_F(WindowTest, ApplySavedGeometryKeepsAnOffScreenPositionOnADisplay)
{
    // supportsPositioning() reads the cached backend, which Application normally detects after
    // SDL_Init; this fixture has no Application, so detect it here.
    VideoBackend::initialize();
    if (isOffscreenVideoDriver() || !Window::supportsPositioning())
    {
        GTEST_SKIP() << "Window positioning is not supported by this video driver";
    }
    try
    {
        Window window(WindowSpecification{.Title = "SavedGeometryTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        window.applySavedGeometry(std::pair{-50'000, -50'000}, false, std::nullopt);

        int displayCount = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
        if (displays == nullptr || displayCount <= 0)
        {
            SDL_free(displays);
            GTEST_SKIP() << "No display bounds available";
        }
        std::vector<SDL_Rect> displayBounds;
        for (const SDL_DisplayID id : std::span<const SDL_DisplayID>(displays, static_cast<std::size_t>(displayCount)))
        {
            SDL_Rect bounds{};
            if (SDL_GetDisplayBounds(id, &bounds))
            {
                displayBounds.push_back(bounds);
            }
        }
        SDL_free(displays);
        const auto onADisplay = [&displayBounds](std::pair<int, int> position)
        {
            const SDL_Point topLeft{.x = position.first, .y = position.second};
            return std::ranges::any_of(displayBounds, [&topLeft](const SDL_Rect& bounds) { return SDL_PointInRect(&topLeft, &bounds); });
        };

        // On X11 the move is only a request until the window manager answers, and setPosition()
        // doesn't wait for it (#1363), so the position read straight away can still be the creation
        // position -- which may itself be on a display, hiding a move that went off-screen. Wait for
        // every pending request to be applied before checking: SDL_SyncWindow() waits a bounded time
        // (100ms on X11) and returns false if it timed out, so retry it until it succeeds or the
        // deadline passes, and fail if it never does.
        constexpr auto SYNC_TIMEOUT = std::chrono::seconds{5};
        const auto deadline = std::chrono::steady_clock::now() + SYNC_TIMEOUT;
        bool synced = SDL_SyncWindow(window.getHandle());
        while (!synced && std::chrono::steady_clock::now() < deadline)
        {
            SDL_PumpEvents();
            synced = SDL_SyncWindow(window.getHandle());
        }
        ASSERT_TRUE(synced) << "the saved-geometry move was not applied within " << SYNC_TIMEOUT.count() << " s";
        const auto [x, y] = window.getPosition();
        EXPECT_TRUE(onADisplay({x, y})) << "window top-left at (" << x << ", " << y << ")";
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

#ifdef _WIN32
// #1279: the borderless window's subclass procedure turns only Win+Down into a restore. Every other
// shell minimize of the client-side maximized window -- the taskbar button, here, with no key held --
// still reaches SDL and minimizes it, and the window comes back maximized, as a native one does.
TEST_F(WindowTest, ShellMinimizeWithoutWinDownStillMinimizesTheMaximizedWindow)
{
    try
    {
        Window window(WindowSpecification{.Title = "ShellMinimizeTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        // No Win+Down, whatever the real keyboard is doing.
        window.setShellRestoreKeysReader([] noexcept { return std::pair{false, false}; });
        window.maximize();
        if (!window.isMaximized())
        {
            GTEST_SKIP() << "Maximize unavailable on this display (headless environment)";
        }
        auto* const hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window.getHandle()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        ASSERT_NE(hwnd, nullptr);

        SendMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        SDL_PumpEvents();

        EXPECT_TRUE(window.isMinimized());
        EXPECT_TRUE(window.isMaximized());
        EXPECT_FALSE(window.restoreForShellMinimize()); // Win+Down is not held
        EXPECT_TRUE(window.isMinimized());
    }
    catch (const std::exception& e)
    {
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

// #1279: the fix itself. With Win+Down held, the shell's minimize of the client-side maximized
// window is dropped and the window is restored to its normal rectangle instead, as the first
// Win+Down restores a native maximized window. The key state is injected: a locked or headless
// desktop cannot hold real keys.
TEST_F(WindowTest, ShellMinimizeWithWinDownRestoresTheMaximizedWindow)
{
    try
    {
        Window window(WindowSpecification{.Title = "ShellRestoreTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        window.setShellRestoreKeysReader([] noexcept { return std::pair{true, true}; });
        const auto normalGeometry = window.getNormalGeometry();
        ASSERT_TRUE(normalGeometry.has_value());
        const WindowGeometry::Rect normal = normalGeometry.value_or(WindowGeometry::Rect{});
        window.maximize();
        if (!window.isMaximized())
        {
            GTEST_SKIP() << "Maximize unavailable on this display (headless environment)";
        }
        auto* const hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window.getHandle()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        ASSERT_NE(hwnd, nullptr);

        SendMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        SDL_PumpEvents();

        EXPECT_FALSE(window.isMinimized());
        EXPECT_FALSE(window.isMaximized());
        EXPECT_EQ(window.getSize(), (std::pair{normal.width, normal.height}));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

// #1279: a minimize that did not pass through WM_SYSCOMMAND (here SDL_MinimizeWindow(), standing in
// for one the shell carried out directly) is undone from SDL_EVENT_WINDOW_MINIMIZED: the window is
// brought back and restored to its normal rectangle.
TEST_F(WindowTest, MinimizeAlreadyCarriedOutWithWinDownIsUndoneAndRestored)
{
    try
    {
        Window window(
            WindowSpecification{.Title = "ShellRestoreFallbackTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        window.setShellRestoreKeysReader([] noexcept { return std::pair{true, true}; });
        const auto normalGeometry = window.getNormalGeometry();
        ASSERT_TRUE(normalGeometry.has_value());
        const WindowGeometry::Rect normal = normalGeometry.value_or(WindowGeometry::Rect{});
        window.maximize();
        if (!window.isMaximized())
        {
            GTEST_SKIP() << "Maximize unavailable on this display (headless environment)";
        }
        window.minimize();
        SDL_PumpEvents();
        ASSERT_TRUE(window.isMinimized());

        EXPECT_TRUE(window.restoreForShellMinimize());

        EXPECT_FALSE(window.isMinimized());
        EXPECT_FALSE(window.isMaximized());
        EXPECT_EQ(window.getSize(), (std::pair{normal.width, normal.height}));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}
#endif // _WIN32: the shell's Win+Down minimize is Windows behaviour (#1279)

TEST_F(WindowTest, SetHitTestCallbackDoesNotThrow)
{
    try
    {
        Window window(WindowSpecification{.Title = "HitTestCallbackTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        // Setting a no-op hit-test callback must not crash or throw.
        EXPECT_NO_THROW(window.setHitTestCallback([](SDL_Window* /*win*/, const SDL_Point* /*area*/, void* /*data*/) -> SDL_HitTestResult
                                                  { return SDL_HITTEST_NORMAL; },
                                                  nullptr));

        // Clearing the callback (nullptr) must also be safe.
        EXPECT_NO_THROW(window.setHitTestCallback(nullptr, nullptr));
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

} // namespace
} // namespace Core
