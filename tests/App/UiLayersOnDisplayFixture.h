#pragma once

// The display checks, helpers and fixture test_UiLayersOnDisplay*.cpp share (#3850: the one 5,400-line file split by
// feature). In a named namespace, not an anonymous one: gtest requires every test of a suite to use the same fixture
// class, and an anonymous namespace would give each test file its own.

#include "App/AboutDialog.h"
#include "App/AboutLayer.h"
#include "App/ElevationNoticeLayer.h"
#include "App/HelpLayer.h"
#include "App/Panels/ProcessDetailsPanel.h"
#include "App/ProcessColumnConfig.h"
#include "App/SettingsDialog.h"
#include "App/SettingsLayer.h"
#include "App/ShellLayer.h"
#include "App/TitleBarButtons.h"
#include "App/TitleBarGeometry.h"
#include "App/TitleBarLayer.h"
#include "App/UserConfig.h"
#include "App/WindowInteraction.h"
#include "Core/AnimationRequest.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/ConfigDirOverride.h"
#include "Core/Event.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Core/VideoBackend.h"
#include "Core/Window.h"
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"
#include "Domain/SamplingConfig.h"
#include "ScratchConfigDir.h"
#include "UI/DpiScale.h"
#include "UI/FontPointSize.h"
#include "UI/IconTextureCache.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RenderMetrics.h"
#include "UI/Theme.h"
#include "UI/UILayer.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>
#include <glad/gl.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(): the bar and its buttons
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h> // DwmSetWindowAttribute(DWMWA_CLOAK) (#3152)
// clang-format on

#include <cstring> // sendDisplayState()
#include <span>    // sendDisplayState()
#else
#include <unistd.h> // getpid(): the hidden-shell test selects this process (#3156)
#endif

namespace UiLayersOnDisplayTesting
{

/// A display with a GL 3.3 core context: the real one when DISPLAY or WAYLAND_DISPLAY is set and
/// usable, else SDL's offscreen driver if Mesa EGL can back it.
inline bool hasDisplay()
{
#ifdef _WIN32
    // A GL 3.3 core context on the desktop, CI included (#2134).
    const bool available = TestSupport::detectWindowsDisplay();
#else
    // NOLINTBEGIN(concurrency-mt-unsafe) - read-only environment access during single-threaded test setup
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe)
    const bool named = (display != nullptr && *display != '\0') || (waylandDisplay != nullptr && *waylandDisplay != '\0');
    const bool available =
        (named && TestSupport::probeGLCapability()) || (TestSupport::tryEnableOffscreenVideoDriver() && TestSupport::probeGLCapability());
#endif
    return TestSupport::enforceDisplayRequirement(available);
}

/// Whether SDL runs these tests on native Wayland, as it chooses a driver: SDL_VIDEODRIVER when set, else
/// Wayland whenever WAYLAND_DISPLAY names one (SDL 3 prefers it to X11). There the compositor moves,
/// resizes and maximizes the window -- the hit test hands it the title bar and the borders (#744) -- and
/// never tells TaskSmack where the window is, so injected drags can't move it (#2724). Known before the
/// app starts, so a test can skip without waiting out its deadlines.
inline bool onNativeWayland()
{
#ifdef _WIN32
    return false;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe) - read-only environment access during single-threaded test setup
    const char* driver = std::getenv("SDL_VIDEODRIVER");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe)
    if (driver != nullptr && *driver != '\0')
    {
        return std::string_view(driver).starts_with("wayland");
    }
    return waylandDisplay != nullptr && *waylandDisplay != '\0';
#endif
}

/// Whether the native Wayland compositor these tests run under maximizes when asked: tools/wayland-matrix.sh
/// sets TASKSMACK_COMPOSITOR_MAXIMIZES=1 under labwc, which does; sway, a tiling compositor, never does.
inline bool compositorMaximizes()
{
#ifdef _WIN32
    return false; // no Wayland there
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - read-only environment access during single-threaded test setup
    const char* maximizes = std::getenv("TASKSMACK_COMPOSITOR_MAXIMIZES");
    return maximizes != nullptr && std::string_view(maximizes) == "1";
#endif
}

/// Why a client-side drag test doesn't run on native Wayland.
inline constexpr const char* COMPOSITOR_DRAGS =
    "native Wayland: the compositor runs title-bar drags, border resizes and maximize (#744), so injected "
    "pointer events can't move the window; TheHitTestCallbackAnswersForTheRealWindow covers the hand-off (#2724)";

/// For a driver that presses keys through ImGuiIO: a focus loss no longer drops its presses (#2268).
/// Under Xvfb there's no window manager, so SDL gives each window it shows the X input focus, and a test
/// app that ctest -j opens alongside this one takes it from ours. ImGui clears every key after the frame
/// that takes a focus loss in, so an F1 pressed on that frame was never seen and Help never opened. The
/// presses here come from the driver, not the keyboard, so they're the same whichever window has focus.
inline void keepKeyPressesThroughFocusLoss()
{
    ImGui::GetIO().ConfigDebugIgnoreFocusLoss = true;
}

/// Each test on its own, empty config directory (#2932): one doesn't open on what another, or the same
/// test's last --gtest_repeat iteration, saved on close.
class UiLayersOnDisplayTest : public TestSupport::ScratchConfigDirTest
{};

} // namespace UiLayersOnDisplayTesting
