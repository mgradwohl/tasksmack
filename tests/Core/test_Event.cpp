/// @file test_Event.cpp
/// @brief Tests for Core::Event, EventDispatcher

#include "Core/Event.h"

#include <gtest/gtest.h>
#include <string>

namespace Core
{
namespace
{

// ========== Concrete event types for testing ==========

class WindowCloseEvent : public Event
{
  public:
    EVENT_CLASS_TYPE(WindowClose) // NOLINT(cppcoreguidelines-macro-usage)
};

class ThemeChangedEvent : public Event
{
  public:
    EVENT_CLASS_TYPE(ThemeChanged) // NOLINT(cppcoreguidelines-macro-usage)
};

// ========== Event base class ==========

TEST(EventTest, DefaultNotHandled)
{
    WindowCloseEvent ev;
    EXPECT_FALSE(ev.isHandled());
}

TEST(EventTest, SetHandledTrue)
{
    WindowCloseEvent ev;
    ev.setHandled(true);
    EXPECT_TRUE(ev.isHandled());
}

TEST(EventTest, SetHandledFalseAfterTrue)
{
    WindowCloseEvent ev;
    ev.setHandled(true);
    ev.setHandled(false);
    EXPECT_FALSE(ev.isHandled());
}

TEST(EventTest, GetEventTypeMatchesStaticType)
{
    WindowCloseEvent ev;
    EXPECT_EQ(ev.getEventType(), EventType::WindowClose);
    EXPECT_EQ(ev.getEventType(), WindowCloseEvent::getStaticType());
}

TEST(EventTest, GetNameReturnsExpectedString)
{
    WindowCloseEvent ev;
    EXPECT_STREQ(ev.getName(), "WindowClose");
}

TEST(EventTest, ToStringReturnsGetName)
{
    WindowCloseEvent ev;
    EXPECT_EQ(ev.toString(), std::string("WindowClose"));
}

TEST(EventTest, DifferentEventTypesAreDistinct)
{
    WindowCloseEvent wc;
    ThemeChangedEvent tc;
    EXPECT_NE(wc.getEventType(), tc.getEventType());
    EXPECT_STRNE(wc.getName(), tc.getName());
}

// ========== EventDispatcher ==========

TEST(EventDispatcherTest, DispatchMatchingTypeReturnsTrue)
{
    WindowCloseEvent ev;
    EventDispatcher dispatcher(ev);

    bool handlerCalled = false;
    const bool dispatched = dispatcher.dispatch<WindowCloseEvent>(
        [&handlerCalled](WindowCloseEvent& /*e*/) -> bool
        {
            handlerCalled = true;
            return true;
        });

    EXPECT_TRUE(dispatched);
    EXPECT_TRUE(handlerCalled);
    EXPECT_TRUE(ev.isHandled());
}

TEST(EventDispatcherTest, DispatchNonMatchingTypeReturnsFalse)
{
    WindowCloseEvent ev;
    EventDispatcher dispatcher(ev);

    bool handlerCalled = false;
    const bool dispatched = dispatcher.dispatch<ThemeChangedEvent>(
        [&handlerCalled](ThemeChangedEvent& /*e*/) -> bool
        {
            handlerCalled = true;
            return true;
        });

    EXPECT_FALSE(dispatched);
    EXPECT_FALSE(handlerCalled);
    EXPECT_FALSE(ev.isHandled());
}

TEST(EventDispatcherTest, DispatchSkipsAlreadyHandledEvent)
{
    WindowCloseEvent ev;
    ev.setHandled(true);
    EventDispatcher dispatcher(ev);

    bool handlerCalled = false;
    const bool dispatched = dispatcher.dispatch<WindowCloseEvent>(
        [&handlerCalled](WindowCloseEvent& /*e*/) -> bool
        {
            handlerCalled = true;
            return true;
        });

    EXPECT_FALSE(dispatched);
    EXPECT_FALSE(handlerCalled);
}

TEST(EventDispatcherTest, HandlerReturnFalseDoesNotMarkHandled)
{
    WindowCloseEvent ev;
    EventDispatcher dispatcher(ev);

    const bool dispatched = dispatcher.dispatch<WindowCloseEvent>([](WindowCloseEvent& /*e*/) -> bool { return false; });

    EXPECT_TRUE(dispatched);
    EXPECT_FALSE(ev.isHandled());
}

} // namespace
} // namespace Core
