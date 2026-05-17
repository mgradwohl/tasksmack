#pragma once

#include "Event.h"

#include <format>

namespace Core
{

/// Window close event - used for clean shutdown coordination
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
