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
        // A driver is already configured. Verify that it can both initialise the
        // SDL video subsystem and load an OpenGL library. Drivers such as 'dummy'
        // pass SDL_Init(SDL_INIT_VIDEO) but cannot create GL contexts; returning
        // true for such a driver would cause Window construction to hit FAIL()
        // instead of GTEST_SKIP(). Clear the variable and fall through to the
        // offscreen driver if either check fails.
        bool glCapable = false;
        if (SDL_Init(SDL_INIT_VIDEO))
        {
            if (SDL_GL_LoadLibrary(nullptr))
            {
                SDL_GL_UnloadLibrary();
                glCapable = true;
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
