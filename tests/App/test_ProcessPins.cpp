// Pinned processes (#3061): which names are pinned, and pinned rows first in either view.

#include "App/Panels/ProcessPins.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <string>
#include <vector>

namespace App
{
namespace
{

using ProcessPins::Pins;

[[nodiscard]] Domain::ProcessSnapshot named(const std::string& name)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.name = name;
    return snapshot;
}

TEST(ProcessPinsTest, PinAndUnpinByName)
{
    Pins pins;
    EXPECT_TRUE(pins.empty());
    EXPECT_TRUE(pins.pin("make"));
    EXPECT_FALSE(pins.pin("make")); // already
    EXPECT_TRUE(pins.contains("make"));
    EXPECT_FALSE(pins.contains("Make")); // names are exact
    EXPECT_TRUE(pins.unpin("make"));
    EXPECT_FALSE(pins.unpin("make"));
    EXPECT_TRUE(pins.empty());
}

TEST(ProcessPinsTest, TheConfigListIsFilteredAndWrittenBackSorted)
{
    const std::string tooLong(ProcessPins::MAX_NAME_BYTES + 1, 'x');
    const Pins pins({"server", "", "game", tooLong, "server"});
    EXPECT_EQ(pins.names(), (std::vector<std::string>{"game", "server"}));
}

TEST(ProcessPinsTest, NoMoreThanMaxPins)
{
    Pins pins;
    for (std::size_t i = 0; i < ProcessPins::MAX_PINS; ++i)
    {
        EXPECT_TRUE(pins.pin(std::format("p{}", i)));
    }
    EXPECT_FALSE(pins.pin("one-too-many"));
    EXPECT_EQ(pins.names().size(), ProcessPins::MAX_PINS);
}

TEST(ProcessPinsTest, PinnedRowsComeFirstEachGroupKeepingItsOrder)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{named("a"), named("game"), named("b"), named("server"), named("game")};
    Pins pins;
    ASSERT_TRUE(pins.pin("game"));
    ASSERT_TRUE(pins.pin("server"));

    std::vector<std::size_t> order{0, 4, 2, 3, 1, 99}; // the table's sort; 99 is out of range
    ProcessPins::pinnedFirst(order, snapshots, pins);

    EXPECT_EQ(order, (std::vector<std::size_t>{4, 3, 1, 0, 2, 99}));
}

TEST(ProcessPinsTest, NoPinsLeavesTheOrderAlone)
{
    const std::vector<Domain::ProcessSnapshot> snapshots{named("a"), named("b")};
    std::vector<std::size_t> order{1, 0};
    ProcessPins::pinnedFirst(order, snapshots, Pins{});
    EXPECT_EQ(order, (std::vector<std::size_t>{1, 0}));
}

} // namespace
} // namespace App
