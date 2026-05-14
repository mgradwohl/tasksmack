#pragma once

#ifndef _WIN32
#include <SDL3/SDL.h>
#endif

#include <cstdlib>
#include <string_view>

namespace TestSupport
{

#ifndef _WIN32
// Returns true if SDL can initialize video AND create an OpenGL 3.3 core context.
// Uses the same SDL_GL attributes as Core::Window so a display that only supports
// a default/legacy context is correctly rejected.
// Calls SDL_Init / SDL_Quit internally; do not call while SDL is already initialized.
[[maybe_unused]] inline bool probeGLCapability()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        return false;
    }
    // Match the GL attributes set by Core::Window before creating its context.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    bool glCapable = false;
    SDL_Window* testWin = SDL_CreateWindow("gl_probe", 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (testWin != nullptr)
    {
        SDL_GLContext ctx = SDL_GL_CreateContext(testWin);
        if (ctx != nullptr)
        {
            SDL_GL_DestroyContext(ctx);
            glCapable = true;
        }
        SDL_DestroyWindow(testWin);
    }
    SDL_Quit();
    return glCapable;
}
#endif

[[maybe_unused]] inline bool tryEnableOffscreenVideoDriver()
{
#ifdef _WIN32
    return false;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    // All env-var access below occurs during single-threaded test SetUp.

    const char* videoDriver = std::getenv("SDL_VIDEODRIVER");
    if (videoDriver != nullptr && videoDriver[0] != '\0')
    {
        // The offscreen driver is the intended headless fallback; trust it immediately.
        // Core::Window tests already handle GL context failure with GTEST_SKIP() via
        // isOffscreenVideoDriver(), so no GL capability probe is needed here.
        if (std::string_view(videoDriver) == "offscreen")
        {
            return true;
        }

        // For any other pre-configured driver (e.g. x11, wayland, dummy), verify GL
        // capability. Drivers such as 'dummy' pass SDL_Init(SDL_INIT_VIDEO) and can
        // load libGL, but cannot create GL contexts; returning true for such a driver
        // would cause Window construction to hit FAIL() instead of GTEST_SKIP().
        // Clear the variable and fall through to offscreen if the probe fails.
        if (probeGLCapability())
        {
            return true;
        }
        unsetenv("SDL_VIDEODRIVER");
    }

    if (setenv("SDL_VIDEODRIVER", "offscreen", 1) != 0)
    {
        return false;
    }
    [[maybe_unused]] const int audioSetResult = setenv("SDL_AUDIODRIVER", "dummy", 1);

    // SDL3 snapshots the process environment on first use; subsequent setenv() calls
    // may not be visible to SDL if it already cached an earlier snapshot. Set the
    // hint at OVERRIDE priority to guarantee SDL uses the offscreen driver regardless.
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

    // Verify the offscreen driver can actually initialize the SDL video subsystem.
    // The Application constructor's catch block cleans up the singleton on failure,
    // so an SDL initialization failure here is safe to detect and recover from.
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        // The offscreen driver is not functional; clear both the env var and the
        // SDL hint override so later SDL initializations are not forced to the
        // non-functional offscreen driver.
        unsetenv("SDL_VIDEODRIVER");
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        return false;
    }
    SDL_Quit();
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    return true;
#endif
}

} // namespace TestSupport
