#pragma once

#include <cstdlib>

#ifndef _WIN32
#include <SDL3/SDL.h>
#endif

namespace TestSupport
{

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
        // capability by actually creating a minimal GL window and context.
        // Drivers such as 'dummy' pass SDL_Init(SDL_INIT_VIDEO) and can load libGL,
        // but cannot create GL contexts; returning true for such a driver would cause
        // Window construction to hit FAIL() instead of GTEST_SKIP().
        // Clear the variable and fall through to offscreen if the probe fails.
        bool glCapable = false;
        if (SDL_Init(SDL_INIT_VIDEO))
        {
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
        }
        if (glCapable)
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

    // Verify the offscreen driver can actually initialize the SDL video subsystem
    // before returning true. Application construction registers a thread-local
    // singleton before creating the window; if SDL initialization subsequently
    // fails, the singleton is left dangling and later tests may misfire.
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        // The offscreen driver is not functional; clear the env var so tests skip cleanly.
        unsetenv("SDL_VIDEODRIVER");
        return false;
    }
    SDL_Quit();
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    return true;
#endif
}

} // namespace TestSupport
