/// @file test_SystemModel.cpp
/// @brief Comprehensive tests for Domain::SystemModel
///
/// Tests cover:
/// - Memory metrics calculations
/// - CPU percentage calculations from counter deltas
/// - Swap metrics
/// - History tracking
/// - Thread-safe operations
/// - Per-core CPU tracking

#include "Domain/SystemModel.h"
#include "Platform/ISystemProbe.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

/// Mock probe that returns controlled test data.
class MockSystemProbe : public Platform::ISystemProbe
{
  public:
    void setCounters(Platform::SystemCounters counters)
    {
        m_Counters = std::move(counters);
    }

    void setCapabilities(Platform::SystemCapabilities caps)
    {
        m_Capabilities = caps;
    }

    [[nodiscard]] Platform::SystemCounters read() override
    {
        return m_Counters;
    }

    [[nodiscard]] Platform::SystemCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

    [[nodiscard]] long ticksPerSecond() const override
    {
        return 100;
    }

  private:
    Platform::SystemCounters m_Counters;
    Platform::SystemCapabilities m_Capabilities;
};

namespace
{

/// Helper to create CPU counters with specific values.
Platform::CpuCounters makeCpuCounters(uint64_t user, uint64_t nice, uint64_t system, uint64_t idle, uint64_t iowait = 0, uint64_t steal = 0)
{
    Platform::CpuCounters c;
    c.user = user;
    c.nice = nice;
    c.system = system;
    c.idle = idle;
    c.iowait = iowait;
    c.steal = steal;
    return c;
}

/// Helper to create memory counters.
Platform::MemoryCounters makeMemoryCounters(uint64_t total,
                                            uint64_t available,
                                            uint64_t free = 0,
                                            uint64_t cached = 0,
                                            uint64_t buffers = 0,
                                            uint64_t swapTotal = 0,
                                            uint64_t swapFree = 0)
{
    Platform::MemoryCounters m;
    m.totalBytes = total;
    m.availableBytes = available;
    m.freeBytes = free;
    m.cachedBytes = cached;
    m.buffersBytes = buffers;
    m.swapTotalBytes = swapTotal;
    m.swapFreeBytes = swapFree;
    return m;
}

/// Helper to create a full system counters struct.
Platform::SystemCounters makeSystemCounters(Platform::CpuCounters cpu,
                                            Platform::MemoryCounters memory,
                                            uint64_t uptime = 0,
                                            std::vector<Platform::CpuCounters> perCore = {})
{
    Platform::SystemCounters s;
    s.cpuTotal = cpu;
    s.memory = memory;
    s.uptimeSeconds = uptime;
    s.cpuPerCore = std::move(perCore);
    return s;
}

} // namespace

// =============================================================================
// Construction Tests
// =============================================================================

TEST(SystemModelTest, ConstructWithValidProbe)
{
    auto probe = std::make_unique<MockSystemProbe>();
    Domain::SystemModel model(std::move(probe));

    auto snap = model.snapshot();
    EXPECT_EQ(snap.coreCount, 0);
    EXPECT_EQ(snap.memoryTotalBytes, 0);
}

TEST(SystemModelTest, ConstructWithNullProbeDoesNotCrash)
{
    Domain::SystemModel model(nullptr);
    model.refresh(); // Should not crash

    auto snap = model.snapshot();
    EXPECT_EQ(snap.coreCount, 0);
}

TEST(SystemModelTest, CapabilitiesAreExposedFromProbe)
{
    auto probe = std::make_unique<MockSystemProbe>();
    Platform::SystemCapabilities caps;
    caps.hasPerCoreCpu = true;
    caps.hasSwap = true;
    caps.hasIoWait = true;
    probe->setCapabilities(caps);

    Domain::SystemModel model(std::move(probe));

    const auto& modelCaps = model.capabilities();
    EXPECT_TRUE(modelCaps.hasPerCoreCpu);
    EXPECT_TRUE(modelCaps.hasSwap);
    EXPECT_TRUE(modelCaps.hasIoWait);
}

// =============================================================================
// Memory Metrics Tests
// =============================================================================

TEST(SystemModelTest, MemoryMetricsCalculatedCorrectly)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // 16 GB total, 8 GB available
    auto mem = makeMemoryCounters(16ULL * 1024 * 1024 * 1024, // 16 GB total
                                  8ULL * 1024 * 1024 * 1024,  // 8 GB available
                                  4ULL * 1024 * 1024 * 1024,  // 4 GB free
                                  2ULL * 1024 * 1024 * 1024,  // 2 GB cached
                                  1ULL * 1024 * 1024 * 1024   // 1 GB buffers
    );
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), mem));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_EQ(snap.memoryTotalBytes, 16ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(snap.memoryAvailableBytes, 8ULL * 1024 * 1024 * 1024);
    // Used = Total - Available = 16 GB - 8 GB = 8 GB
    EXPECT_EQ(snap.memoryUsedBytes, 8ULL * 1024 * 1024 * 1024);
    EXPECT_DOUBLE_EQ(snap.memoryUsedPercent, 50.0);
}

TEST(SystemModelTest, MemoryPercentageEdgeCases)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // 100% used (available = 0)
    auto mem = makeMemoryCounters(1024 * 1024, 0);
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), mem));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.memoryUsedPercent, 100.0);
    EXPECT_EQ(snap.memoryUsedBytes, 1024 * 1024);
}

TEST(SystemModelTest, MemoryFallbackWhenNoAvailable)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // Old kernel without MemAvailable (available = 0)
    // total=100, free=20, cached=30, buffers=10
    // used = 100 - 20 - 30 - 10 = 40
    auto mem = makeMemoryCounters(100, 0, 20, 30, 10);
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), mem));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_EQ(snap.memoryUsedBytes, 40);
    EXPECT_DOUBLE_EQ(snap.memoryUsedPercent, 40.0);
}

// =============================================================================
// Swap Metrics Tests
// =============================================================================

TEST(SystemModelTest, SwapMetricsCalculatedCorrectly)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // 4 GB swap total, 3 GB free -> 1 GB used
    auto mem = makeMemoryCounters(8ULL * 1024 * 1024 * 1024, // 8 GB RAM
                                  4ULL * 1024 * 1024 * 1024, // 4 GB available
                                  0,
                                  0,
                                  0,                         // free, cached, buffers
                                  4ULL * 1024 * 1024 * 1024, // 4 GB swap total
                                  3ULL * 1024 * 1024 * 1024  // 3 GB swap free
    );
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), mem));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_EQ(snap.swapTotalBytes, 4ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(snap.swapUsedBytes, 1ULL * 1024 * 1024 * 1024);
    EXPECT_DOUBLE_EQ(snap.swapUsedPercent, 25.0);
}

TEST(SystemModelTest, SwapZeroWhenNoSwap)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    auto mem = makeMemoryCounters(8ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    // swapTotal and swapFree default to 0
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), mem));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_EQ(snap.swapTotalBytes, 0);
    EXPECT_EQ(snap.swapUsedBytes, 0);
    EXPECT_DOUBLE_EQ(snap.swapUsedPercent, 0.0);
}

// =============================================================================
// CPU Percentage Calculation Tests
// =============================================================================

TEST(SystemModelTest, FirstRefreshShowsZeroCpu)
{
    auto probe = std::make_unique<MockSystemProbe>();
    probe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    // First sample has no delta - CPU should be 0
    EXPECT_DOUBLE_EQ(snap.cpuTotal.totalPercent, 0.0);
}

TEST(SystemModelTest, CpuPercentCalculatedFromDeltas)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample: user=1000, system=500, idle=8500 (total=10000)
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Second sample: user=2000, system=1000, idle=17000 (total=20000)
    // Delta: user=1000, system=500, idle=8500 (total delta=10000)
    // idle% = 8500/10000 = 85%
    // total% = 100% - 85% = 15%
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(2000, 0, 1000, 17000), makeMemoryCounters(1024, 512)));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.cpuTotal.totalPercent, 15.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.idlePercent, 85.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.userPercent, 10.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.systemPercent, 5.0);
}

TEST(SystemModelTest, CpuPercentHighUsage)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 1000, 8000), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Second sample: 90% busy (idle only 10%)
    // Delta: user=4500, system=4500, idle=1000 (total=10000)
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(5500, 0, 5500, 9000), makeMemoryCounters(1024, 512)));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.cpuTotal.totalPercent, 90.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.idlePercent, 10.0);
}

TEST(SystemModelTest, CpuPercentWithIoWaitAndSteal)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8000, 300, 200), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Second sample with iowait and steal
    // Delta: user=1000, system=500, idle=7000, iowait=1000, steal=500 (total=10000)
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(2000, 0, 1000, 15000, 1300, 700), makeMemoryCounters(1024, 512)));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.cpuTotal.iowaitPercent, 10.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.stealPercent, 5.0);
}

TEST(SystemModelTest, CpuPercentClampsToValidRange)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 10000), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Second sample: 100% idle
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 20000), makeMemoryCounters(1024, 512)));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.cpuTotal.totalPercent, 0.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.idlePercent, 100.0);
}

// =============================================================================
// Per-Core CPU Tests
// =============================================================================

TEST(SystemModelTest, PerCoreCpuTracking)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    std::vector<Platform::CpuCounters> cores1 = {
        makeCpuCounters(1000, 0, 500, 8500), // Core 0
        makeCpuCounters(2000, 0, 1000, 7000) // Core 1
    };
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(3000, 0, 1500, 15500), makeMemoryCounters(1024, 512), 0, cores1));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Second sample
    std::vector<Platform::CpuCounters> cores2 = {
        makeCpuCounters(2000, 0, 1000, 17000), // Core 0: 15% busy
        makeCpuCounters(4000, 0, 2000, 14000)  // Core 1: 30% busy
    };
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(6000, 0, 3000, 31000), makeMemoryCounters(1024, 512), 0, cores2));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_EQ(snap.coreCount, 2);
    ASSERT_EQ(snap.cpuPerCore.size(), 2);
    EXPECT_DOUBLE_EQ(snap.cpuPerCore[0].totalPercent, 15.0);
    EXPECT_DOUBLE_EQ(snap.cpuPerCore[1].totalPercent, 30.0);
}

// =============================================================================
// History Tracking Tests
// =============================================================================

TEST(SystemModelTest, HistoryTracksMultipleSamples)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    Domain::SystemModel model(std::move(probe));

    // Sample 0 (baseline): total=10000, all idle
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 10000), makeMemoryCounters(1000, 500) // 50% memory
                                             ));
    model.refresh();

    // Sample 1: delta: user=1000, sys=1000, idle=8000 (total=10000) -> 20% CPU
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 1000, 18000), makeMemoryCounters(1000, 400) // 60% memory
                                             ));
    model.refresh();

    // Sample 2: delta: user=2000, sys=1000, idle=7000 (total=10000) -> 30% CPU
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(3000, 0, 2000, 25000), makeMemoryCounters(1000, 300) // 70% memory
                                             ));
    model.refresh();

    auto cpuHist = model.cpuHistory();
    auto memHist = model.memoryHistory();

    EXPECT_EQ(cpuHist.size(), 2);
    EXPECT_EQ(memHist.size(), 2);

    // History returns oldest to newest
    EXPECT_FLOAT_EQ(cpuHist[0], 20.0F);
    EXPECT_FLOAT_EQ(cpuHist[1], 30.0F);
    EXPECT_FLOAT_EQ(memHist[0], 60.0F);
    EXPECT_FLOAT_EQ(memHist[1], 70.0F);
}

TEST(SystemModelTest, HistoryInitiallyEmpty)
{
    auto probe = std::make_unique<MockSystemProbe>();
    Domain::SystemModel model(std::move(probe));

    EXPECT_TRUE(model.cpuHistory().empty());
    EXPECT_TRUE(model.memoryHistory().empty());
    EXPECT_TRUE(model.swapHistory().empty());
}

TEST(SystemModelTest, PerCoreHistoryTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    std::vector<Platform::CpuCounters> cores1 = {makeCpuCounters(0, 0, 0, 10000)};
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 10000), makeMemoryCounters(1024, 512), 0, cores1));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Second sample
    std::vector<Platform::CpuCounters> cores2 = {makeCpuCounters(2500, 0, 2500, 15000)};
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(2500, 0, 2500, 15000), makeMemoryCounters(1024, 512), 0, cores2));
    model.refresh();

    auto perCoreHist = model.perCoreHistory();
    ASSERT_EQ(perCoreHist.size(), 1);
    EXPECT_EQ(perCoreHist[0].size(), 1);
    EXPECT_FLOAT_EQ(perCoreHist[0][0], 50.0F); // 50% CPU on core 0
}

// =============================================================================
// updateFromCounters Tests
// =============================================================================

TEST(SystemModelTest, UpdateFromCountersWorks)
{
    auto probe = std::make_unique<MockSystemProbe>();
    Domain::SystemModel model(std::move(probe));

    auto counters = makeSystemCounters(
        makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(16ULL * 1024 * 1024 * 1024, 8ULL * 1024 * 1024 * 1024), 12345);
    model.updateFromCounters(counters);

    auto snap = model.snapshot();
    EXPECT_EQ(snap.uptimeSeconds, 12345);
    EXPECT_EQ(snap.memoryTotalBytes, 16ULL * 1024 * 1024 * 1024);
}

TEST(SystemModelTest, UpdateFromCountersCalculatesDelta)
{
    auto probe = std::make_unique<MockSystemProbe>();
    Domain::SystemModel model(std::move(probe));

    // First update
    model.updateFromCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(1024, 512)));

    // Second update
    model.updateFromCounters(makeSystemCounters(makeCpuCounters(2000, 0, 1000, 17000), makeMemoryCounters(1024, 512)));

    auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.cpuTotal.totalPercent, 15.0);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(SystemModelTest, ConcurrentSnapshotAccess)
{
    auto probe = std::make_unique<MockSystemProbe>();
    probe->setCounters(
        makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(8ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    std::vector<std::thread> readers;
    for (int i = 0; i < 10; ++i)
    {
        readers.emplace_back(
            [&model]()
            {
                for (int j = 0; j < 100; ++j)
                {
                    auto snap = model.snapshot();
                    auto cpuHist = model.cpuHistory();
                    auto memHist = model.memoryHistory();
                    (void) snap;
                    (void) cpuHist;
                    (void) memHist;
                }
            });
    }

    for (auto& t : readers)
    {
        t.join();
    }

    // Model should be in a consistent state
    auto snap = model.snapshot();
    EXPECT_EQ(snap.memoryTotalBytes, 8ULL * 1024 * 1024 * 1024);
}

TEST(SystemModelTest, ConcurrentRefreshAndRead)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));

    std::atomic<bool> done{false};

    // Writer thread
    std::thread writer(
        [&]()
        {
            for (uint64_t i = 0; i < 100 && !done; ++i)
            {
                rawProbe->setCounters(
                    makeSystemCounters(makeCpuCounters(1000 + (i * 10), 0, 500, 8500 + (i * 100)), makeMemoryCounters(1024, 512 - i)));
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
                    auto snap = model.snapshot();
                    auto cpuHist = model.cpuHistory();
                    (void) snap;
                    (void) cpuHist;
                }
            });
    }

    writer.join();
    for (auto& t : readers)
    {
        t.join();
    }

    // Model should be in a consistent state
    auto snap = model.snapshot();
    EXPECT_GT(snap.memoryTotalBytes, 0);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(SystemModelTest, ZeroTotalCpuDeltaHandled)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Same counters (no delta) - shouldn't crash
    model.refresh();

    auto snap = model.snapshot();
    // CPU should be 0 when no delta
    EXPECT_DOUBLE_EQ(snap.cpuTotal.totalPercent, 0.0);
}

TEST(SystemModelTest, UptimeTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    probe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(1024, 512), 86400 // 1 day
                                          ));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_EQ(snap.uptimeSeconds, 86400);
}

TEST(SystemModelTest, CoreCountReported)
{
    auto probe = std::make_unique<MockSystemProbe>();

    std::vector<Platform::CpuCounters> cores(8, makeCpuCounters(1000, 0, 500, 8500));
    probe->setCounters(makeSystemCounters(makeCpuCounters(8000, 0, 4000, 68000), makeMemoryCounters(1024, 512), 0, cores));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_EQ(snap.coreCount, 8);
}

TEST(SystemModelTest, HistorySizeConstant)
{
    // Verify the history size constant is accessible
    EXPECT_EQ(Domain::SystemModel::HISTORY_SIZE, 120);
}
