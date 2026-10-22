#include "X11ConnectionLoss.h"

#if defined(__linux__)
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>

#include <dlfcn.h>

// Xlib's Display, declared at global scope as <X11/Xlib.h> does, so the handler's type matches Xlib's
// XIOErrorHandler exactly (see X11WindowManager.cpp).
struct _XDisplay; // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) -- Xlib's own name
#endif

namespace Core::X11ConnectionLoss
{

#if defined(__linux__)

namespace
{

using XIOErrorHandler = int (*)(::_XDisplay* display);
using XSetIOErrorHandlerFn = XIOErrorHandler (*)(XIOErrorHandler handler);

/// Xlib calls this, on the thread whose Xlib call found the connection broken (the main thread), and
/// exits itself if it returns: so it never returns.
[[noreturn]] int onConnectionLost(::_XDisplay* /*display*/)
{
    // stderr as well as the log, as Xlib's own handler reports it: the log may be off or elsewhere
    std::fputs("TaskSmack: lost the connection to the X server; exiting\n", stderr);
    try
    {
        spdlog::critical("Lost the connection to the X server; exiting");
        spdlog::default_logger_raw()->flush();
    }
    catch (...) // NOLINT(bugprone-empty-catch) -- logging is best effort; exiting is what matters
    {}
    // No exit(): it would run the static destructors under the sampler threads (#2657, #2633).
    std::quick_exit(EXIT_STATUS);
}

} // namespace

bool install() noexcept
{
    // RTLD_NOLOAD: only the copy SDL loaded; never load a second one.
    void* library = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (library == nullptr)
    {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- dlsym returns void* by POSIX definition
    const auto setIOErrorHandler = reinterpret_cast<XSetIOErrorHandlerFn>(dlsym(library, "XSetIOErrorHandler"));
    if (setIOErrorHandler != nullptr)
    {
        setIOErrorHandler(&onConnectionLost);
    }
    dlclose(library); // Drops only the reference RTLD_NOLOAD added; SDL keeps libX11 loaded
    return setIOErrorHandler != nullptr;
}

#else

bool install() noexcept
{
    return false;
}

#endif

} // namespace Core::X11ConnectionLoss
