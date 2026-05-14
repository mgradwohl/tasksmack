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

} // namespace
} // namespace Core
