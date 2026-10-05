#pragma once

// How Application::run() routes the SDL window events it acts on, extracted as pure functions so the
// routing can be unit-tested without a live window -- see CONTRIBUTING.md's "extract the pure
// decision logic into a small header" pattern (also used by Core/FramePacing.h).

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <utility>

namespace Core::WindowEventRouting
{

/// What run() does with an SDL event, beyond handing it to every layer's onSDLEvent().
enum class Action : std::uint8_t
{
    /// Nothing beyond the layers' onSDLEvent().
    None,
    /// A request to close the window (SDL_EVENT_WINDOW_CLOSE_REQUESTED: Alt+F4, the OS close button).
    /// Raised as a WindowCloseEvent, which a layer may veto.
    VetoableClose,
    /// SDL_EVENT_QUIT: the process is being told to end (SIGINT, SIGTERM, an OS logout or shutdown).
    /// Not raised as a WindowCloseEvent and never vetoable: a confirm-on-close dialog must not be able
    /// to block the OS terminating the app (#1150). With SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE disabled
    /// (Application does this), closing the window no longer also produces one, so a single close
    /// request raises exactly one WindowCloseEvent.
    Quit,
    /// SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: the framebuffer size changed; data1/data2 hold it.
    PixelSizeChanged,
    /// SDL_EVENT_WINDOW_RESIZED: the logical size changed; the pixel size is queried.
    Resized,
    /// SDL_EVENT_WINDOW_MOVED: the window moved, which keeps an interactive move responsive.
    Moved,
    /// SDL_EVENT_WINDOW_EXPOSED: part of the window needs repainting. A plain redraw request, not a
    /// resize interaction (#1154) -- SDL sends one per Windows WM_PAINT, per damaged X11 rectangle,
    /// and on Wayland configure and occlusion changes. See exposeChangesSize() for the one case where
    /// it is treated as a resize.
    Exposed,
    /// The window moved to another display (SDL_EVENT_WINDOW_DISPLAY_CHANGED) or a display's mode
    /// changed (SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED): re-read the refresh rate frames are paced
    /// against (#1126).
    DisplayChanged,
    /// SDL_EVENT_WINDOW_MAXIMIZED: the OS maximized the window (Win+Up, snap to the top edge,
    /// ShowWindow(SW_MAXIMIZE), or SDL_MaximizeWindow()). For the borderless window on a
    /// client-side-maximize backend the OS sizes it from the primary screen rather than the current
    /// monitor's work area, so run() hands it to Window::adoptSystemMaximize() (#1208).
    SystemMaximized,
};

/// Classify an SDL event type.
[[nodiscard]] constexpr auto classify(std::uint32_t sdlEventType) noexcept -> Action
{
    switch (sdlEventType)
    {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return Action::VetoableClose;
    case SDL_EVENT_QUIT:
        return Action::Quit;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        return Action::PixelSizeChanged;
    case SDL_EVENT_WINDOW_RESIZED:
        return Action::Resized;
    case SDL_EVENT_WINDOW_MOVED:
        return Action::Moved;
    case SDL_EVENT_WINDOW_EXPOSED:
        return Action::Exposed;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        return Action::DisplayChanged;
    case SDL_EVENT_WINDOW_MAXIMIZED:
        return Action::SystemMaximized;
    default:
        return Action::None;
    }
}

/// Whether an SDL_EVENT_WINDOW_EXPOSED should be handled as a resize: only when the framebuffer is a
/// real size that differs from the last size run() raised a WindowResizedEvent for. This keeps the
/// fallback for Windows border drags, which can surface EXPOSED before (or instead of)
/// PIXEL_SIZE_CHANGED, without treating every repaint as an interaction (#1154).
///
/// @param lastPixelSize     {width, height} of the last WindowResizedEvent (or the initial size).
/// @param currentPixelSize  {width, height} of the framebuffer now.
[[nodiscard]] constexpr bool exposeChangesSize(std::pair<int, int> lastPixelSize, std::pair<int, int> currentPixelSize) noexcept
{
    return currentPixelSize.first > 0 && currentPixelSize.second > 0 && currentPixelSize != lastPixelSize;
}

/// Whether a framebuffer size from a resize event is a real size. A minimised window reports 0x0;
/// that is neither a WindowResizedEvent nor a resize interaction, so it must not start the
/// interaction frame pacing (FramePacing::computeIsInteracting counts resize events).
[[nodiscard]] constexpr bool isRealPixelSize(std::pair<int, int> pixelSize) noexcept
{
    return pixelSize.first > 0 && pixelSize.second > 0;
}

} // namespace Core::WindowEventRouting
