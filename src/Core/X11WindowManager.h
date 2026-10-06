#pragma once

// What the X11 window manager (or XWayland's) says it supports, for Window::maximize() (#1339).

#include <algorithm>
#include <span>

struct SDL_Window;

namespace Core::X11WindowManager
{

/// Whether an EWMH _NET_SUPPORTED list (the root window property, as atoms) names both
/// _NET_WM_STATE_MAXIMIZED_VERT and _NET_WM_STATE_MAXIMIZED_HORZ: the window manager then maximizes
/// a window itself, to its own work area, when SDL_MaximizeWindow() asks. Atom 0 (None: the atom
/// was never interned, so nothing lists it) never matches.
[[nodiscard]] constexpr bool
supportedListHasMaximize(std::span<const unsigned long> supported, unsigned long maximizedVert, unsigned long maximizedHorz) noexcept
{
    return maximizedVert != 0 && maximizedHorz != 0 && std::ranges::find(supported, maximizedVert) != supported.end() &&
           std::ranges::find(supported, maximizedHorz) != supported.end();
}

/// Whether the window manager of `window`'s X11 display (X11 or XWayland) supports EWMH maximize,
/// read from the root window's _NET_SUPPORTED. False on any other video backend or platform, and
/// when it can't be read (no window manager, no EWMH, libX11 not loaded by SDL). One X server round
/// trip: call it per maximize, never per frame.
[[nodiscard]] bool supportsEwmhMaximize(SDL_Window* window) noexcept;

} // namespace Core::X11WindowManager
