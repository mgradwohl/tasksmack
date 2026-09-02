/// @file test_VideoBackend.cpp
/// @brief Tests for Core::VideoBackend backend detection and capability queries.
///
/// VideoBackend caches its detection result in process-lifetime static state (initialize()
/// is a no-op after the first successful call), and detectBackend() calls the real
/// SDL_GetCurrentVideoDriver() with no seam to inject a fake driver name. CI runners have no
/// real Wayland/X11 display, so these tests can't assert a specific backend was chosen; instead
/// they assert the invariants that must hold for whichever backend SDL actually reports.

#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/VideoBackend.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

namespace Core
{
namespace
{

bool ensureVideoDriverInitialized()
{
    if (SDL_Init(SDL_INIT_VIDEO))
    {
        return true;
    }
    return TestSupport::tryEnableOffscreenVideoDriver() && SDL_Init(SDL_INIT_VIDEO);
}

// VideoBackend's static state persists for the life of the test process, so initialize()
// only needs to run once; SDL is intentionally never quit here (see file comment).
class VideoBackendTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!ensureVideoDriverInitialized())
        {
            GTEST_SKIP() << "No SDL video driver available (headless environment)";
        }
        VideoBackend::initialize();
    }
};

TEST_F(VideoBackendTest, InitializeIsIdempotent)
{
    const std::string_view driverBefore = VideoBackend::driverName();
    const bool waylandBefore = VideoBackend::isWayland();

    VideoBackend::initialize();

    EXPECT_EQ(VideoBackend::driverName(), driverBefore);
    EXPECT_EQ(VideoBackend::isWayland(), waylandBefore);
}

TEST_F(VideoBackendTest, DriverNameIsNotEmptyAfterInitialize)
{
    EXPECT_FALSE(VideoBackend::driverName().empty());
}

TEST_F(VideoBackendTest, BackendFlagsAreMutuallyExclusive)
{
    const int trueFlags = static_cast<int>(VideoBackend::isWayland()) + static_cast<int>(VideoBackend::isX11()) +
                          static_cast<int>(VideoBackend::isXWaylandFallback()) + static_cast<int>(VideoBackend::isWindows());
    EXPECT_LE(trueFlags, 1) << "At most one backend flag should be true for driver '" << VideoBackend::driverName() << "'";
}

// Regression test for the bug fixed in this PR: detectBackend() must never classify a
// driver that SDL itself reports as "wayland" (native Wayland) as XWayland fallback.
TEST_F(VideoBackendTest, NativeWaylandIsNeverClassifiedAsXWaylandFallback)
{
    if (VideoBackend::driverName() == "wayland")
    {
        EXPECT_TRUE(VideoBackend::isWayland());
        EXPECT_FALSE(VideoBackend::isXWaylandFallback());
    }
    else
    {
        GTEST_SKIP() << "Not running under the native Wayland SDL driver";
    }
}

TEST_F(VideoBackendTest, SupportsClientSideMaximizeIsFalseOnlyOnWayland)
{
    EXPECT_EQ(VideoBackend::supportsClientSideMaximize(), !VideoBackend::isWayland());
}

TEST_F(VideoBackendTest, SupportsGlobalMouseStateIsAlwaysTrue)
{
    EXPECT_TRUE(VideoBackend::supportsGlobalMouseState());
}

TEST_F(VideoBackendTest, IsWindowsIsFalseOnNonWindowsBuilds)
{
#ifndef _WIN32
    EXPECT_FALSE(VideoBackend::isWindows());
#else
    GTEST_SKIP() << "Only meaningful on non-Windows builds";
#endif
}

} // namespace
} // namespace Core
