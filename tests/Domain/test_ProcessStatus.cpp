/// @file test_ProcessStatus.cpp
/// @brief Tests for process status field handling in Domain::ProcessModel
///
/// Tests verify that process status information (e.g., Suspended, Efficiency Mode)
/// is correctly passed through from platform probes to domain snapshots.

#include "Domain/ProcessModel.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using TestMocks::makeProcessCounters;
using TestMocks::MockProcessProbe;

namespace
{

// =============================================================================
// Status Field Tests
// =============================================================================

TEST(ProcessStatusTest, StatusFieldIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();

    // Create a process with status set
    Platform::ProcessCounters counter = makeProcessCounters(100, "test_proc");
    counter.status = "Suspended";

    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    // Enable status capability
    Platform::ProcessCapabilities caps;
    caps.hasStatus = true;
    probe->setCapabilities(caps);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].status, "Suspended");
}

TEST(ProcessStatusTest, EmptyStatusIsHandled)
{
    auto probe = std::make_unique<MockProcessProbe>();

    // Create a process with no status
    Platform::ProcessCounters counter = makeProcessCounters(101, "normal_proc");
    counter.status = "";

    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_TRUE(snaps[0].status.empty());
}

TEST(ProcessStatusTest, EfficiencyModeStatus)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters counter = makeProcessCounters(102, "efficient_proc");
    counter.status = "Efficiency Mode";

    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].status, "Efficiency Mode");
}

TEST(ProcessStatusTest, MultipleProcessesWithDifferentStatuses)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters c1 = makeProcessCounters(100, "suspended_proc");
    c1.status = "Suspended";

    Platform::ProcessCounters c2 = makeProcessCounters(101, "efficient_proc");
    c2.status = "Efficiency Mode";

    Platform::ProcessCounters c3 = makeProcessCounters(102, "normal_proc");
    c3.status = "";

    probe->setCounters({c1, c2, c3});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 3);

    // Find each process and verify status
    for (const auto& snap : snaps)
    {
        if (snap.pid == 100)
        {
            EXPECT_EQ(snap.status, "Suspended");
        }
        else if (snap.pid == 101)
        {
            EXPECT_EQ(snap.status, "Efficiency Mode");
        }
        else if (snap.pid == 102)
        {
            EXPECT_TRUE(snap.status.empty());
        }
    }
}

TEST(ProcessStatusTest, StatusPersistsAcrossRefreshes)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters counter = makeProcessCounters(100, "test_proc");
    counter.status = "Suspended";
    counter.userTime = 1000;
    counter.systemTime = 500;

    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));

    // First refresh
    model.refresh();
    auto snaps1 = model.snapshots();
    ASSERT_EQ(snaps1.size(), 1);
    EXPECT_EQ(snaps1[0].status, "Suspended");

    // Second refresh with updated times but same status
    counter.userTime = 1100;
    counter.systemTime = 550;
    model.updateFromCounters({counter}, 110000);
    auto snaps2 = model.snapshots();
    ASSERT_EQ(snaps2.size(), 1);
    EXPECT_EQ(snaps2[0].status, "Suspended");
}

TEST(ProcessStatusTest, StatusChangesAreDetected)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters counter = makeProcessCounters(100, "test_proc");
    counter.status = "Suspended";

    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));

    // First refresh: Suspended
    model.refresh();
    auto snaps1 = model.snapshots();
    ASSERT_EQ(snaps1.size(), 1);
    EXPECT_EQ(snaps1[0].status, "Suspended");

    // Second refresh: status changed to empty
    counter.status = "";
    model.updateFromCounters({counter}, 110000);
    auto snaps2 = model.snapshots();
    ASSERT_EQ(snaps2.size(), 1);
    EXPECT_TRUE(snaps2[0].status.empty());

    // Third refresh: status changed to Efficiency Mode
    counter.status = "Efficiency Mode";
    model.updateFromCounters({counter}, 120000);
    auto snaps3 = model.snapshots();
    ASSERT_EQ(snaps3.size(), 1);
    EXPECT_EQ(snaps3[0].status, "Efficiency Mode");
}

} // namespace
