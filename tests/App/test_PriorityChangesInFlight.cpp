/// @file test_PriorityChangesInFlight.cpp
/// @brief The priority changes in flight the Processes table and Process Details share (#3467): a process
/// one panel publishes is held for the other only, by its exact identity (a reused PID is not held), each
/// publish replaces that panel's last, and the reasons name the panel the change runs in. I/O priority changes are
/// held apart from CPU priority ones (#3884): each kind holds only its own.

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
using Change = PriorityChangesInFlight::Change;

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

// #3884: an I/O priority change in one panel holds the other's I/O priority controls, not its CPU priority ones, and
// a CPU priority change holds no I/O priority control: the two set different things, so they never race.
TEST(PriorityChangesInFlightTest, IoPriorityChangesAreHeldApartFromCpuPriorityOnes)
{
    PriorityChangesInFlight changes;
    const std::array details{SERVER};
    changes.publish(Panel::ProcessDetails, details, Change::IoPriority);
    EXPECT_TRUE(changes.heldElsewhere(Panel::Processes, SERVER, Change::IoPriority));
    EXPECT_FALSE(changes.heldElsewhere(Panel::Processes, SERVER)) << "an I/O change holds no CPU priority control";
    EXPECT_FALSE(changes.heldElsewhere(Panel::ProcessDetails, SERVER, Change::IoPriority)) << "its own panel's business";

    const std::array table{WORKER};
    changes.publish(Panel::Processes, table);
    EXPECT_TRUE(changes.heldElsewhere(Panel::ProcessDetails, WORKER));
    EXPECT_FALSE(changes.heldElsewhere(Panel::ProcessDetails, WORKER, Change::IoPriority)) << "a CPU change holds no I/O control";

    // Each kind's publish replaces only that kind's last.
    changes.publish(Panel::ProcessDetails, {});
    EXPECT_TRUE(changes.heldElsewhere(Panel::Processes, SERVER, Change::IoPriority)) << "clearing CPU claims leaves I/O ones";
    changes.publish(Panel::ProcessDetails, {}, Change::IoPriority);
    EXPECT_FALSE(changes.heldElsewhere(Panel::Processes, SERVER, Change::IoPriority)) << "finished: released";
}

TEST(PriorityChangesInFlightTest, TheIoReasonsSayIoPriorityAndNameThePanel)
{
    PriorityChangesInFlight changes;
    const std::array one{SERVER};
    changes.publish(Panel::ProcessDetails, one, Change::IoPriority);
    changes.publish(Panel::Processes, one, Change::IoPriority);
    const std::string_view table(changes.holdReason(Panel::Processes, SERVER, Change::IoPriority));
    EXPECT_TRUE(table.contains("I/O priority change in Process Details")) << table;
    const std::string_view details(changes.holdReason(Panel::ProcessDetails, SERVER, Change::IoPriority));
    EXPECT_TRUE(details.contains("I/O priority change in the Processes tab")) << details;
    EXPECT_TRUE(std::string_view(PriorityChangesInFlight::refusal(Panel::Processes, Change::IoPriority)).contains("I/O priority"));
    EXPECT_FALSE(std::string_view(changes.holdReason(Panel::Processes, SERVER, Change::IoPriority)).empty());
    EXPECT_EQ(changes.holdReason(Panel::Processes, SERVER), nullptr) << "no CPU priority change is in flight";
}

} // namespace
} // namespace App
