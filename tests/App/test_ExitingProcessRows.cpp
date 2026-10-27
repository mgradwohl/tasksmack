#include "App/Panels/ExitingProcessRows.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace App
{
namespace
{

using ExitingProcessRows::Tracker;

[[nodiscard]] Domain::ProcessSnapshot proc(std::int32_t pid, std::int32_t parentPid, std::uint64_t startTicks = 100)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.pid = pid;
    snapshot.parentPid = parentPid;
    snapshot.startTimeTicks = startTicks;
    snapshot.uniqueKey = (static_cast<std::uint64_t>(pid) << 32U) ^ startTicks;
    return snapshot;
}

TEST(ExitingProcessRowsTest, AProcessMissingFromTheNewGenerationIsKeptUntilItsTimeIsUp)
{
    Tracker tracker;
    const std::vector<Domain::ProcessSnapshot> before{proc(1, 0), proc(20, 1), proc(30, 1)};
    const std::vector<Domain::ProcessSnapshot> after{proc(1, 0), proc(30, 1)};

    EXPECT_TRUE(tracker.onGeneration(before, after, 10.0));
    ASSERT_EQ(tracker.exited().size(), 1U);
    EXPECT_EQ(tracker.exited().front().snapshot.pid, 20);

    EXPECT_FALSE(tracker.expire(10.0 + ExitingProcessRows::DURATION_SECONDS - 0.1));
    EXPECT_TRUE(tracker.expire(10.0 + ExitingProcessRows::DURATION_SECONDS));
    EXPECT_TRUE(tracker.exited().empty());
}

TEST(ExitingProcessRowsTest, APidReusedByANewProcessStillCountsAsAnExit)
{
    Tracker tracker;
    // Same PID, another start time: another process, so the first exited.
    EXPECT_TRUE(tracker.onGeneration({proc(20, 1, 100)}, {proc(20, 1, 200)}, 0.0));
    ASSERT_EQ(tracker.exited().size(), 1U);
    EXPECT_EQ(tracker.exited().front().snapshot.startTimeTicks, 100U);
}

TEST(ExitingProcessRowsTest, NothingExitedAddsNothingAndClearDropsAll)
{
    Tracker tracker;
    const std::vector<Domain::ProcessSnapshot> same{proc(1, 0), proc(2, 1)};
    EXPECT_FALSE(tracker.onGeneration(same, same, 0.0));
    EXPECT_FALSE(tracker.clear());
    EXPECT_TRUE(tracker.onGeneration(same, {proc(1, 0)}, 0.0));
    EXPECT_TRUE(tracker.clear());
    EXPECT_TRUE(tracker.exited().empty());
}

TEST(ExitingProcessRowsTest, ExitedRowsFollowTheRunningOnesWithIndicesUnchanged)
{
    std::vector<Domain::ProcessSnapshot> current{proc(1, 0), proc(30, 1)};
    current[0].childrenIndices = {1};
    Tracker tracker;
    Domain::ProcessSnapshot gone = proc(20, 1);
    gone.childrenIndices = {7}; // into its own, older generation
    ASSERT_TRUE(tracker.onGeneration({proc(1, 0), gone, proc(30, 1)}, current, 0.0));

    const std::vector<Domain::ProcessSnapshot> rows = ExitingProcessRows::displayRows(current, tracker.exited());

    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].pid, 1);
    EXPECT_EQ(rows[1].pid, 30);
    EXPECT_EQ(rows[2].pid, 20);
    EXPECT_TRUE(rows[2].childrenIndices.empty());
    // Listed under its running parent, after the parent's own children.
    EXPECT_EQ(rows[0].childrenIndices, (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(ExitingProcessRows::countLive({0, 1, 2}, current.size()), 2U);
}

TEST(ExitingProcessRowsTest, AnExitedProcessIsARootUnlessItsParentStartedBeforeIt)
{
    // PID 5 now names a process that started after the exited one: not its parent.
    const std::vector<Domain::ProcessSnapshot> current{proc(5, 0, 900)};
    Tracker tracker;
    ASSERT_TRUE(tracker.onGeneration({proc(6, 5, 300)}, current, 0.0));

    const std::vector<Domain::ProcessSnapshot> rows = ExitingProcessRows::displayRows(current, tracker.exited());

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_TRUE(rows[0].childrenIndices.empty());
}

TEST(ExitingProcessRowsTest, AChildThatExitedWithItsParentStaysUnderIt)
{
    const std::vector<Domain::ProcessSnapshot> current{proc(1, 0)};
    Tracker tracker;
    ASSERT_TRUE(tracker.onGeneration({proc(1, 0), proc(40, 1, 100), proc(41, 40, 150)}, current, 0.0));

    const std::vector<Domain::ProcessSnapshot> rows = ExitingProcessRows::displayRows(current, tracker.exited());

    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].childrenIndices, (std::vector<std::size_t>{1}));
    EXPECT_EQ(rows[1].childrenIndices, (std::vector<std::size_t>{2}));
}

} // namespace
} // namespace App
