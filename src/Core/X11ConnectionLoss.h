#pragma once

// What TaskSmack does when its X11 (or XWayland) connection breaks (#2657): the user logged out, the X
// server or WSLg restarted. Xlib's default I/O error handler calls exit(), which runs every static
// destructor on the main thread while the sampler threads still use that storage (spdlog's registry,
// caches): data races, use-after-free, and once a crash (#2633). The handler installed here logs the
// loss and ends the process with std::quick_exit(), which runs no static destructors.

namespace Core::X11ConnectionLoss
{

/// The exit status when the X connection breaks, as Xlib's default handler's exit(1).
inline constexpr int EXIT_STATUS = 1;

/// Install the handler, process-wide, through the libX11 SDL's X11 video driver loaded (dlopen()
/// RTLD_NOLOAD: never a second copy). Call after SDL_Init(SDL_INIT_VIDEO) on the X11 or XWayland
/// backend. False when libX11 isn't loaded or has no XSetIOErrorHandler, and on other platforms.
bool install() noexcept;

} // namespace Core::X11ConnectionLoss
