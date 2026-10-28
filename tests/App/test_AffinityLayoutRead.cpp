/// @file test_AffinityLayoutRead.cpp
/// @brief App::AffinityLayoutRead (#3182): the Set affinity dialog's CPU layout, read once on a worker and
/// kept, never on the caller's (the UI) thread.

#include "App/Panels/AffinityLayoutRead.h"
#include "Mocks/MockProbes.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <thread>

namespace App
{
namespace
{

[[nodiscard]] Platform::CpuAffinityLayout twoGroups()
{
    Platform::CpuAffinityLayout layout;
    layout.groupSizes = {2, 2};
    layout.online = Platform::CpuAffinity::fromMask(0b1111U);
    layout.appliedAsCpuSets = true;
    return layout;
}

TEST(AffinityLayoutReadTest, ReadsOnceOnAWorkerAndKeepsIt)
{
    TestMocks::MockAffinityLayoutActions actions;
    actions.setAffinityLayout(twoGroups());
    AffinityLayoutRead read;
    EXPECT_FALSE(read.started());
    EXPECT_EQ(read.layout(), nullptr) << "nothing read before start()";

    for (int frame = 0; frame < 5; ++frame)
    {
        read.start(&actions); // every frame's update path: only the first starts a read
    }
    EXPECT_TRUE(read.started());
    const Platform::CpuAffinityLayout* layout = read.layout(/*wait=*/true);
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->groupSizes, twoGroups().groupSizes);
    EXPECT_EQ(layout->online, twoGroups().online);
    EXPECT_TRUE(layout->appliedAsCpuSets);
    EXPECT_NE(actions.affinityLayoutThread(), std::this_thread::get_id()) << "read on the worker, not here";

    // Kept: later frames and openings read nothing more.
    read.start(&actions);
    EXPECT_EQ(read.layout(), layout);
    EXPECT_EQ(actions.affinityLayoutCount(), 1);
}

TEST(AffinityLayoutReadTest, PendingUntilTheReadFinishesAndNeverWaitedFor)
{
    TestMocks::MockAffinityLayoutActions actions;
    actions.holdAffinityLayout();
    AffinityLayoutRead read;
    read.start(&actions);
    for (int frame = 0; frame < 3; ++frame)
    {
        EXPECT_EQ(read.layout(), nullptr) << "still being read: the caller isn't held up";
    }
    actions.releaseAffinityLayout();
    EXPECT_NE(read.layout(/*wait=*/true), nullptr);
    EXPECT_EQ(actions.affinityLayoutCount(), 1);
}

TEST(AffinityLayoutReadTest, NullActionsReadNothing)
{
    AffinityLayoutRead read;
    read.start(nullptr);
    EXPECT_FALSE(read.started());
    EXPECT_EQ(read.layout(/*wait=*/true), nullptr);
}

// A read that throws is logged and taken as no layout: the dialog then offers CPUs 0 to N-1, ungrouped,
// and isn't left waiting, nor is the read tried again.
TEST(AffinityLayoutReadTest, AReadThatThrowsIsAnEmptyLayout)
{
    TestMocks::MockAffinityLayoutActions actions;
    actions.setAffinityLayout(twoGroups());
    actions.throwFromAffinityLayout();
    AffinityLayoutRead read;
    read.start(&actions);
    const Platform::CpuAffinityLayout* layout = read.layout(/*wait=*/true);
    ASSERT_NE(layout, nullptr);
    EXPECT_TRUE(layout->groupSizes.empty());
    EXPECT_TRUE(layout->online.empty());
    EXPECT_FALSE(layout->appliedAsCpuSets);
    read.start(&actions);
    EXPECT_EQ(actions.affinityLayoutCount(), 1);
}

} // namespace
} // namespace App
