/// @file test_ApplicationEvents.cpp
/// @brief Tests for the settings-change events' startup flag (#1102)

#include "Core/ApplicationEvents.h"
#include "Core/Event.h"

#include <gtest/gtest.h>

namespace Core
{
namespace
{

// A user's change forces a sample so it shows at once; the configured value ShellLayer delivers at
// startup is applied without one, since the models were just seeded (#1102).
TEST(ApplicationEventsTest, SettingsChangesAreUserChangesByDefault)
{
    EXPECT_FALSE(RefreshRateChangedEvent(500).isInitial());
    EXPECT_FALSE(HistoryDurationChangedEvent(600).isInitial());
}

TEST(ApplicationEventsTest, StartupSettingsAreMarkedInitial)
{
    const RefreshRateChangedEvent refresh(250, /*initial=*/true);
    const HistoryDurationChangedEvent history(1800, /*initial=*/true);
    EXPECT_TRUE(refresh.isInitial());
    EXPECT_EQ(refresh.getIntervalMs(), 250);
    EXPECT_TRUE(history.isInitial());
    EXPECT_EQ(history.getSeconds(), 1800);
}

// The row menu's Details item asks the shell for the Process Details tab with its own event type,
// apart from the selection itself (#1209).
TEST(ApplicationEventsTest, ShowProcessDetailsHasItsOwnEventType)
{
    ShowProcessDetailsEvent show;
    EXPECT_EQ(show.getEventType(), EventType::ShowProcessDetails);
    EXPECT_NE(ShowProcessDetailsEvent::getStaticType(), ProcessSelectedEvent::getStaticType());

    EventDispatcher dispatcher(show);
    bool handled = false;
    EXPECT_TRUE(dispatcher.dispatch<ShowProcessDetailsEvent>(
        [&handled](ShowProcessDetailsEvent& /*e*/)
        {
            handled = true;
            return false;
        }));
    EXPECT_TRUE(handled);
}

// #1395: each event names itself and what it carries when logged.
TEST(ApplicationEventsTest, EachEventDescribesItselfForTheLog)
{
    EXPECT_EQ(ProcessSelectedEvent(4242, 777).toString(), "ProcessSelectedEvent: pid=4242, startTimeTicks=777");
    EXPECT_EQ(ShowProcessDetailsEvent().toString(), "ShowProcessDetailsEvent");
    EXPECT_EQ(RefreshRateChangedEvent(500).toString(), "RefreshRateChangedEvent: 500ms");
    EXPECT_EQ(HistoryDurationChangedEvent(600).toString(), "HistoryDurationChangedEvent: 600s");
    EXPECT_EQ(ActiveTabChangedEvent("Processes").toString(), "ActiveTabChangedEvent: Processes");
    EXPECT_EQ(OpenSettingsEvent().toString(), "OpenSettingsEvent");
    EXPECT_EQ(OpenAboutEvent().toString(), "OpenAboutEvent");
    EXPECT_EQ(OpenHelpEvent().toString(), "OpenHelpEvent");
    EXPECT_EQ(OpenElevationNoticeEvent().toString(), "OpenElevationNoticeEvent");
}

} // namespace
} // namespace Core
