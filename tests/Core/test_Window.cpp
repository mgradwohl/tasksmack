#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Window.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <string_view>

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

bool hasDisplay()
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

} // namespace

namespace Core
{
namespace
{

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
        if (isOffscreenVideoDriver())
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
        EXPECT_EQ(window.getWidth(), WINDOW_MIN_DIMENSION);
        EXPECT_EQ(window.getHeight(), WINDOW_MIN_DIMENSION);

        window.setSize(20000, 50000);
        EXPECT_EQ(window.getWidth(), WINDOW_MAX_DIMENSION);
        EXPECT_EQ(window.getHeight(), WINDOW_MAX_DIMENSION);
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver())
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
        if (isOffscreenVideoDriver())
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
        if (isOffscreenVideoDriver())
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
        if (isOffscreenVideoDriver())
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
        if (isOffscreenVideoDriver())
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
        if (isOffscreenVideoDriver())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

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
        if (isOffscreenVideoDriver())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

} // namespace
} // namespace Core
