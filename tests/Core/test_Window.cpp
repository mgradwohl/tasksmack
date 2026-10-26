#include "Core/GraphicsHostInfo.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/VideoBackend.h"
#include "Core/Window.h"
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"
#include "Core/WindowSizeTestUtils.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
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
    // A GL 3.3 core context on the desktop, CI included (#2134), as test_Application.cpp checks.
    return TestSupport::detectWindowsDisplay();
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

// Pumps events until the window's size differs from @p before, or a timeout (#1896).
void waitForWindowSizeChange(const Window& window, std::pair<int, int> before)
{
    constexpr auto RESIZE_TIMEOUT = std::chrono::seconds{5};
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds{10};
    const auto deadline = std::chrono::steady_clock::now() + RESIZE_TIMEOUT;
    while (window.getSize() == before && std::chrono::steady_clock::now() < deadline)
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

// Frees the array SDL_GetDisplays() returns.
struct SdlFree
{
    void operator()(SDL_DisplayID* ids) const noexcept
    {
        SDL_free(ids);
    }
};

using DisplayIds = std::unique_ptr<SDL_DisplayID, SdlFree>;

// The rate a display mode advertises: its exact rational rate when SDL has one (59.94 Hz is
// 60000/1001), else the float rate SDL rounds it to, which SDL leaves at 0 when it doesn't know.
double advertisedRefreshHz(const SDL_DisplayMode& mode)
{
    if (mode.refresh_rate_numerator > 0 && mode.refresh_rate_denominator > 0)
    {
        return static_cast<double>(mode.refresh_rate_numerator) / static_cast<double>(mode.refresh_rate_denominator);
    }
    return static_cast<double>(mode.refresh_rate);
}

// What SDL says about a display mode's rate, for a failure message.
std::string describeRefresh(const SDL_DisplayMode& mode)
{
    std::ostringstream text;
    text << "refresh_rate_numerator=" << mode.refresh_rate_numerator << " refresh_rate_denominator=" << mode.refresh_rate_denominator
         << " refresh_rate=" << mode.refresh_rate;
    return text.str();
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
        const Window window(
            WindowSpecification{.Title = "WindowGLContextTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
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
        // Once a parallel ctest run read it as 1x1 on WSLg (#2650); skip that on a desktop.
        if (!TestSupport::waitForWindowSize(window, WINDOW_MIN_DIMENSION, WINDOW_MIN_DIMENSION) && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << TestSupport::sizeNotAppliedMessage(window, WINDOW_MIN_DIMENSION, WINDOW_MIN_DIMENSION);
        }
        EXPECT_EQ(window.getWidth(), WINDOW_MIN_DIMENSION);
        EXPECT_EQ(window.getHeight(), WINDOW_MIN_DIMENSION);

        const auto sizeBefore = window.getSize();
        window.setSize(20000, 50000);
        (void) TestSupport::waitForWindowSize(window, WINDOW_MAX_DIMENSION, WINDOW_MAX_DIMENSION);
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
        const Window window(
            WindowSpecification{.Title = "WindowLiveSizeTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        if (!TestSupport::waitForWindowSize(window, 640, 480) && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << TestSupport::sizeNotAppliedMessage(window, 640, 480);
        }
        EXPECT_EQ(window.getWidth(), 640);
        EXPECT_EQ(window.getHeight(), 480);

        if (!SDL_SetWindowSize(window.getHandle(), 512, 384) || !SDL_SyncWindow(window.getHandle()))
        {
            GTEST_SKIP() << "Display server could not apply the first window resize: " << SDL_GetError();
        }
        if (!TestSupport::waitForWindowSize(window, 512, 384) && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << TestSupport::sizeNotAppliedMessage(window, 512, 384);
        }
        EXPECT_EQ(window.getWidth(), 512);
        EXPECT_EQ(window.getHeight(), 384);

        if (!SDL_SetWindowSize(window.getHandle(), 576, 432) || !SDL_SyncWindow(window.getHandle()))
        {
            GTEST_SKIP() << "Display server could not apply the second window resize: " << SDL_GetError();
        }
        if (!TestSupport::waitForWindowSize(window, 576, 432) && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << TestSupport::sizeNotAppliedMessage(window, 576, 432);
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
        const Window window(
            WindowSpecification{.Title = "WindowPixelSizeTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        if (!TestSupport::waitForWindowSize(window, 640, 480) && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << TestSupport::sizeNotAppliedMessage(window, 640, 480);
        }
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
        const Window window(
            WindowSpecification{.Title = "WindowPositionTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
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

// #2722: the client-side maximize (Windows, X11 without a window manager's EWMH maximize) reads the
// display's usable bounds through a seam, so a test can choose them, or make the read fail.
constexpr SDL_Rect CHOSEN_USABLE_BOUNDS{.x = 100, .y = 60, .w = 900, .h = 640};

int& usableBoundsReads() noexcept
{
    static int reads = 0;
    return reads;
}

bool readChosenUsableBounds(SDL_DisplayID /*display*/, SDL_Rect* bounds) noexcept
{
    ++usableBoundsReads();
    *bounds = CHOSEN_USABLE_BOUNDS;
    return true;
}

bool failUsableBoundsRead(SDL_DisplayID /*display*/, SDL_Rect* /*bounds*/) noexcept
{
    ++usableBoundsReads();
    return false;
}

TEST_F(WindowTest, AClientSideMaximizeFillsTheDisplaysUsableBounds)
{
    try
    {
        Window window(WindowSpecification{.Title = "UsableBoundsTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        window.setUsableBoundsReader(&readChosenUsableBounds);
        usableBoundsReads() = 0;
        window.maximize();
        if (usableBoundsReads() == 0)
        {
            GTEST_SKIP() << "The compositor or window manager maximizes here, not the client";
        }
        SDL_SyncWindow(window.getHandle());

        EXPECT_TRUE(window.isMaximized());
        EXPECT_EQ(window.getSize(), (std::pair{CHOSEN_USABLE_BOUNDS.w, CHOSEN_USABLE_BOUNDS.h}));
        if (!isOffscreenVideoDriver()) // the offscreen driver keeps a window where it was made
        {
            EXPECT_EQ(window.getPosition(), (std::pair{CHOSEN_USABLE_BOUNDS.x, CHOSEN_USABLE_BOUNDS.y}));
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

/// WindowTest, with the default logger writing to m_Logged for the test.
class WindowLogTest : public WindowTest
{
  protected:
    void SetUp() override
    {
        m_PreviousLogger = spdlog::default_logger();
        spdlog::set_default_logger(
            std::make_shared<spdlog::logger>("window-log-test", std::make_shared<spdlog::sinks::ostream_sink_mt>(m_Logged)));
        WindowTest::SetUp();
    }

    void TearDown() override
    {
        WindowTest::TearDown();
        spdlog::set_default_logger(m_PreviousLogger);
    }

    std::ostringstream m_Logged;

  private:
    std::shared_ptr<spdlog::logger> m_PreviousLogger;
};

// #865 checked this by hand: when the usable bounds can't be read, maximize() warns and asks SDL to
// maximize instead, and tracks that as a maximize, so the normal geometry isn't lost (#1121).
TEST_F(WindowLogTest, AClientSideMaximizeWithoutUsableBoundsWarnsAndAsksSDLInstead)
{
    try
    {
        Window window(WindowSpecification{.Title = "UsableBoundsTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        window.setUsableBoundsReader(&failUsableBoundsRead);
        const std::optional<WindowGeometry::Rect> normal = window.getNormalGeometry();
        ASSERT_TRUE(normal.has_value());
        usableBoundsReads() = 0;
        window.maximize();
        if (usableBoundsReads() == 0)
        {
            GTEST_SKIP() << "The compositor or window manager maximizes here, not the client";
        }

        EXPECT_TRUE(m_Logged.str().contains("Failed to get display usable bounds for borderless maximize")) << m_Logged.str();
        if (!isOffscreenVideoDriver()) // the offscreen driver can't maximize, so SDL's fallback fails too
        {
            EXPECT_TRUE(window.isMaximized());
        }
        EXPECT_EQ(window.getNormalGeometry(), normal);
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
        const Window window(
            WindowSpecification{.Title = "HitTestCallbackTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

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

TEST_F(WindowTest, ASizeSavedAtTwiceTheScaleComesBackAtHalfTheSize)
{
    VideoBackend::initialize();
    try
    {
        Window window(WindowSpecification{.Title = "SavedScaleTest", .Width = 800, .Height = 600, .VSync = false, .Borderless = true});
        // applySavedGeometry() converts the current size, so it must be the created one: read as 1x1
        // on WSLg it converts to the 200x200 minimum (#2706).
        if (!TestSupport::waitForWindowSize(window, 800, 600) && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << TestSupport::sizeNotAppliedMessage(window, 800, 600);
        }
        const float scale = window.getUnitScale();
        if (scale <= 0.0F)
        {
            GTEST_SKIP() << "No window scale on this display";
        }
        // Saved on a display at twice this one's scale: the same apparent size is half as many units
        // here (#1168). No position, so only the size is converted and fitted.
        window.applySavedGeometry(std::nullopt, false, scale * 2.0F);
        if (!TestSupport::waitForWindowSize(window, 400, 300) && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << TestSupport::sizeNotAppliedMessage(window, 400, 300);
        }
        EXPECT_EQ(window.getSize(), (std::pair{400, 300}));
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << e.what();
    }
}

TEST_F(WindowTest, ASavedSizeLargerThanTheDisplayIsShrunkToFit)
{
    VideoBackend::initialize();
    try
    {
        Window window(WindowSpecification{.Title = "SavedSizeFitTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        SDL_Rect usable{};
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window.getHandle());
        if (display == 0 || !SDL_GetDisplayUsableBounds(display, &usable) || usable.w <= 0 || usable.h <= 0)
        {
            GTEST_SKIP() << "No display bounds available";
        }
        // A size saved on a bigger monitor, now gone: kept within this display rather than off its edges.
        const auto sizeBefore = window.getSize();
        window.setSize(std::min(usable.w * 2, 16'000), std::min(usable.h * 2, 16'000));
        // On X11 the resize is only a request: applySavedGeometry() reads the current size, so wait until
        // the resize lands, or the fit sees 640x480, has nothing to shrink, and the 2x size lands after
        // it (#1896). A window manager may clamp the grow at the display (WSLg's XWM), so wait for any
        // change, not the exact size.
        waitForWindowSizeChange(window, sizeBefore);
        if (window.getSize() == sizeBefore && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window manager declined to grow the window; nothing to shrink";
        }
        window.applySavedGeometry(std::nullopt, false, std::nullopt);
        (void) TestSupport::waitForWindowSize(window, usable.w, usable.h);
        // The grow can carry the window onto a bigger monitor, and the fit is to the display it is on
        // then, so compare with that display's bounds, not the one it started on (#2088).
        SDL_Rect fitted = usable;
        if (const SDL_DisplayID now = SDL_GetDisplayForWindow(window.getHandle()); now != 0 && now != display)
        {
            ASSERT_TRUE(SDL_GetDisplayUsableBounds(now, &fitted));
        }
        const auto [width, height] = window.getSize();
        EXPECT_LE(width, fitted.w);
        EXPECT_LE(height, fitted.h);
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << e.what();
    }
}

// queryGraphicsHostInfo() reports the live SDL environment (#2966): SDL's video driver, every display
// SDL enumerates in SDL's order, and each display's bounds and current mode, the logical bounds kept
// apart from the mode's pixel size. Nothing here assumes how many displays there are, their sizes or
// rates, HDR, or the backend: every expectation is read from SDL on the same machine.
TEST_F(WindowTest, QueryGraphicsHostInfoReportsSDLsDriverAndDisplays)
{
    try
    {
        const Window window(
            WindowSpecification{.Title = "GraphicsHostInfoTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        const GraphicsHostInfo info = window.queryGraphicsHostInfo();

        // The OpenGL strings captured when the window made its context come along.
        EXPECT_FALSE(info.glVersion.empty());
        EXPECT_FALSE(info.glRenderer.empty());

        const char* driver = SDL_GetCurrentVideoDriver();
        ASSERT_NE(driver, nullptr) << "No current SDL video driver with a window open";
        EXPECT_EQ(info.videoDriver, driver);

        int count = 0;
        const DisplayIds ids(SDL_GetDisplays(&count));
        ASSERT_NE(ids, nullptr) << "SDL_GetDisplays failed with a window open: " << SDL_GetError();
        ASSERT_GT(count, 0);
        ASSERT_EQ(info.displays.size(), static_cast<std::size_t>(count));

        const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
        ASSERT_NE(primary, 0U) << "SDL_GetPrimaryDisplay failed: " << SDL_GetError();
        std::size_t primaries = 0;
        const std::span<const SDL_DisplayID> sdlDisplays(ids.get(), static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < sdlDisplays.size(); ++i)
        {
            const SDL_DisplayID id = sdlDisplays[i];
            const DisplayInfo& display = info.displays[i];
            SCOPED_TRACE(::testing::Message() << "display " << i << " (SDL id " << id << ", \"" << display.name << "\")");

            // The same display in the same place: DisplayInfo has no ID, so its name stands in.
            const char* name = SDL_GetDisplayName(id);
            EXPECT_EQ(display.name, name != nullptr ? name : "");

            // x/y/width/height are SDL's display bounds, in window coordinates.
            SDL_Rect bounds{};
            if (SDL_GetDisplayBounds(id, &bounds))
            {
                EXPECT_EQ(display.x, bounds.x);
                EXPECT_EQ(display.y, bounds.y);
                EXPECT_EQ(display.width, bounds.w);
                EXPECT_EQ(display.height, bounds.h);
                EXPECT_GT(display.width, 0);
                EXPECT_GT(display.height, 0);
            }
            else
            {
                ADD_FAILURE() << "SDL_GetDisplayBounds failed for an enumerated display: " << SDL_GetError();
            }

            // pixelWidth/pixelHeight are the current mode's size converted to pixels by its pixel density
            // (SDL treats a density of 0 as 1), and refreshHz is the rate that mode advertises.
            if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id); mode != nullptr)
            {
                SCOPED_TRACE(::testing::Message() << "mode " << mode->w << "x" << mode->h << " pixel_density=" << mode->pixel_density << " "
                                                  << describeRefresh(*mode));
                const float density = mode->pixel_density > 0.0F ? mode->pixel_density : 1.0F;
                EXPECT_EQ(display.pixelWidth, static_cast<int>(std::lround(static_cast<float>(mode->w) * density)));
                EXPECT_EQ(display.pixelHeight, static_cast<int>(std::lround(static_cast<float>(mode->h) * density)));
                EXPECT_GT(display.pixelWidth, 0);
                EXPECT_GT(display.pixelHeight, 0);
                // The same expression in the same order, so the doubles are identical.
                EXPECT_DOUBLE_EQ(display.refreshHz, advertisedRefreshHz(*mode));
                EXPECT_TRUE(std::isfinite(display.refreshHz));
                EXPECT_GE(display.refreshHz, 0.0);
            }
            else
            {
                // No mode: the pixel size and rate stay "unknown".
                EXPECT_EQ(display.pixelWidth, 0);
                EXPECT_EQ(display.pixelHeight, 0);
                EXPECT_DOUBLE_EQ(display.refreshHz, 0.0);
            }

            EXPECT_FLOAT_EQ(display.contentScale, SDL_GetDisplayContentScale(id));
            EXPECT_TRUE(std::isfinite(display.contentScale));
            EXPECT_GT(display.contentScale, 0.0F);

            EXPECT_EQ(display.hdrEnabled,
                      SDL_GetBooleanProperty(SDL_GetDisplayProperties(id), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false));

            EXPECT_EQ(display.primary, id == primary);
            if (display.primary)
            {
                ++primaries;
            }
        }
        // SDL's primary display is one of those it enumerates, and the only one flagged.
        EXPECT_EQ(primaries, 1U);
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << e.what();
    }
}

// getDisplayRefreshRate() is the rate the window's display's current mode advertises (#2966): the exact
// numerator/denominator when SDL supplies them, else SDL's float rate, and 0 (unknown) when SDL has no
// mode. It agrees with what queryGraphicsHostInfo() reports for the same display. No rate is assumed.
TEST_F(WindowTest, GetDisplayRefreshRateIsTheRateTheWindowsDisplayModeAdvertises)
{
    try
    {
        const Window window(
            WindowSpecification{.Title = "DisplayRefreshRateTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        const double rate = window.getDisplayRefreshRate();
        EXPECT_TRUE(std::isfinite(rate));
        EXPECT_GE(rate, 0.0);

        const SDL_DisplayID displayId = SDL_GetDisplayForWindow(window.getHandle());
        ASSERT_NE(displayId, 0U) << "The window is on no display: " << SDL_GetError();

        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displayId);
        if (mode == nullptr)
        {
            EXPECT_DOUBLE_EQ(rate, 0.0) << "SDL has no current mode for the window's display: " << SDL_GetError();
        }
        else
        {
            SCOPED_TRACE(describeRefresh(*mode));
            if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
            {
                // The rational rate, not the float SDL rounds it to. The production code divides the same
                // two values as doubles, so the results are identical.
                EXPECT_DOUBLE_EQ(rate,
                                 static_cast<double>(mode->refresh_rate_numerator) / static_cast<double>(mode->refresh_rate_denominator));
            }
            else
            {
                EXPECT_DOUBLE_EQ(rate, static_cast<double>(mode->refresh_rate));
            }
        }

        // The System Information page's rate for this display is the same number.
        int count = 0;
        const DisplayIds ids(SDL_GetDisplays(&count));
        ASSERT_NE(ids, nullptr) << "SDL_GetDisplays failed with a window open: " << SDL_GetError();
        const std::span<const SDL_DisplayID> sdlDisplays(ids.get(), static_cast<std::size_t>(std::max(count, 0)));
        const auto found = std::ranges::find(sdlDisplays, displayId);
        ASSERT_NE(found, sdlDisplays.end()) << "The window's display " << displayId << " is not among SDL's displays";
        const auto index = static_cast<std::size_t>(std::distance(sdlDisplays.begin(), found));
        const GraphicsHostInfo info = window.queryGraphicsHostInfo();
        ASSERT_LT(index, info.displays.size());
        EXPECT_DOUBLE_EQ(info.displays[index].refreshHz, rate);
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver() && !TestSupport::displayRequired())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << e.what();
    }
}

} // namespace
} // namespace Core
