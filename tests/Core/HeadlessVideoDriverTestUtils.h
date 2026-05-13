#pragma once

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

    const bool videoSet = (setenv("SDL_VIDEODRIVER", "offscreen", 1) == 0);
    if (videoSet)
    {
        [[maybe_unused]] const int audioSetResult = setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)

    return videoSet;
#endif
}

} // namespace TestSupport
