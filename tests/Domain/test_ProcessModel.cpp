/// @file test_ProcessModel.cpp
/// @brief Comprehensive tests for Domain::ProcessModel
///
/// Tests cover:
/// - CPU percentage calculations from counter deltas
/// - Snapshot data transformation
/// - State character translation
/// - Unique key generation for PID reuse handling
/// - Thread-safe operations

#include "Domain/ProcessModel.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

// Use shared mock from TestMocks namespace
using TestMocks::makeProcessCounters;
using TestMocks::MockProcessProbe;

namespace
{

// Test constants for overflow scenarios
constexpr uint64_t OVERFLOW_TEST_MARGIN = 10000; // Distance from max value for overflow tests

/// Helper to create a process counter (legacy compatibility wrapper).
Platform::ProcessCounters makeCounter(int32_t pid,
                                      const std::string& name,
                                      char state,
                                      uint64_t userTime,
                                      uint64_t systemTime,
                                      uint64_t startTime = 1000,
                                      uint64_t rssBytes = 1024 * 1024,
                                      int32_t parentPid = 1)
{
    return makeProcessCounters(pid, name, state, userTime, systemTime, startTime, rssBytes, parentPid);
}

} // namespace

// =============================================================================
// Construction Tests
// =============================================================================

TEST(ProcessModelTest, ConstructWithValidProbe)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    EXPECT_EQ(model.processCount(), 0);
    EXPECT_TRUE(model.snapshots().empty());
}

TEST(ProcessModelTest, ConstructWithNullProbeDoesNotCrash)
{
    Domain::ProcessModel model(nullptr);
    model.refresh(); // Should not crash

    EXPECT_EQ(model.processCount(), 0);
}

TEST(ProcessModelTest, CapabilitiesAreExposedFromProbe)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCapabilities caps;
    caps.hasIoCounters = true;
    caps.hasThreadCount = true;
    caps.hasUserSystemTime = true;
    caps.hasStartTime = true;
    probe->setCapabilities(caps);

    Domain::ProcessModel model(std::move(probe));

    const auto& modelCaps = model.capabilities();
    EXPECT_TRUE(modelCaps.hasIoCounters);
    EXPECT_TRUE(modelCaps.hasThreadCount);
}

// =============================================================================
// CPU Percentage Calculation Tests
// =============================================================================

TEST(ProcessModelTest, FirstRefreshShowsZeroCpuPercent)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuPercent, 0.0); // No previous data to compare
}

TEST(ProcessModelTest, CpuPercentCalculatedFromDeltas)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample: process has used 1000 user + 500 system = 1500 total
    rawProbe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Second sample: process has used 2000 user + 1000 system = 3000 total
    // Delta = 3000 - 1500 = 1500
    // Total CPU delta = 200000 - 100000 = 100000
    // CPU% = (1500 / 100000) * 100 = 1.5%
    rawProbe->setCounters({makeCounter(100, "test_proc", 'R', 2000, 1000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 1.5);
}

TEST(ProcessModelTest, CpuPercentForMultipleProcesses)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample: two processes
    rawProbe->setCounters({
        makeCounter(100, "proc_a", 'R', 1000, 0),
        makeCounter(200, "proc_b", 'R', 2000, 0),
    });
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Second sample: proc_a gained 500, proc_b gained 1000
    // Total CPU delta = 100000
    // proc_a: (500 / 100000) * 100 = 0.5%
    // proc_b: (1000 / 100000) * 100 = 1.0%
    rawProbe->setCounters({
        makeCounter(100, "proc_a", 'R', 1500, 0),
        makeCounter(200, "proc_b", 'R', 3000, 0),
    });
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    // Find each process
    const Domain::ProcessSnapshot* snapA = nullptr;
    const Domain::ProcessSnapshot* snapB = nullptr;
    for (const auto& s : snaps)
    {
        if (s.pid == 100)
            snapA = &s;
        if (s.pid == 200)
            snapB = &s;
    }

    ASSERT_NE(snapA, nullptr);
    ASSERT_NE(snapB, nullptr);
    EXPECT_DOUBLE_EQ(snapA->cpuPercent, 0.5);
    EXPECT_DOUBLE_EQ(snapB->cpuPercent, 1.0);
}

TEST(ProcessModelTest, CpuPercentZeroWhenNoDelta)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "idle_proc", 'S', 1000, 500)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Second sample: process hasn't used any more CPU
    rawProbe->setCounters({makeCounter(100, "idle_proc", 'S', 1000, 500)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 0.0);
}

TEST(ProcessModelTest, CpuPercentZeroWhenTotalCpuDeltaIsZero)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Second sample with same total CPU time (shouldn't happen in practice)
    rawProbe->setCounters({makeCounter(100, "test_proc", 'R', 2000, 1000)});
    rawProbe->setTotalCpuTime(100000); // Same as before
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 0.0); // Division by zero avoided
}

TEST(ProcessModelTest, HighCpuPercentageCalculation)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "busy_proc", 'R', 0, 0)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Process uses 50% of total CPU delta
    // Delta = 50000, Total = 100000
    // CPU% = 50%
    rawProbe->setCounters({makeCounter(100, "busy_proc", 'R', 50000, 0)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 50.0);
}

// =============================================================================
// PID Reuse / Unique Key Tests
// =============================================================================

TEST(ProcessModelTest, NewProcessWithSamePidGetsZeroCpu)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Original process PID 100, startTime 1000
    rawProbe->setCounters({makeCounter(100, "original", 'R', 10000, 5000, /*startTime*/ 1000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // New process reuses PID 100 but has different startTime
    rawProbe->setCounters({makeCounter(100, "new_proc", 'R', 100, 50, /*startTime*/ 2000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].name, "new_proc");
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 0.0); // No valid previous data
}

TEST(ProcessModelTest, SameProcessRetainsCpuHistory)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Process with consistent startTime
    rawProbe->setCounters({makeCounter(100, "persistent", 'R', 1000, 500, /*startTime*/ 1000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Same process (same startTime) with more CPU usage
    rawProbe->setCounters({makeCounter(100, "persistent", 'R', 2000, 1000, /*startTime*/ 1000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuPercent, 1.5); // History preserved
}

TEST(ProcessModelTest, UniqueKeyIsConsistentForSameProcess)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps1 = model.snapshots();

    // Refresh with same process
    rawProbe->setCounters({makeCounter(100, "test", 'R', 2000, 0, 5000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps2 = model.snapshots();

    EXPECT_EQ(snaps1[0].uniqueKey, snaps2[0].uniqueKey);
}

TEST(ProcessModelTest, UniqueKeyDiffersForPidReuse)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "proc_v1", 'R', 1000, 0, /*startTime*/ 1000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps1 = model.snapshots();

    // New process with same PID but different start time
    rawProbe->setCounters({makeCounter(100, "proc_v2", 'R', 100, 0, /*startTime*/ 2000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps2 = model.snapshots();

    EXPECT_NE(snaps1[0].uniqueKey, snaps2[0].uniqueKey);
}

// =============================================================================
// State Translation Tests
// =============================================================================

TEST(ProcessModelTest, StateTranslationRunning)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'R', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Running");
}

TEST(ProcessModelTest, StateTranslationSleeping)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'S', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Sleeping");
}

TEST(ProcessModelTest, StateTranslationDiskSleep)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'D', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Disk Sleep");
}

TEST(ProcessModelTest, StateTranslationZombie)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'Z', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Zombie");
}

TEST(ProcessModelTest, StateTranslationStopped)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'T', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Stopped");
}

TEST(ProcessModelTest, StateTranslationUnknown)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", '?', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Unknown");
}

TEST(ProcessModelTest, StateTranslationTracing)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "debugged_proc", 't', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Tracing");
}

TEST(ProcessModelTest, StateTranslationDead)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "dead_proc", 'X', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Dead");
}

TEST(ProcessModelTest, StateTranslationIdle)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "idle_kernel_thread", 'I', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Idle");
}

// =============================================================================
// Snapshot Data Mapping Tests
// =============================================================================

TEST(ProcessModelTest, SnapshotContainsAllFields)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters c;
    c.pid = 12345;
    c.parentPid = 100;
    c.name = "my_process";
    c.state = 'S';
    c.userTime = 1000;
    c.systemTime = 500;
    c.startTimeTicks = 9999;
    c.rssBytes = 1024 * 1024 * 50; // 50 MB
    c.virtualBytes = 1024 * 1024 * 200;
    c.threadCount = 4;

    probe->setCounters({c});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    const auto& snap = snaps[0];
    EXPECT_EQ(snap.pid, 12345);
    EXPECT_EQ(snap.parentPid, 100);
    EXPECT_EQ(snap.name, "my_process");
    EXPECT_EQ(snap.displayState, "Sleeping");
    EXPECT_EQ(snap.memoryBytes, 1024 * 1024 * 50);
    EXPECT_EQ(snap.virtualBytes, 1024 * 1024 * 200);
    EXPECT_EQ(snap.threadCount, 4);
    EXPECT_NE(snap.uniqueKey, 0);
}

TEST(ProcessModelTest, PageFaultsAreCopiedToSnapshot)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters c;
    c.pid = 12345;
    c.parentPid = 100;
    c.name = "test_process";
    c.state = 'R';
    c.userTime = 1000;
    c.systemTime = 500;
    c.startTimeTicks = 9999;
    c.rssBytes = 1024 * 1024;
    c.pageFaultCount = 123456; // Set page fault count

    probe->setCounters({c});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    const auto& snap = snaps[0];
    EXPECT_EQ(snap.pageFaults, 123456); // Verify page faults are copied correctly
}

TEST(ProcessModelTest, PageFaultsDefaultToZeroWhenNotSet)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters c;
    c.pid = 12345;
    c.parentPid = 100;
    c.name = "test_process";
    c.state = 'R';
    c.userTime = 1000;
    c.systemTime = 500;
    c.startTimeTicks = 9999;
    c.rssBytes = 1024 * 1024;
    // pageFaultCount not explicitly set, should default to 0

    probe->setCounters({c});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    const auto& snap = snaps[0];
    EXPECT_EQ(snap.pageFaults, 0); // Default value should be 0
}

TEST(ProcessModelTest, ProcessCountReturnsCorrectValue)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({
        makeCounter(1, "proc1", 'R', 0, 0),
        makeCounter(2, "proc2", 'S', 0, 0),
        makeCounter(3, "proc3", 'S', 0, 0),
    });
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.processCount(), 3);
}

// =============================================================================
// Process Lifecycle Tests
// =============================================================================

TEST(ProcessModelTest, ProcessDisappearingIsHandled)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Two processes
    rawProbe->setCounters({
        makeCounter(100, "proc_a", 'R', 1000, 0),
        makeCounter(200, "proc_b", 'R', 2000, 0),
    });
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.processCount(), 2);

    // proc_b terminates
    rawProbe->setCounters({
        makeCounter(100, "proc_a", 'R', 1500, 0),
    });
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    EXPECT_EQ(model.processCount(), 1);
    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].pid, 100);
}

TEST(ProcessModelTest, NewProcessAppearingIsHandled)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // One process
    rawProbe->setCounters({makeCounter(100, "proc_a", 'R', 1000, 0)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.processCount(), 1);

    // New process appears
    rawProbe->setCounters({
        makeCounter(100, "proc_a", 'R', 1500, 0),
        makeCounter(200, "new_proc", 'R', 100, 0, /*startTime*/ 2000),
    });
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    EXPECT_EQ(model.processCount(), 2);

    auto snaps = model.snapshots();
    bool foundNew = false;
    for (const auto& s : snaps)
    {
        if (s.pid == 200)
        {
            foundNew = true;
            EXPECT_EQ(s.name, "new_proc");
            EXPECT_DOUBLE_EQ(s.cpuPercent, 0.0); // New process, no history
        }
    }
    EXPECT_TRUE(foundNew);
}

// =============================================================================
// updateFromCounters Tests (Background Sampler Interface)
// =============================================================================

TEST(ProcessModelTest, UpdateFromCountersWorks)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    // Direct update without using the probe
    std::vector<Platform::ProcessCounters> counters = {
        makeCounter(100, "external_proc", 'R', 1000, 500),
    };
    model.updateFromCounters(counters, 100000);

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].pid, 100);
    EXPECT_EQ(snaps[0].name, "external_proc");
}

TEST(ProcessModelTest, UpdateFromCountersCalculatesCpuDelta)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    // First update
    std::vector<Platform::ProcessCounters> counters1 = {
        makeCounter(100, "proc", 'R', 1000, 500),
    };
    model.updateFromCounters(counters1, 100000);

    // Second update with CPU usage
    std::vector<Platform::ProcessCounters> counters2 = {
        makeCounter(100, "proc", 'R', 2000, 1000),
    };
    model.updateFromCounters(counters2, 200000);

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 1.5);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(ProcessModelTest, ConcurrentSnapshotAccess)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({
        makeCounter(1, "proc1", 'R', 1000, 0),
        makeCounter(2, "proc2", 'S', 2000, 0),
    });
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Concurrent reads should not crash
    std::vector<std::thread> readers;
    for (int i = 0; i < 10; ++i)
    {
        readers.emplace_back(
            [&model]()
            {
                for (int j = 0; j < 100; ++j)
                {
                    auto snaps = model.snapshots();
                    auto count = model.processCount();
                    (void) snaps;
                    (void) count;
                }
            });
    }

    for (auto& t : readers)
    {
        t.join();
    }

    EXPECT_EQ(model.processCount(), 2);
}

TEST(ProcessModelTest, ConcurrentRefreshAndRead)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(1, "proc", 'R', 1000, 0)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));

    std::atomic<bool> done{false};

    // Writer thread
    std::thread writer(
        [&]()
        {
            for (uint64_t i = 0; i < 100 && !done; ++i)
            {
                rawProbe->setCounters({makeCounter(1, "proc", 'R', 1000 + (i * 10), 0)});
                rawProbe->setTotalCpuTime(100000 + (i * 1000));
                model.refresh();
            }
            done = true;
        });

    // Reader threads
    std::vector<std::thread> readers;
    for (int i = 0; i < 5; ++i)
    {
        readers.emplace_back(
            [&model, &done]()
            {
                while (!done)
                {
                    auto snaps = model.snapshots();
                    auto count = model.processCount();
                    (void) snaps;
                    (void) count;
                }
            });
    }

    writer.join();
    for (auto& t : readers)
    {
        t.join();
    }

    // Model should be in a consistent state
    EXPECT_GE(model.processCount(), 0);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(ProcessModelTest, EmptyCountersResultInEmptySnapshots)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.processCount(), 0);
    EXPECT_TRUE(model.snapshots().empty());
}

TEST(ProcessModelTest, LargeNumberOfProcesses)
{
    auto probe = std::make_unique<MockProcessProbe>();

    std::vector<Platform::ProcessCounters> counters;
    for (int32_t i = 0; i < 1000; ++i)
    {
        counters.push_back(
            makeCounter(i + 1, "proc_" + std::to_string(i), 'S', static_cast<uint64_t>(i) * 100, static_cast<uint64_t>(i) * 50));
    }
    probe->setCounters(counters);
    probe->setTotalCpuTime(10000000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.processCount(), 1000);
}

TEST(ProcessModelTest, ProcessWithZeroStartTime)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "kernel_thread", 'S', 1000, 500, /*startTime*/ 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // Should still work - uniqueKey based on hash of 0 is valid
    EXPECT_NE(snaps[0].uniqueKey, 0);
}

TEST(ProcessModelTest, IntegerOverflowInCpuCounters)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Start with very high values near overflow
    constexpr uint64_t nearMax = std::numeric_limits<uint64_t>::max() - OVERFLOW_TEST_MARGIN;
    rawProbe->setCounters({makeCounter(100, "overflow_proc", 'R', nearMax, 5000)});
    rawProbe->setTotalCpuTime(nearMax * 2);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Counter wraps around (overflow scenario)
    // In practice, OS counters may wrap, but our delta calculation should handle it gracefully
    // by treating the new value as a new baseline
    rawProbe->setCounters({makeCounter(100, "overflow_proc", 'R', 1000, 500)});
    rawProbe->setTotalCpuTime(nearMax * 2 + 100000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // CPU% should be 0 or minimal because the counter appears to have decreased
    // (which our implementation treats as a new process baseline)
    EXPECT_GE(snaps[0].cpuPercent, 0.0);
    // CPU% is calculated as (processDelta / totalCpuDelta) * 100, so it should be <= 100%
    // regardless of core count (totalCpuDelta includes all cores)
    EXPECT_LE(snaps[0].cpuPercent, 100.0);
}

TEST(ProcessModelTest, ExtremeValuesMaxUint64)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters c;
    c.pid = std::numeric_limits<int32_t>::max();
    c.parentPid = std::numeric_limits<int32_t>::max() - 1;
    c.name = "extreme_proc";
    c.state = 'R';
    c.userTime = std::numeric_limits<uint64_t>::max();
    c.systemTime = std::numeric_limits<uint64_t>::max();
    c.startTimeTicks = std::numeric_limits<uint64_t>::max();
    c.rssBytes = std::numeric_limits<uint64_t>::max();
    c.virtualBytes = std::numeric_limits<uint64_t>::max();
    c.threadCount = std::numeric_limits<int32_t>::max();

    probe->setCounters({c});
    probe->setTotalCpuTime(std::numeric_limits<uint64_t>::max());

    Domain::ProcessModel model(std::move(probe));

    // Should not crash or produce undefined behavior
    EXPECT_NO_THROW({ model.refresh(); });

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Verify extreme values are preserved
    EXPECT_EQ(snaps[0].pid, std::numeric_limits<int32_t>::max());
    EXPECT_EQ(snaps[0].parentPid, std::numeric_limits<int32_t>::max() - 1);
    EXPECT_EQ(snaps[0].name, "extreme_proc");
    EXPECT_EQ(snaps[0].memoryBytes, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(snaps[0].virtualBytes, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(snaps[0].threadCount, std::numeric_limits<int32_t>::max());

    // CPU% should be valid (0.0 on first sample, no previous data)
    EXPECT_GE(snaps[0].cpuPercent, 0.0);
    EXPECT_LE(snaps[0].cpuPercent, 100.0);

    // UniqueKey should be valid (non-zero hash)
    EXPECT_NE(snaps[0].uniqueKey, 0);
}

// =============================================================================
// Builder Pattern Tests
// =============================================================================

TEST(ProcessModelTest, BuilderPatternSimpleSetup)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(123, "test_process").withCpuTime(123, 1000, 500).withMemory(123, 4096 * 1024).withState(123, 'R');
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].pid, 123);
    EXPECT_EQ(snaps[0].name, "test_process");
    EXPECT_EQ(snaps[0].displayState, "Running");
    EXPECT_EQ(snaps[0].memoryBytes, 4096 * 1024);
}

TEST(ProcessModelTest, BuilderPatternMultipleProcesses)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(100, "proc_a")
        .withState(100, 'R')
        .withProcess(200, "proc_b")
        .withState(200, 'S')
        .withProcess(300, "proc_c")
        .withState(300, 'D');
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.processCount(), 3);
}

TEST(ProcessModelTest, BuilderPatternBackwardCompatibility)
{
    // Old style still works
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(123, "legacy_proc", 'R', 1000, 500)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].pid, 123);
    EXPECT_EQ(snaps[0].name, "legacy_proc");
}

// CPU Affinity Tests
// =============================================================================

TEST(ProcessModelTest, CpuAffinityIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "affinity_test", 'R', 1000, 500);
    counter.cpuAffinityMask = 0x0F; // Cores 0-3
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuAffinityMask, 0x0F);
}

TEST(ProcessModelTest, CpuAffinityZeroWhenNotAvailable)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "no_affinity", 'R', 1000, 500);
    counter.cpuAffinityMask = 0; // Not available
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuAffinityMask, 0);
}

TEST(ProcessModelTest, CpuAffinityAllCores)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "all_cores", 'R', 1000, 500);
    counter.cpuAffinityMask = 0xFFFFFFFFFFFFFFFF; // All 64 cores
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuAffinityMask, 0xFFFFFFFFFFFFFFFF);
}

// =============================================================================
// Network Rate Calculation Tests
// =============================================================================

TEST(ProcessModelTest, NetworkRatesZeroOnFirstRefresh)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(100, "network_proc").withNetworkCounters(100, 1000, 2000);
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].netSentBytesPerSec, 0.0);     // No previous data
    EXPECT_DOUBLE_EQ(snaps[0].netReceivedBytesPerSec, 0.0); // No previous data
}

TEST(ProcessModelTest, NetworkRatesCalculatedFromDeltas)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample: 1000 sent, 2000 received
    rawProbe->withProcess(100, "network_proc").withNetworkCounters(100, 1000, 2000);
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Wait a bit to ensure time delta is non-zero
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Second sample: 2000 sent (+1000), 4000 received (+2000)
    // Rates depend on time delta (should be ~100ms = 0.1s)
    rawProbe->setCounters({});
    rawProbe->withProcess(100, "network_proc").withNetworkCounters(100, 2000, 4000);
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // With ~0.1s delta: 1000 bytes / 0.1s = ~10000 B/s, 2000 bytes / 0.1s = ~20000 B/s
    // Allow some tolerance for timing variations
    EXPECT_GT(snaps[0].netSentBytesPerSec, 5000.0);
    EXPECT_LT(snaps[0].netSentBytesPerSec, 20000.0);
    EXPECT_GT(snaps[0].netReceivedBytesPerSec, 10000.0);
    EXPECT_LT(snaps[0].netReceivedBytesPerSec, 40000.0);
}

TEST(ProcessModelTest, NetworkRatesHandleCounterDecrease)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->withProcess(100, "proc").withNetworkCounters(100, 2000, 4000);
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Counter decreased (process restarted or counter wrapped)
    rawProbe->setCounters({});
    rawProbe->withProcess(100, "proc").withNetworkCounters(100, 500, 1000);
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // Should be 0 (no rate calculated when counter decreases)
    EXPECT_DOUBLE_EQ(snaps[0].netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snaps[0].netReceivedBytesPerSec, 0.0);
}
// =============================================================================
// I/O Rate Calculation Tests
// =============================================================================
TEST(ProcessModelTest, FirstRefreshShowsZeroIoRates)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters c = makeCounter(100, "test_proc", 'R', 1000, 500);
    c.readBytes = 1024 * 1024; // 1 MB
    c.writeBytes = 512 * 1024; // 512 KB

    probe->setCounters({c});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 0.0); // No previous data
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 0.0);
}

TEST(ProcessModelTest, IoRatesCalculatedFromDeltas)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample: process has read 1 MB, written 512 KB
    Platform::ProcessCounters c1 = makeCounter(100, "test_proc", 'R', 1000, 500);
    c1.readBytes = 1024 * 1024;
    c1.writeBytes = 512 * 1024;

    rawProbe->setCounters({c1});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Sleep a bit to ensure time delta
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Second sample: process has read 3 MB total (delta = 2 MB), written 1.5 MB total (delta = 1 MB)
    Platform::ProcessCounters c2 = makeCounter(100, "test_proc", 'R', 2000, 1000);
    c2.readBytes = 3 * 1024 * 1024;
    c2.writeBytes = 1536 * 1024;

    rawProbe->setCounters({c2});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Should have positive rates (exact value depends on elapsed time)
    EXPECT_GT(snaps[0].ioReadBytesPerSec, 0.0);
    EXPECT_GT(snaps[0].ioWriteBytesPerSec, 0.0);

    // Read delta = 2 MB, write delta = 1 MB
    // With ~100ms elapsed, we expect roughly:
    // Read: 2 MB / 0.1s = ~20 MB/s
    // Write: 1 MB / 0.1s = ~10 MB/s
    // Allow wide tolerance for timing variations
    EXPECT_GT(snaps[0].ioReadBytesPerSec, 1024.0 * 1024.0); // At least 1 MB/s
    EXPECT_GT(snaps[0].ioWriteBytesPerSec, 512.0 * 1024.0); // At least 512 KB/s
}

TEST(ProcessModelTest, IoRatesHandleNoActivity)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    Platform::ProcessCounters c1 = makeCounter(100, "idle_proc", 'S', 1000, 500);
    c1.readBytes = 1024 * 1024;
    c1.writeBytes = 512 * 1024;

    rawProbe->setCounters({c1});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Second sample: no change in I/O counters
    Platform::ProcessCounters c2 = makeCounter(100, "idle_proc", 'S', 1000, 500);
    c2.readBytes = 1024 * 1024; // Same as before
    c2.writeBytes = 512 * 1024; // Same as before

    rawProbe->setCounters({c2});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 0.0);
}

TEST(ProcessModelTest, IoRatesForMultipleProcesses)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample: two processes
    Platform::ProcessCounters c1a = makeCounter(100, "proc_a", 'R', 1000, 0);
    c1a.readBytes = 1024 * 1024;
    c1a.writeBytes = 512 * 1024;

    Platform::ProcessCounters c1b = makeCounter(200, "proc_b", 'R', 2000, 0);
    c1b.readBytes = 2 * 1024 * 1024;
    c1b.writeBytes = 1024 * 1024;

    rawProbe->setCounters({c1a, c1b});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Second sample: proc_a read 1 MB more, proc_b wrote 2 MB more
    Platform::ProcessCounters c2a = makeCounter(100, "proc_a", 'R', 1500, 0);
    c2a.readBytes = 2 * 1024 * 1024; // +1 MB
    c2a.writeBytes = 512 * 1024;     // No change

    Platform::ProcessCounters c2b = makeCounter(200, "proc_b", 'R', 3000, 0);
    c2b.readBytes = 2 * 1024 * 1024;  // No change
    c2b.writeBytes = 3 * 1024 * 1024; // +2 MB

    rawProbe->setCounters({c2a, c2b});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    // Find each process
    const Domain::ProcessSnapshot* snapA = nullptr;
    const Domain::ProcessSnapshot* snapB = nullptr;
    for (const auto& s : snaps)
    {
        if (s.pid == 100)
            snapA = &s;
        if (s.pid == 200)
            snapB = &s;
    }

    ASSERT_NE(snapA, nullptr);
    ASSERT_NE(snapB, nullptr);

    // proc_a should have read rate > 0, write rate = 0
    EXPECT_GT(snapA->ioReadBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snapA->ioWriteBytesPerSec, 0.0);

    // proc_b should have write rate > 0, read rate = 0
    EXPECT_DOUBLE_EQ(snapB->ioReadBytesPerSec, 0.0);
    EXPECT_GT(snapB->ioWriteBytesPerSec, 0.0);
}

TEST(ProcessModelTest, IoRatesHandleCounterWrapAround)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample with high counter values
    Platform::ProcessCounters c1 = makeCounter(100, "wrap_proc", 'R', 1000, 500);
    c1.readBytes = 1000 * 1024 * 1024; // 1000 MB
    c1.writeBytes = 500 * 1024 * 1024; // 500 MB

    rawProbe->setCounters({c1});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Second sample: counter appears to have decreased (wraparound or reset)
    // Our implementation should handle this gracefully by showing 0 rate
    Platform::ProcessCounters c2 = makeCounter(100, "wrap_proc", 'R', 2000, 1000);
    c2.readBytes = 100 * 1024 * 1024; // Less than before
    c2.writeBytes = 50 * 1024 * 1024; // Less than before

    rawProbe->setCounters({c2});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Should handle gracefully (no negative rates)
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 0.0);
}

TEST(ProcessModelTest, NewProcessWithSamePidGetsZeroIoRates)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Original process PID 100, startTime 1000
    Platform::ProcessCounters c1 = makeCounter(100, "original", 'R', 10000, 5000, /*startTime*/ 1000);
    c1.readBytes = 1024 * 1024;
    c1.writeBytes = 512 * 1024;

    rawProbe->setCounters({c1});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // New process reuses PID 100 but has different startTime
    Platform::ProcessCounters c2 = makeCounter(100, "new_proc", 'R', 100, 50, /*startTime*/ 2000);
    c2.readBytes = 2 * 1024 * 1024;
    c2.writeBytes = 1024 * 1024;

    rawProbe->setCounters({c2});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].name, "new_proc");
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 0.0);  // No valid previous data
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 0.0);
}
