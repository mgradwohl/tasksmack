/// @file test_ProcessGeneration.cpp
/// @brief Domain::ProcessGeneration (#3345): the rows, their PID index and the process tree, made together; lookups
/// by identity through the generation's own index (#3334).

#include "Domain/ProcessGeneration.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

[[nodiscard]] Domain::ProcessSnapshot row(std::int32_t pid, std::int32_t parentPid, std::uint64_t startTimeTicks)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.pid = pid;
    snapshot.parentPid = parentPid;
    snapshot.startTimeTicks = startTimeTicks;
    return snapshot;
}

TEST(ProcessGenerationTest, TheEmptyGenerationHasNoRowsAndFindsNothing)
{
    const Domain::ProcessGeneration empty;
    EXPECT_TRUE(empty.rows().empty());
    EXPECT_EQ(empty.find(1, 0), nullptr);
}

TEST(ProcessGenerationTest, EachRowListsItsChildrenAndAParentThatStartedLaterIsNotOne)
{
    const Domain::ProcessGeneration generation({
        row(1, 0, 10),   // 0: init
        row(200, 1, 50), // 1: a child of init
        row(300, 1, 60), // 2: another
        row(400, 200, 70),
        row(500, 600, 40),  // 4: its PID 600's holder started later: a reused PID, not its parent
        row(600, 1, 90),    // 5
        row(700, 700, 100), // 6: its own parent: no link to itself
        row(800, 900, 0),   // 7: PID 900 isn't listed
    });
    const std::vector<Domain::ProcessSnapshot>& rows = generation.rows();
    EXPECT_EQ(rows[0].childrenIndices, (std::vector<std::size_t>{1, 2, 5}));
    EXPECT_EQ(rows[1].childrenIndices, (std::vector<std::size_t>{3}));
    EXPECT_TRUE(rows[5].childrenIndices.empty()) << "started after PID 500, so not its parent";
    EXPECT_TRUE(rows[6].childrenIndices.empty());
}

TEST(ProcessGenerationTest, AnUnknownStartTimeKeepsTheLink)
{
    const Domain::ProcessGeneration generation({row(1, 0, 0), row(2, 1, 5), row(3, 4, 7), row(4, 0, 0)});
    EXPECT_EQ(generation.rows()[0].childrenIndices, (std::vector<std::size_t>{1}));
    EXPECT_EQ(generation.rows()[3].childrenIndices, (std::vector<std::size_t>{2})) << "the parent's start time is 0";
}

// #3334: a lookup needs the whole identity; two processes started in the same tick share a start time, so the PID
// tells them apart, and one PID listed twice (reused within one read) finds each listing by its start time.
TEST(ProcessGenerationTest, FindMatchesTheWholeIdentityIncludingAPidListedTwice)
{
    const Domain::ProcessGeneration generation({row(100, 0, 10), row(101, 0, 10), row(100, 0, 20)});
    const Domain::ProcessSnapshot* first = generation.find(100, 10);
    const Domain::ProcessSnapshot* last = generation.find(100, 20);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(first, &generation.rows()[0]) << "the index maps PID 100 to its last listing; the first is found too";
    EXPECT_EQ(last, &generation.rows()[2]);
    EXPECT_EQ(generation.find(101, 10), &generation.rows()[1]);
    EXPECT_EQ(generation.find(101, 11), nullptr) << "the right PID with another start time is another process";
    EXPECT_EQ(generation.find(102, 10), nullptr);
}

// #3601: a lookup by PID alone is one probe of the index, so a PID listed twice finds its last listing.
TEST(ProcessGenerationTest, FindByPidAloneUsesTheIndexAndAPidListedTwiceFindsItsLastListing)
{
    const Domain::ProcessGeneration generation({row(100, 0, 10), row(101, 0, 10), row(100, 0, 20)});
    EXPECT_EQ(generation.find(100), &generation.rows()[2]);
    EXPECT_EQ(generation.find(101), &generation.rows()[1]);
    EXPECT_EQ(generation.find(102), nullptr);
    EXPECT_EQ(Domain::ProcessGeneration{}.find(1), nullptr);
}

} // namespace
