#pragma once

#include <SDL3/SDL.h>

#include <cstdlib>

namespace TestSupport
{

[[maybe_unused]] inline bool tryEnableOffscreenVideoDriver()
{
#ifdef _WIN32
    return false;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay) - process env setup during single-threaded test startup
    const char* videoDriver = std::getenv("SDL_VIDEODRIVER");
    if (videoDriver != nullptr && videoDriver[0] != '\0')
    {
        return true;
    }

    if (setenv("SDL_VIDEODRIVER", "offscreen", 1) != 0)
    {
        return false;
    }
    [[maybe_unused]] const int audioSetResult = setenv("SDL_AUDIODRIVER", "dummy", 1);
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)

    // Verify the offscreen driver can actually initialize the SDL video subsystem before
    // returning true. Application construction registers a thread-local singleton before
    // creating the window; if SDL initialization subsequently fails, the singleton is left
    // dangling and later tests may misfire.
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        // The offscreen driver is not functional; clear the env var so tests skip cleanly.
        // NOLINTBEGIN(concurrency-mt-unsafe)
        unsetenv("SDL_VIDEODRIVER");
        // NOLINTEND(concurrency-mt-unsafe)
        return false;
    }
    SDL_Quit();
    return true;
#endif
}

} // namespace TestSupport
