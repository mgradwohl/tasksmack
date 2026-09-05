/// @file test_Event.cpp
/// @brief Tests for Core::Event, EventDispatcher

#include "Core/Event.h"
#include "Core/WindowEvents.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace Core
{
namespace
{

// ========== Concrete event types for testing ==========

class ThemeChangedEvent : public Event
{
  public:
    EVENT_CLASS_TYPE(ThemeChanged) // NOLINT(cppcoreguidelines-macro-usage)
};

/// Flips an external flag on destruction, so a test can observe whether the derived
/// destructor actually ran when destroyed through a base Event* (proving virtual dispatch
/// reached it, rather than just proving the call didn't crash).
class DestructorTrackingEvent : public Event
{
  public:
    explicit DestructorTrackingEvent(bool& destroyedFlag) : m_DestroyedFlag(destroyedFlag)
    {}
    ~DestructorTrackingEvent() override
    {
        m_DestroyedFlag = true;
    }
    DestructorTrackingEvent(const DestructorTrackingEvent&) = delete;
    DestructorTrackingEvent& operator=(const DestructorTrackingEvent&) = delete;
    DestructorTrackingEvent(DestructorTrackingEvent&&) = delete;
    DestructorTrackingEvent& operator=(DestructorTrackingEvent&&) = delete;

    EVENT_CLASS_TYPE(ThemeChanged) // NOLINT(cppcoreguidelines-macro-usage)

  private:
    bool& m_DestroyedFlag;
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

TEST(EventTest, DestroysThroughBasePointerWithoutLeaking)
{
    // Event's own virtual destructor body is only exercised when destruction happens
    // through a base Event* (as opposed to a concrete type going out of scope directly),
    // so this is the only coverage of that vtable slot. Asserting the derived destructor
    // actually ran (not just that reset() didn't crash) verifies virtual dispatch reached
    // it, matching this codebase's Rule-of-5 discipline.
    bool destroyed = false;
    std::unique_ptr<Event> ev = std::make_unique<DestructorTrackingEvent>(destroyed);
    ev.reset();
    EXPECT_TRUE(destroyed);
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
