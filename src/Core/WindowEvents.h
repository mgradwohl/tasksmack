#pragma once

#include "Event.h"

#include <format>

namespace Core
{

/// Window close event - raised by Application::run() when the OS or the user asks to close the
/// window (SDL_EVENT_QUIT, or SDL_EVENT_WINDOW_CLOSE_REQUESTED such as Alt+F4).
///
/// Contract: marking this event handled VETOES the close. If no layer handles it, run() calls
/// stop() and the app shuts down. A layer that only wants to observe the close (to flush state,
/// say) must return false from its handler; returning true for any other reason keeps the window
/// open. A layer that vetoes is responsible for closing later, e.g. by calling
/// Application::stop() once the user confirms.
///
/// Layers receive it top of the stack first, and the first handler to return true stops it, so
/// a veto also hides the event from the layers below. Process-wide shutdown work belongs in
/// Layer::onDetach(), which runs on every orderly shutdown; this event is only for deciding whether to
/// close.
///
/// No layer handles it today, so every close stops the app. Window::requestClose(), used by the
/// custom title bar's Close button and system menu, does not raise this event yet (#1077).
class WindowCloseEvent : public Event
{
  public:
    WindowCloseEvent() = default;
    EVENT_CLASS_TYPE(WindowClose)
};

/// Window resized event - fired when the framebuffer pixel size changes
/// width and height are in physical pixels (matches SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
class WindowResizedEvent : public Event
{
  public:
    WindowResizedEvent(int width, int height) : m_Width(width), m_Height(height)
    {}

    [[nodiscard]] auto getWidth() const noexcept -> int
    {
        return m_Width;
    }
    [[nodiscard]] auto getHeight() const noexcept -> int
    {
        return m_Height;
    }

    [[nodiscard]] auto toString() const -> std::string override
    {
        return std::format("WindowResized: {}x{}", m_Width, m_Height);
    }

    EVENT_CLASS_TYPE(WindowResized)

  private:
    int m_Width;
    int m_Height;
};

} // namespace Core
