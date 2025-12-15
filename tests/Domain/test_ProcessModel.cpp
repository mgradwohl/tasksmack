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
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

/// Mock probe that returns controlled test data.
/// Defined outside anonymous namespace to work with std::make_unique.
class MockProcessProbe : public Platform::IProcessProbe
{
  public:
    void setCounters(std::vector<Platform::ProcessCounters> counters)
    {
        m_Counters = std::move(counters);
    }

    void setTotalCpuTime(uint64_t time)
    {
        m_TotalCpuTime = time;
    }

    void setCapabilities(Platform::ProcessCapabilities caps)
    {
        m_Capabilities = caps;
    }

    [[nodiscard]] std::vector<Platform::ProcessCounters> enumerate() override
    {
        return m_Counters;
    }

    [[nodiscard]] uint64_t totalCpuTime() const override
    {
        return m_TotalCpuTime;
    }

    [[nodiscard]] Platform::ProcessCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

    [[nodiscard]] long ticksPerSecond() const override
    {
        return 100;
    } // Standard HZ value

  private:
    std::vector<Platform::ProcessCounters> m_Counters;
    uint64_t m_TotalCpuTime = 0;
    Platform::ProcessCapabilities m_Capabilities;
};

namespace
{

/// Helper to create a process counter.
Platform::ProcessCounters makeCounter(int32_t pid,
                                      const std::string& name,
                                      char state,
                                      uint64_t userTime,
                                      uint64_t systemTime,
                                      uint64_t startTime = 1000,
                                      uint64_t rssBytes = 1024 * 1024,
                                      int32_t parentPid = 1)
{
    Platform::ProcessCounters c;
    c.pid = pid;
    c.parentPid = parentPid;
    c.name = name;
    c.state = state;
    c.userTime = userTime;
    c.systemTime = systemTime;
    c.startTimeTicks = startTime;
    c.rssBytes = rssBytes;
    c.virtualBytes = rssBytes * 2;
    c.threadCount = 1;
    return c;
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
