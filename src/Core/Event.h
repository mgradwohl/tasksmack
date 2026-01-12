#pragma once

#include <functional>
#include <string>

namespace Core
{

/// Event types supported by the application
enum class EventType : uint8_t
{
    None = 0,
    WindowClose,
    // Application events (for UI coordination)
    ProcessSelected,
    RefreshRateChanged,
    HistoryDurationChanged,
    ThemeChanged,
    FontSizeChanged,
    ActiveTabChanged,
    ProcessColumnsChanged,
    OpenSettings,
    OpenAbout
};

/// Macro to implement event type boilerplate
// NOLINTBEGIN(cppcoreguidelines-macro-usage) - Macros required for event type dispatch
#define EVENT_CLASS_TYPE(type)                                                                                                             \
    static auto getStaticType() -> EventType                                                                                               \
    {                                                                                                                                      \
        return EventType::type;                                                                                                            \
    }                                                                                                                                      \
    [[nodiscard]] auto getEventType() const -> EventType override                                                                          \
    {                                                                                                                                      \
        return getStaticType();                                                                                                            \
    }                                                                                                                                      \
    [[nodiscard]] auto getName() const -> const char* override                                                                             \
    {                                                                                                                                      \
        return #type;                                                                                                                      \
    }
// NOLINTEND(cppcoreguidelines-macro-usage)

/// Base class for all events
class Event
{
  public:
    Event() = default;
    virtual ~Event() = default;
    Event(const Event&) = delete;
    auto operator=(const Event&) -> Event& = delete;
    Event(Event&&) = default;
    auto operator=(Event&&) -> Event& = default;

    [[nodiscard]] bool isHandled() const
    {
        return m_Handled;
    }
    void setHandled(bool handled)
    {
        m_Handled = handled;
    }

    [[nodiscard]] virtual auto getEventType() const -> EventType = 0;
    [[nodiscard]] virtual auto getName() const -> const char* = 0;
    [[nodiscard]] virtual auto toString() const -> std::string
    {
        return getName();
    }

  private:
    bool m_Handled = false;
};

/// Type-safe event dispatcher
class EventDispatcher
{
  public:
    explicit EventDispatcher(Event& event) : m_Event(event)
    {
    }

    /// Dispatch event to handler if types match
    /// Returns true if the event was dispatched (regardless of whether it was handled)
    template<typename T> auto dispatch(const std::function<bool(T&)>& func) -> bool
    {
        if (m_Event.getEventType() == T::getStaticType() && !m_Event.isHandled())
        {
            m_Event.setHandled(func(static_cast<T&>(m_Event)));
            return true;
        }
        return false;
    }

  private:
    Event& m_Event; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) - Intentional reference for event dispatching
};

} // namespace Core
