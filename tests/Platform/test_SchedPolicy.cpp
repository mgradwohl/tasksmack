// A process's CPU scheduling policy (#3086): Linux's numbers, the labels and the column's order.

#include "Platform/SchedPolicy.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

TEST(SchedPolicyTest, LinuxNumbersMapToPoliciesIgnoringResetOnFork)
{
    EXPECT_EQ(schedPolicyFromLinux(0), SchedPolicy::Normal);
    EXPECT_EQ(schedPolicyFromLinux(1), SchedPolicy::Fifo);
    EXPECT_EQ(schedPolicyFromLinux(2), SchedPolicy::RoundRobin);
    EXPECT_EQ(schedPolicyFromLinux(3), SchedPolicy::Batch);
    EXPECT_EQ(schedPolicyFromLinux(5), SchedPolicy::Idle);
    EXPECT_EQ(schedPolicyFromLinux(6), SchedPolicy::Deadline);
    EXPECT_EQ(schedPolicyFromLinux(0x40000000U | 1U), SchedPolicy::Fifo); // SCHED_RESET_ON_FORK
    EXPECT_EQ(schedPolicyFromLinux(4), SchedPolicy::Unknown);             // SCHED_ISO, never merged
}

TEST(SchedPolicyTest, LabelsNameTheRealTimePriority)
{
    EXPECT_EQ(schedPolicyLabel(SchedPolicy::Normal, 0), "Normal");
    EXPECT_EQ(schedPolicyLabel(SchedPolicy::Batch, 0), "Batch");
    EXPECT_EQ(schedPolicyLabel(SchedPolicy::Idle, 0), "Idle");
    EXPECT_EQ(schedPolicyLabel(SchedPolicy::Deadline, 0), "Deadline");
    EXPECT_EQ(schedPolicyLabel(SchedPolicy::Fifo, 50), "FIFO 50");
    EXPECT_EQ(schedPolicyLabel(SchedPolicy::RoundRobin, 10), "RR 10");
    EXPECT_EQ(schedPolicyLabel(SchedPolicy::Unknown, 0), "");
}

TEST(SchedPolicyTest, OrderIsByPolicyThenRealTimePriority)
{
    EXPECT_TRUE(compareSchedPolicy(SchedPolicy::Idle, 0, SchedPolicy::Normal, 0) < 0);
    EXPECT_TRUE(compareSchedPolicy(SchedPolicy::Normal, 0, SchedPolicy::Fifo, 1) < 0);
    EXPECT_TRUE(compareSchedPolicy(SchedPolicy::Fifo, 10, SchedPolicy::Fifo, 50) < 0);
    EXPECT_TRUE(compareSchedPolicy(SchedPolicy::RoundRobin, 5, SchedPolicy::RoundRobin, 5) == 0);
}

} // namespace
} // namespace Platform
