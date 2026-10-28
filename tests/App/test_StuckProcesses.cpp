// The Processes table's tint for a process stuck in uninterruptible sleep (#3097).

#include "App/Panels/StuckProcesses.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/ProcessState.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace App
{
namespace
{

[[nodiscard]] Domain::ProcessSnapshot proc(std::uint64_t key, char state)
{
    Domain::ProcessSnapshot s;
    s.uniqueKey = key;
    s.displayState = std::string(Domain::processStateName(state));
    return s;
}

TEST(StuckProcessesTest, OnlyAProcessInDiskSleepForTheWholeSpanIsStuck)
{
    StuckProcesses::Tracker tracker;
    tracker.onGeneration({proc(1, 'D'), proc(2, 'S'), proc(3, 'D')}, 10.0);
    EXPECT_FALSE(tracker.stuck(1)); // just entered D
    EXPECT_FALSE(tracker.stuck(2));

    // 3 left D for a moment, so it starts again; 1 stayed.
    tracker.onGeneration({proc(1, 'D'), proc(2, 'S'), proc(3, 'R')}, 11.0);
    tracker.onGeneration({proc(1, 'D'), proc(2, 'D'), proc(3, 'D')}, 10.0 + StuckProcesses::STUCK_SECONDS);
    EXPECT_TRUE(tracker.stuck(1));
    EXPECT_FALSE(tracker.stuck(2));
    EXPECT_FALSE(tracker.stuck(3));

    // Out of D, or gone: no longer stuck.
    tracker.onGeneration({proc(2, 'D'), proc(3, 'D')}, 20.0);
    EXPECT_FALSE(tracker.stuck(1));
    EXPECT_TRUE(tracker.stuck(2)); // in D since 12.0
    EXPECT_TRUE(tracker.stuck(3));
    tracker.onGeneration({proc(2, 'S'), proc(3, 'D')}, 21.0);
    EXPECT_FALSE(tracker.stuck(2));
    EXPECT_TRUE(tracker.stuck(3));
}

} // namespace
} // namespace App
