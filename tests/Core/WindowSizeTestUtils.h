#pragma once

#include "Core/Window.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <utility>

namespace TestSupport
{

// On a real display a window's size is asynchronous (#1309): SDL only updates what
// getWidth()/getHeight() report when it processes the window manager's configure event, which can
// come late under load or under a compositing window manager. WSLg's (Weston's XWM via XWayland)
// has been seen to leave a new window at 1x1, the size before it was mapped, for seconds (#2650,
// #2706). Pumps events until @p window reports @p width x @p height or a deadline passes, and
// returns whether it got there. Callers skip on a desktop when it didn't, and keep asserting where
// TASKSMACK_REQUIRE_DISPLAY=1 (CI, Xvfb with no window manager), so a size that is really wrong
// still fails there.
inline bool waitForWindowSize(const Core::Window& window, int width, int height)
{
    constexpr auto RESIZE_TIMEOUT = std::chrono::seconds{5};
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds{10};
    const auto deadline = std::chrono::steady_clock::now() + RESIZE_TIMEOUT;
    while (window.getSize() != std::pair{width, height} && std::chrono::steady_clock::now() < deadline)
    {
        SDL_PumpEvents();
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return window.getSize() == std::pair{width, height};
}

// The skip message for a size the window manager didn't apply within waitForWindowSize()'s wait.
inline std::string sizeNotAppliedMessage(const Core::Window& window, int width, int height)
{
    return std::format(
        "Window manager didn't apply {}x{} within the wait (got {}x{})", width, height, window.getWidth(), window.getHeight());
}

} // namespace TestSupport
