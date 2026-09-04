/// @file test_VideoBackend.cpp
/// @brief Tests for Core::VideoBackend backend detection and capability queries.
///
/// Two kinds of coverage here:
/// - ClassifyBackendTest table-tests VideoBackend::classifyBackend() directly. It's a pure
///   function of (driver name, WAYLAND_DISPLAY-is-set) with no SDL/environment calls of its
///   own, so every backend/edge case can be exercised deterministically regardless of what
///   video driver is actually active in the test process.
/// - VideoBackendTest exercises the real initialize()/isWayland()/etc. path against whichever
///   real or headless SDL driver is active. VideoBackend caches its detection result in
///   process-lifetime static state (initialize() is a no-op after the first successful call),
///   and CI runners have no real Wayland/X11 display, so these tests can't assert a specific
///   backend was chosen; instead they assert the invariants that must hold for whichever
///   backend SDL actually reports.

#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/VideoBackend.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

namespace Core
{
namespace
{

TEST(ClassifyBackendTest, EmptyDriverNameIsUnknown)
{
    EXPECT_EQ(VideoBackend::classifyBackend("", false), VideoBackend::Backend::Unknown);
    EXPECT_EQ(VideoBackend::classifyBackend("", true), VideoBackend::Backend::Unknown);
}

TEST(ClassifyBackendTest, UnrecognizedDriverNameIsUnknown)
{
    EXPECT_EQ(VideoBackend::classifyBackend("dummy", false), VideoBackend::Backend::Unknown);
    EXPECT_EQ(VideoBackend::classifyBackend("offscreen", false), VideoBackend::Backend::Unknown);
    EXPECT_EQ(VideoBackend::classifyBackend("cocoa", false), VideoBackend::Backend::Unknown);
}

TEST(ClassifyBackendTest, WaylandDriverIsAlwaysNativeWaylandRegardlessOfEnv)
{
    // Regression case for the bug fixed in this PR: a "wayland" driver name must never be
    // reclassified as XWayland just because WAYLAND_DISPLAY (or DISPLAY) also happens to be set.
    EXPECT_EQ(VideoBackend::classifyBackend("wayland", false), VideoBackend::Backend::Wayland);
    EXPECT_EQ(VideoBackend::classifyBackend("wayland", true), VideoBackend::Backend::Wayland);
}

TEST(ClassifyBackendTest, X11DriverWithoutWaylandDisplayIsPlainX11)
{
    EXPECT_EQ(VideoBackend::classifyBackend("x11", false), VideoBackend::Backend::X11);
}

TEST(ClassifyBackendTest, X11DriverWithWaylandDisplayIsXWaylandFallback)
{
    EXPECT_EQ(VideoBackend::classifyBackend("x11", true), VideoBackend::Backend::XWaylandFallback);
}

TEST(ClassifyBackendTest, WindowsDriverClassifiesPerPlatform)
{
#ifdef _WIN32
    EXPECT_EQ(VideoBackend::classifyBackend("windows", false), VideoBackend::Backend::Windows);
#else
    // The "windows" driver name can never occur on a non-Windows build, and detectBackend()'s
    // #ifdef _WIN32 guard compiles the check out entirely -- classifyBackend() must match that.
    EXPECT_EQ(VideoBackend::classifyBackend("windows", false), VideoBackend::Backend::Unknown);
#endif
}

// ========== ShouldUseBorderlessTitleBar ==========
// See #745/#750 review: pure policy for whether the window uses the custom title bar (true) or
// native OS/compositor decorations (false). Table-tested directly, independent of whichever real
// video driver is active in the test process.

TEST(ShouldUseBorderlessTitleBarTest, DefaultOff_AlwaysBorderlessRegardlessOfBackend)
{
    EXPECT_TRUE(VideoBackend::shouldUseBorderlessTitleBar(/*forceNativeDecorationsOnWayland=*/false, /*isWayland=*/true));
    EXPECT_TRUE(VideoBackend::shouldUseBorderlessTitleBar(/*forceNativeDecorationsOnWayland=*/false, /*isWayland=*/false));
}

TEST(ShouldUseBorderlessTitleBarTest, OptIn_OnNativeWayland_DisablesBorderless)
{
    EXPECT_FALSE(VideoBackend::shouldUseBorderlessTitleBar(/*forceNativeDecorationsOnWayland=*/true, /*isWayland=*/true));
}

TEST(ShouldUseBorderlessTitleBarTest, OptIn_OnNonWayland_HasNoEffect)
{
    // X11, XWayland, and Windows: the opt-in setting is native-Wayland-only, so it must not
    // disable the custom title bar on any other backend.
    EXPECT_TRUE(VideoBackend::shouldUseBorderlessTitleBar(/*forceNativeDecorationsOnWayland=*/true, /*isWayland=*/false));
}

bool ensureVideoDriverInitialized()
{
    if (SDL_Init(SDL_INIT_VIDEO))
    {
        return true;
    }
    return TestSupport::tryEnableOffscreenVideoDriver() && SDL_Init(SDL_INIT_VIDEO);
}

// VideoBackend's own static state persists for the life of the test process (initialize()
// only needs to run once), but SDL_Init/SDL_Quit must still be balanced per test: other Core
// test files (e.g. ApplicationTest.FailedConstructionClearsSingletonAndAllowsRetry) rely on
// the video subsystem being uninitialized so their own SDL_Init() call is the one that applies
// a hint like SDL_HINT_VIDEO_DRIVER. SDL ref-counts subsystem init, so a leaked SDL_Init() here
// would make a later SDL_Init() elsewhere just increment the refcount and silently ignore
// whatever hint that test set. driverName() stays valid across SDL_Quit() regardless -- SDL's
// driver names are static string literals, not allocated per Init/Quit cycle.
class VideoBackendTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!ensureVideoDriverInitialized())
        {
            GTEST_SKIP() << "No SDL video driver available (headless environment)";
        }
        m_SdlInitialized = true;
        VideoBackend::initialize();
    }

    void TearDown() override
    {
        if (m_SdlInitialized)
        {
            SDL_Quit();
        }
    }

  private:
    bool m_SdlInitialized = false;
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

TEST_F(VideoBackendTest, ResetForTestingAllowsReinitialization)
{
    // Without resetForTesting(), initialize() is a no-op after the first successful call --
    // a latent trap for any future test that forces a specific driver and expects a second
    // initialize() in the same process to actually re-detect it. Capture the live state,
    // reset, verify it's cleared, then re-initialize and confirm it matches again (also
    // restores global state for any VideoBackendTest that runs after this one).
    const std::string_view driverBefore = VideoBackend::driverName();
    const bool waylandBefore = VideoBackend::isWayland();

    VideoBackend::resetForTesting();
    EXPECT_TRUE(VideoBackend::driverName().empty());
    EXPECT_FALSE(VideoBackend::isWayland());
    EXPECT_FALSE(VideoBackend::isX11());
    EXPECT_FALSE(VideoBackend::isXWaylandFallback());
    EXPECT_FALSE(VideoBackend::isWindows());

    VideoBackend::initialize();
    EXPECT_EQ(VideoBackend::driverName(), driverBefore);
    EXPECT_EQ(VideoBackend::isWayland(), waylandBefore);
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

TEST_F(VideoBackendTest, SupportsGlobalMouseStateIsFalseOnlyOnWayland)
{
    EXPECT_EQ(VideoBackend::supportsGlobalMouseState(), !VideoBackend::isWayland());
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
