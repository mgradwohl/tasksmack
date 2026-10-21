#include "Core/X11ConnectionLoss.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstdlib>

#include <dlfcn.h>
#include <sys/socket.h>

struct _XDisplay; // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) -- Xlib's own name

namespace
{

// Exit statuses the child uses when something before the connection loss fails, so the failure says
// which step, and the one a static destructor reports if exit() ran them.
constexpr int SDL_INIT_FAILED = 4;
constexpr int INSTALL_FAILED = 5;
constexpr int NO_X11_CONNECTION = 6;
constexpr int STATIC_DESTRUCTOR_RAN = 3;

/// Destroyed only by exit()'s static destructors: then it ends the process with its own status.
struct ExitIfDestroyed
{
    ExitIfDestroyed() = default;
    ExitIfDestroyed(const ExitIfDestroyed&) = delete;
    ExitIfDestroyed& operator=(const ExitIfDestroyed&) = delete;
    ExitIfDestroyed(ExitIfDestroyed&&) = delete;
    ExitIfDestroyed& operator=(ExitIfDestroyed&&) = delete;
    ~ExitIfDestroyed()
    {
        std::_Exit(STATIC_DESTRUCTOR_RAN);
    }
};

/// The X connection's socket: Xlib's XConnectionNumber(), from the libX11 SDL loaded.
[[nodiscard]] int connectionFd(::_XDisplay* display)
{
    void* library = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (library == nullptr)
    {
        return -1;
    }
    using XConnectionNumberFn = int (*)(::_XDisplay* display);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- dlsym returns void* by POSIX definition
    const auto connectionNumber = reinterpret_cast<XConnectionNumberFn>(dlsym(library, "XConnectionNumber"));
    const int fd = (connectionNumber != nullptr) ? connectionNumber(display) : -1;
    dlclose(library);
    return fd;
}

/// The child: X11 up, the handler installed, then its connection cut. Xlib's next read finds it broken.
[[noreturn]] void loseTheXConnection()
{
    static const ExitIfDestroyed sentinel; // Registered with exit()'s destructors, as the app's statics are
    static_cast<void>(sentinel);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::_Exit(SDL_INIT_FAILED);
    }
    if (!Core::X11ConnectionLoss::install())
    {
        std::_Exit(INSTALL_FAILED);
    }
    SDL_Window* window = SDL_CreateWindow("X11ConnectionLossTest", 64, 64, SDL_WINDOW_HIDDEN);
    auto* display = (window != nullptr) ? static_cast<::_XDisplay*>(SDL_GetPointerProperty(
                                              SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr))
                                        : nullptr;
    const int fd = (display != nullptr) ? connectionFd(display) : -1;
    if (fd < 0)
    {
        std::_Exit(NO_X11_CONNECTION);
    }
    ::shutdown(fd, SHUT_RDWR);
    while (true)
    {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
}

// #2657: a broken X connection ends the process through the handler -- its message, status 1 -- and
// without exit()'s static destructors, which race the sampler threads (#2633). Xlib's default handler
// would also exit 1, but with "X connection to ... broken", after running the sentinel's destructor.
TEST(X11ConnectionLossDeathTest, ABrokenConnectionExitsWithoutRunningStaticDestructors)
{
    if (std::getenv("DISPLAY") == nullptr) // NOLINT(concurrency-mt-unsafe) -- read before any thread
    {
        GTEST_SKIP() << "No X display (DISPLAY unset)";
    }
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        loseTheXConnection(), ::testing::ExitedWithCode(Core::X11ConnectionLoss::EXIT_STATUS), "lost the connection to the X server");
}

} // namespace
