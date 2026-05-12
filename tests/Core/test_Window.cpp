#include "Core/Window.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>

namespace
{

bool hasDisplay()
{
#ifdef _WIN32
    return true;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    return (display != nullptr && display[0] != '\0') || (waylandDisplay != nullptr && waylandDisplay[0] != '\0');
#endif
}

} // namespace

namespace Core
{
namespace
{

TEST(WindowTest, CloseRequestLifecycle)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

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

TEST(WindowTest, SetSizeClampsToExpectedBounds)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

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

TEST(WindowTest, SetAndGetPositionRoundTrip)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    try
    {
        Window window(WindowSpecification{.Title = "WindowPositionTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});
        window.setPosition(40, 60);
        const auto [x, y] = window.getPosition();
        EXPECT_EQ(x, 40);
        EXPECT_EQ(y, 60);
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Window creation failed: " << e.what();
    }
}

TEST(WindowTest, WindowStateControlMethodsDoNotThrow)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

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
