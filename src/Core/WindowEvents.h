#pragma once

#include "Event.h"

namespace Core
{

/// Window close event - used for clean shutdown coordination
class WindowCloseEvent : public Event
{
  public:
    WindowCloseEvent() = default;
    EVENT_CLASS_TYPE(WindowClose)
};

} // namespace Core
