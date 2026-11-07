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

// #3244: the label is worded where the Policy column draws it, held by value with no allocation; its
// text is the label's, word for word, up to the largest priority a uint32 holds.
TEST(SchedPolicyTest, TheHeldTextIsTheLabelWordForWord)
{
    EXPECT_EQ(schedPolicyText(SchedPolicy::Normal, 0).view(), "Normal");
    EXPECT_EQ(schedPolicyText(SchedPolicy::Batch, 0).view(), "Batch");
    EXPECT_EQ(schedPolicyText(SchedPolicy::Idle, 0).view(), "Idle");
    EXPECT_EQ(schedPolicyText(SchedPolicy::Deadline, 0).view(), "Deadline");
    EXPECT_EQ(schedPolicyText(SchedPolicy::Fifo, 1).view(), "FIFO 1");
    EXPECT_EQ(schedPolicyText(SchedPolicy::Fifo, 99).view(), "FIFO 99");
    EXPECT_EQ(schedPolicyText(SchedPolicy::RoundRobin, 10).view(), "RR 10");
    EXPECT_EQ(schedPolicyText(SchedPolicy::Fifo, 4294967295U).view(), "FIFO 4294967295") << "the longest fits";
    EXPECT_EQ(schedPolicyText(SchedPolicy::RoundRobin, 4294967295U).view(), "RR 4294967295");
    EXPECT_TRUE(schedPolicyText(SchedPolicy::Unknown, 0).empty());
    EXPECT_FALSE(schedPolicyText(SchedPolicy::Normal, 0).empty());
    EXPECT_EQ(schedPolicyLabel(SchedPolicy::Fifo, 4294967295U), "FIFO 4294967295");
}

TEST(SchedPolicyTest, OrderIsByPolicyThenRealTimePriority)
{
    EXPECT_TRUE(compareSchedPolicy(SchedPolicy::Idle, 0, SchedPolicy::Normal, 0) < 0);
    EXPECT_TRUE(compareSchedPolicy(SchedPolicy::Normal, 0, SchedPolicy::Fifo, 1) < 0);
    EXPECT_TRUE(compareSchedPolicy(SchedPolicy::Fifo, 10, SchedPolicy::Fifo, 50) < 0);
    EXPECT_TRUE(compareSchedPolicy(SchedPolicy::RoundRobin, 5, SchedPolicy::RoundRobin, 5) == 0);
}

// #3219: Set scheduling policy may set every policy but Deadline (and Unknown), and real time takes 1 to 99.
TEST(SchedPolicyTest, EveryPolicyButDeadlineIsSettableWithItsPriority)
{
    for (const SchedPolicy policy : {SchedPolicy::Normal, SchedPolicy::Batch, SchedPolicy::Idle})
    {
        EXPECT_TRUE(isSettableSchedPolicy(policy));
        EXPECT_FALSE(isRealtimeSchedPolicy(policy));
        EXPECT_TRUE(isValidSchedPolicySetting(policy, 0));
        EXPECT_FALSE(isValidSchedPolicySetting(policy, 1));
        EXPECT_GE(settableSchedPolicyToLinux(policy), 0);
    }
    for (const SchedPolicy policy : {SchedPolicy::Fifo, SchedPolicy::RoundRobin})
    {
        EXPECT_TRUE(isSettableSchedPolicy(policy));
        EXPECT_TRUE(isRealtimeSchedPolicy(policy));
        EXPECT_FALSE(isValidSchedPolicySetting(policy, 0));
        EXPECT_TRUE(isValidSchedPolicySetting(policy, 1));
        EXPECT_TRUE(isValidSchedPolicySetting(policy, 99));
        EXPECT_FALSE(isValidSchedPolicySetting(policy, 100));
        EXPECT_FALSE(isValidSchedPolicySetting(policy, -1));
    }
    EXPECT_EQ(settableSchedPolicyToLinux(SchedPolicy::Fifo), 1);
    EXPECT_EQ(settableSchedPolicyToLinux(SchedPolicy::RoundRobin), 2);
    for (const SchedPolicy policy : {SchedPolicy::Deadline, SchedPolicy::Unknown})
    {
        EXPECT_FALSE(isSettableSchedPolicy(policy));
        EXPECT_FALSE(isValidSchedPolicySetting(policy, 0));
        EXPECT_FALSE(isValidSchedPolicySetting(policy, 10));
        EXPECT_EQ(settableSchedPolicyToLinux(policy), -1);
    }
}

} // namespace
} // namespace Platform
