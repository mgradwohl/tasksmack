#include "Core/WindowEvents.h"

#include <gtest/gtest.h>

namespace Core
{
namespace
{

TEST(WindowEventsTest, WindowCloseEventHasExpectedTypeAndName)
{
    WindowCloseEvent event;
    EXPECT_EQ(event.getEventType(), EventType::WindowClose);
    EXPECT_EQ(event.getEventType(), WindowCloseEvent::getStaticType());
    EXPECT_STREQ(event.getName(), "WindowClose");
    EXPECT_EQ(event.toString(), "WindowClose");
    EXPECT_FALSE(event.isHandled());
}

TEST(WindowEventsTest, WindowCloseEventDispatches)
{
    WindowCloseEvent event;
    EventDispatcher dispatcher(event);

    bool called = false;
    const bool dispatched = dispatcher.dispatch<WindowCloseEvent>(
        [&called](WindowCloseEvent& /*e*/) -> bool
        {
            called = true;
            return true;
        });

    EXPECT_TRUE(dispatched);
    EXPECT_TRUE(called);
    EXPECT_TRUE(event.isHandled());
}

// ========== WindowResizedEvent ==========

TEST(WindowEventsTest, WindowResizedEventHasExpectedTypeAndName)
{
    WindowResizedEvent event(1280, 720);
    EXPECT_EQ(event.getEventType(), EventType::WindowResized);
    EXPECT_EQ(event.getEventType(), WindowResizedEvent::getStaticType());
    EXPECT_STREQ(event.getName(), "WindowResized");
    EXPECT_FALSE(event.isHandled());
}

TEST(WindowEventsTest, WindowResizedEventStoresDimensions)
{
    WindowResizedEvent event(1920, 1080);
    EXPECT_EQ(event.getWidth(), 1920);
    EXPECT_EQ(event.getHeight(), 1080);
}

TEST(WindowEventsTest, WindowResizedEventToStringContainsDimensions)
{
    WindowResizedEvent event(800, 600);
    const std::string str = event.toString();
    EXPECT_NE(str.find("800"), std::string::npos);
    EXPECT_NE(str.find("600"), std::string::npos);
}

TEST(WindowEventsTest, WindowResizedEventDispatches)
{
    WindowResizedEvent event(640, 480);
    EventDispatcher dispatcher(event);

    int receivedWidth = 0;
    int receivedHeight = 0;
    const bool dispatched = dispatcher.dispatch<WindowResizedEvent>(
        [&receivedWidth, &receivedHeight](WindowResizedEvent& e) -> bool
        {
            receivedWidth = e.getWidth();
            receivedHeight = e.getHeight();
            return true;
        });

    EXPECT_TRUE(dispatched);
    EXPECT_EQ(receivedWidth, 640);
    EXPECT_EQ(receivedHeight, 480);
    EXPECT_TRUE(event.isHandled());
}

TEST(WindowEventsTest, WindowResizedEventDoesNotDispatchAsWindowClose)
{
    WindowResizedEvent event(800, 600);
    EventDispatcher dispatcher(event);

    bool called = false;
    const bool dispatched = dispatcher.dispatch<WindowCloseEvent>(
        [&called](WindowCloseEvent& /*e*/) -> bool
        {
            called = true;
            return true;
        });

    EXPECT_FALSE(dispatched);
    EXPECT_FALSE(called);
    EXPECT_FALSE(event.isHandled());
}

} // namespace
} // namespace Core
