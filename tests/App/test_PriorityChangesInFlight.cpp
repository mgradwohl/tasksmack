/// @file test_PriorityChangesInFlight.cpp
/// @brief The priority changes in flight the Processes table and Process Details share (#3467): a process
/// one panel publishes is held for the other only, by its exact identity (a reused PID is not held), each
/// publish replaces that panel's last, and the reasons name the panel the change runs in.

#include "App/Panels/PriorityChangesInFlight.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace App
{
namespace
{

using Panel = PriorityChangesInFlight::Panel;

constexpr Platform::ProcessTarget SERVER{.pid = 100, .startTimeTicks = 1000};
constexpr Platform::ProcessTarget WORKER{.pid = 101, .startTimeTicks = 1100};

TEST(PriorityChangesInFlightTest, NothingIsHeldUntilAPanelPublishesIt)
{
    const PriorityChangesInFlight changes;
    EXPECT_FALSE(changes.heldElsewhere(Panel::Processes, SERVER));
    EXPECT_FALSE(changes.heldElsewhere(Panel::ProcessDetails, SERVER));
    EXPECT_EQ(changes.holdReason(Panel::Processes, SERVER), nullptr);
}

TEST(PriorityChangesInFlightTest, AProcessOnePanelPublishesIsHeldForTheOtherOnly)
{
    PriorityChangesInFlight changes;
    const std::array details{SERVER};
    changes.publish(Panel::ProcessDetails, details);
    EXPECT_TRUE(changes.heldElsewhere(Panel::Processes, SERVER));
    EXPECT_FALSE(changes.heldElsewhere(Panel::ProcessDetails, SERVER)) << "a panel's own change is its own business";
    EXPECT_FALSE(changes.heldElsewhere(Panel::Processes, WORKER));

    const std::array table{SERVER, WORKER};
    changes.publish(Panel::Processes, table);
    EXPECT_TRUE(changes.heldElsewhere(Panel::ProcessDetails, WORKER));
    EXPECT_TRUE(changes.heldElsewhere(Panel::Processes, SERVER)) << "the table's publish leaves Process Details' alone";
}

TEST(PriorityChangesInFlightTest, APublishReplacesThatPanelsLast)
{
    PriorityChangesInFlight changes;
    const std::array both{SERVER, WORKER};
    changes.publish(Panel::Processes, both);
    const std::array worker{WORKER};
    changes.publish(Panel::Processes, worker);
    EXPECT_FALSE(changes.heldElsewhere(Panel::ProcessDetails, SERVER));
    EXPECT_TRUE(changes.heldElsewhere(Panel::ProcessDetails, WORKER));
    changes.publish(Panel::Processes, {});
    EXPECT_FALSE(changes.heldElsewhere(Panel::ProcessDetails, WORKER)) << "finished: released";
}

// A reused PID is another process: a change for the old one holds nothing for the new one.
TEST(PriorityChangesInFlightTest, ASamePidWithAnotherStartTimeIsNotHeld)
{
    PriorityChangesInFlight changes;
    const std::array details{SERVER};
    changes.publish(Panel::ProcessDetails, details);
    EXPECT_FALSE(changes.heldElsewhere(Panel::Processes, {.pid = SERVER.pid, .startTimeTicks = SERVER.startTimeTicks + 1}));
    EXPECT_FALSE(changes.heldElsewhere(Panel::Processes, {.pid = SERVER.pid, .startTimeTicks = 0}));
}

TEST(PriorityChangesInFlightTest, TheReasonsNameThePanelTheChangeRunsIn)
{
    PriorityChangesInFlight changes;
    const std::array one{SERVER};
    changes.publish(Panel::ProcessDetails, one);
    changes.publish(Panel::Processes, one);
    EXPECT_TRUE(std::string_view(changes.holdReason(Panel::Processes, SERVER)).contains("Process Details"));
    EXPECT_TRUE(std::string_view(changes.holdReason(Panel::ProcessDetails, SERVER)).contains("Processes tab"));
    EXPECT_TRUE(std::string_view(PriorityChangesInFlight::refusal(Panel::Processes)).contains("still running in Process Details"));
    EXPECT_TRUE(std::string_view(PriorityChangesInFlight::refusal(Panel::ProcessDetails)).contains("still running in the Processes tab"));
}

} // namespace
} // namespace App
