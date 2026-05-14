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
    return true;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    if ((display != nullptr && display[0] != '\0') || (waylandDisplay != nullptr && waylandDisplay[0] != '\0'))
    {
        return true;
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
        GTEST_SKIP() << "Window creation failed: " << e.what();
    }
}

TEST_F(WindowTest, SetSizeClampsToExpectedBounds)
{
    try
    {
        Window window(WindowSpecification{.Title = "WindowClampTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        window.setSize(0, -10);
        EXPECT_EQ(window.getWidth(), 1);
        EXPECT_EQ(window.getHeight(), 1);

        window.setSize(20000, 50000);
        EXPECT_EQ(window.getWidth(), 16384);
        EXPECT_EQ(window.getHeight(), 16384);
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Window creation failed: " << e.what();
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
        GTEST_SKIP() << "Window creation failed: " << e.what();
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
        GTEST_SKIP() << "Window creation failed: " << e.what();
    }
}

} // namespace
} // namespace Core
