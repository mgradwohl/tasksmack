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

#include "Domain/SamplingConfig.h"
#include "Domain/SystemModel.h"
#include "Mocks/MockProbes.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

// Use shared mock from TestMocks namespace
using TestMocks::makeCpuCounters;
using TestMocks::makeInterfaceCounters;
using TestMocks::makeMemoryCounters;
using TestMocks::makeSystemCounters;
using TestMocks::MockSystemProbe;

// =============================================================================
// Platform::CpuCounters Tests (SystemTypes.h)
// =============================================================================

TEST(CpuCountersTest, TotalCalculatesAllComponents)
{
    Platform::CpuCounters c;
    c.user = 100;
    c.nice = 20;
    c.system = 50;
    c.idle = 800;
    c.iowait = 10;
    c.irq = 5;
    c.softirq = 3;
    c.steal = 7;
    c.guest = 4;
    c.guestNice = 1;

    // total = 100 + 20 + 50 + 800 + 10 + 5 + 3 + 7 + 4 + 1 = 1000
    EXPECT_EQ(c.total(), 1000);
}

TEST(CpuCountersTest, ActiveExcludesIdleAndIowait)
{
    Platform::CpuCounters c;
    c.user = 100;
    c.nice = 20;
    c.system = 50;
    c.idle = 800;  // NOT included in active
    c.iowait = 10; // NOT included in active
    c.irq = 5;
    c.softirq = 3;
    c.steal = 7;
    c.guest = 4;
    c.guestNice = 1;

    // active = 100 + 20 + 50 + 5 + 3 + 7 + 4 + 1 = 190
    // (excludes idle=800 and iowait=10)
    EXPECT_EQ(c.active(), 190);
}

TEST(CpuCountersTest, ActiveWithZeroValues)
{
    Platform::CpuCounters c;
    // All zeros by default
    EXPECT_EQ(c.active(), 0);
}

TEST(CpuCountersTest, TotalWithZeroValues)
{
    Platform::CpuCounters c;
    // All zeros by default
    EXPECT_EQ(c.total(), 0);
}

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

TEST(SystemModelTest, MaxHistorySecondsClamped)
{
    auto probe = std::make_unique<MockSystemProbe>();
    probe->setCounters(makeSystemCounters(makeCpuCounters(100, 0, 50, 500), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));

    // Default should match shared sampling default
    EXPECT_DOUBLE_EQ(model.maxHistorySeconds(), Domain::Sampling::HISTORY_SECONDS_DEFAULT);

    // Clamp below minimum (10s)
    model.setMaxHistorySeconds(5.0);
    EXPECT_DOUBLE_EQ(model.maxHistorySeconds(), Domain::Sampling::HISTORY_SECONDS_MIN);

    // Clamp above maximum (1800s)
    model.setMaxHistorySeconds(7200.0);
    EXPECT_DOUBLE_EQ(model.maxHistorySeconds(), Domain::Sampling::HISTORY_SECONDS_MAX);
}

// =============================================================================
// Network Monitoring Tests
// =============================================================================

TEST(SystemModelTest, NetworkCapabilityExposed)
{
    auto probe = std::make_unique<MockSystemProbe>();
    Platform::SystemCapabilities caps;
    caps.hasNetworkCounters = true;
    probe->setCapabilities(caps);

    Domain::SystemModel model(std::move(probe));

    const auto& modelCaps = model.capabilities();
    EXPECT_TRUE(modelCaps.hasNetworkCounters);
}

TEST(SystemModelTest, NetworkRatesZeroOnFirstSample)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // Set up counters with network data
    auto counters = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                       makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                       0,    // uptime
                                       {},   // per-core
                                       1000, // netRxBytes
                                       2000  // netTxBytes
    );
    rawProbe->setCounters(counters);

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    // First sample has no previous, so rates should be 0
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 0.0);
}

TEST(SystemModelTest, NetworkRatesComputedFromDeltas)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample: set initial network counters
    auto counters1 = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        0,    // uptime
                                        {},   // per-core
                                        1000, // netRxBytes
                                        2000  // netTxBytes
    );
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 0.0); // t=0

    // Second sample: increased counters after 1 second
    auto counters2 = makeSystemCounters(makeCpuCounters(200, 0, 100, 1700),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        1,    // uptime
                                        {},   // per-core
                                        2000, // netRxBytes (+1000)
                                        4000  // netTxBytes (+2000)
    );
    model.updateFromCounters(counters2, 1.0); // t=1

    auto snap = model.snapshot();
    // After 1 second: delta=1000 bytes / 1 second = 1000 bytes/sec
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 2000.0);
}

TEST(SystemModelTest, NetworkRatesHandleCounterRollback)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample
    auto counters1 = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        0,
                                        {},
                                        5000, // netRxBytes (high)
                                        8000  // netTxBytes (high)
    );
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 0.0);

    // Second sample: counters lower (system restart or counter overflow)
    auto counters2 = makeSystemCounters(makeCpuCounters(200, 0, 100, 1700),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        1,
                                        {},
                                        100, // netRxBytes (rolled back)
                                        200  // netTxBytes (rolled back)
    );
    model.updateFromCounters(counters2, 1.0);

    auto snap = model.snapshot();
    // When counters roll back, rates should be 0 (not negative)
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 0.0);
}

TEST(SystemModelTest, NetworkHistoryTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    auto counters1 = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        0,
                                        {},
                                        0, // netRxBytes
                                        0  // netTxBytes
    );
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 0.0);

    // Add several samples
    for (int i = 1; i <= 5; ++i)
    {
        auto counters = makeSystemCounters(
            makeCpuCounters(100 * static_cast<uint64_t>(i + 1), 0, 50 * static_cast<uint64_t>(i + 1), 850 * static_cast<uint64_t>(i + 1)),
            makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
            static_cast<uint64_t>(i),
            {},
            1000ULL * static_cast<uint64_t>(i), // Increasing RX
            2000ULL * static_cast<uint64_t>(i)  // Increasing TX
        );
        model.updateFromCounters(counters, static_cast<double>(i));
    }

    auto rxHistory = model.netRxHistory();
    auto txHistory = model.netTxHistory();

    // 5 deltas recorded (from samples 1-5)
    EXPECT_EQ(rxHistory.size(), 5);
    EXPECT_EQ(txHistory.size(), 5);

    // First entry: (1000-0) / 1 second = 1000 bytes/sec
    EXPECT_FLOAT_EQ(rxHistory[0], 1000.0F);
    EXPECT_FLOAT_EQ(txHistory[0], 2000.0F);

    // All subsequent deltas: 1000 bytes per 1 second = 1000 bytes/sec
    for (std::size_t j = 0; j < 5; ++j)
    {
        EXPECT_FLOAT_EQ(rxHistory[j], 1000.0F);
        EXPECT_FLOAT_EQ(txHistory[j], 2000.0F);
    }
}

TEST(SystemModelTest, NetworkRatesWithVariableTimeDelta)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    auto counters1 =
        makeSystemCounters(makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 0, 0);
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 0.0);

    // 1000 bytes in 0.5 seconds = 2000 bytes/sec
    auto counters2 = makeSystemCounters(makeCpuCounters(200, 0, 100, 1700),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        0,
                                        {},
                                        1000, // +1000 RX
                                        500   // +500 TX
    );
    model.updateFromCounters(counters2, 0.5);

    auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 2000.0); // 1000 bytes / 0.5 sec
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 1000.0); // 500 bytes / 0.5 sec
}

TEST(SystemModelTest, NetworkRatesZeroWhenTimeDeltaIsZero)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    auto counters1 = makeSystemCounters(
        makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 1000, 2000);
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 1.0);

    // Same timestamp - time delta is 0
    auto counters2 = makeSystemCounters(
        makeCpuCounters(200, 0, 100, 1700), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 2000, 4000);
    model.updateFromCounters(counters2, 1.0); // Same time

    auto snap = model.snapshot();
    // Division by zero protection: rates should be 0
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 0.0);
}

TEST(SystemModelTest, NetworkHistoryTrimmedByTime)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    auto counters =
        makeSystemCounters(makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 0, 0);
    rawProbe->setCounters(counters);

    Domain::SystemModel model(std::move(probe));
    model.setMaxHistorySeconds(10.0); // Short window for testing

    // First sample at t=0
    model.updateFromCounters(counters, 0.0);

    // Add samples spanning 15 seconds
    for (int i = 1; i <= 15; ++i)
    {
        auto c = makeSystemCounters(
            makeCpuCounters(100 * static_cast<uint64_t>(i + 1), 0, 50 * static_cast<uint64_t>(i + 1), 850 * static_cast<uint64_t>(i + 1)),
            makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
            static_cast<uint64_t>(i),
            {},
            1000ULL * static_cast<uint64_t>(i),
            2000ULL * static_cast<uint64_t>(i));
        model.updateFromCounters(c, static_cast<double>(i));
    }

    auto rxHistory = model.netRxHistory();
    auto timestamps = model.timestamps();

    // With 10-second window and samples at t=1..15, should keep ~10 samples
    // (samples from t=6..15, which is within 10 seconds of t=15)
    EXPECT_LE(rxHistory.size(), 11U); // At most 11 samples in 10-second window
    EXPECT_GE(rxHistory.size(), 9U);  // At least 9 samples (timing may vary slightly)

    // Timestamps should be within the window
    if (!timestamps.empty())
    {
        const double latestTime = timestamps.back();
        for (double ts : timestamps)
        {
            EXPECT_GE(ts, latestTime - 10.0);
        }
    }
}
// ==========================================================================
// Per-Interface Network Tests
// ==========================================================================

TEST(SystemModelTest, PerInterfaceNetworkRatesZeroOnFirstSample)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // Create counters with two network interfaces
    auto iface1 = makeInterfaceCounters("eth0", 1000, 500, true, 1000);
    auto iface2 = makeInterfaceCounters("wlan0", 2000, 1000, true, 100);

    auto counters = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                       makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                       0,
                                       {},
                                       3000,
                                       1500,
                                       {iface1, iface2});
    rawProbe->setCounters(counters);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters, 1.0);

    auto snap = model.snapshot();
    // Should have two interfaces
    ASSERT_EQ(snap.networkInterfaces.size(), 2U);

    // First sample - rates should be zero (no previous data)
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].rxBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].txBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[1].rxBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[1].txBytesPerSec, 0.0);

    // Interface metadata should be present
    EXPECT_EQ(snap.networkInterfaces[0].name, "eth0");
    EXPECT_TRUE(snap.networkInterfaces[0].isUp);
    EXPECT_EQ(snap.networkInterfaces[0].linkSpeedMbps, 1000U);
    EXPECT_EQ(snap.networkInterfaces[1].name, "wlan0");
    EXPECT_TRUE(snap.networkInterfaces[1].isUp);
    EXPECT_EQ(snap.networkInterfaces[1].linkSpeedMbps, 100U);
}

TEST(SystemModelTest, PerInterfaceNetworkRatesComputedFromDeltas)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample
    auto iface1_t1 = makeInterfaceCounters("eth0", 1000, 500);
    auto iface2_t1 = makeInterfaceCounters("wlan0", 2000, 1000);

    auto counters1 = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        0,
                                        {},
                                        3000,
                                        1500,
                                        {iface1_t1, iface2_t1});
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 1.0);

    // Second sample 1 second later with increased counters
    auto iface1_t2 = makeInterfaceCounters("eth0", 2000, 1500);  // +1000 rx, +1000 tx
    auto iface2_t2 = makeInterfaceCounters("wlan0", 2500, 1200); // +500 rx, +200 tx

    auto counters2 = makeSystemCounters(makeCpuCounters(200, 0, 100, 1700),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        0,
                                        {},
                                        4500,
                                        2700,
                                        {iface1_t2, iface2_t2});
    model.updateFromCounters(counters2, 2.0); // 1 second later

    auto snap = model.snapshot();
    ASSERT_EQ(snap.networkInterfaces.size(), 2U);

    // eth0: (2000-1000) / 1.0 = 1000 rx/s, (1500-500) / 1.0 = 1000 tx/s
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].rxBytesPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].txBytesPerSec, 1000.0);

    // wlan0: (2500-2000) / 1.0 = 500 rx/s, (1200-1000) / 1.0 = 200 tx/s
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[1].rxBytesPerSec, 500.0);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[1].txBytesPerSec, 200.0);
}

TEST(SystemModelTest, PerInterfaceNetworkRatesHandleNewInterface)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample with one interface
    auto iface1 = makeInterfaceCounters("eth0", 1000, 500);

    auto counters1 = makeSystemCounters(
        makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 1000, 500, {iface1});
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 1.0);

    // Second sample adds a new interface (e.g., VPN connected)
    auto iface1_t2 = makeInterfaceCounters("eth0", 2000, 1000);
    auto iface_new = makeInterfaceCounters("tun0", 500, 250); // New interface

    auto counters2 = makeSystemCounters(makeCpuCounters(200, 0, 100, 1700),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        0,
                                        {},
                                        2500,
                                        1250,
                                        {iface1_t2, iface_new});
    model.updateFromCounters(counters2, 2.0);

    auto snap = model.snapshot();
    ASSERT_EQ(snap.networkInterfaces.size(), 2U);

    // eth0 should have calculated rates
    EXPECT_EQ(snap.networkInterfaces[0].name, "eth0");
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].rxBytesPerSec, 1000.0);

    // tun0 is new, so rates should be zero (no previous data for this interface)
    EXPECT_EQ(snap.networkInterfaces[1].name, "tun0");
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[1].rxBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[1].txBytesPerSec, 0.0);
}

TEST(SystemModelTest, PerInterfaceNetworkRatesHandleInterfaceRemoval)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample with two interfaces
    auto iface1 = makeInterfaceCounters("eth0", 1000, 500);
    auto iface2 = makeInterfaceCounters("wlan0", 2000, 1000);

    auto counters1 = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        0,
                                        {},
                                        3000,
                                        1500,
                                        {iface1, iface2});
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 1.0);

    // Second sample - wlan0 is gone (e.g., Wi-Fi disabled)
    auto iface1_t2 = makeInterfaceCounters("eth0", 2000, 1000);

    auto counters2 = makeSystemCounters(makeCpuCounters(200, 0, 100, 1700),
                                        makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                        0,
                                        {},
                                        2000,
                                        1000,
                                        {iface1_t2});
    model.updateFromCounters(counters2, 2.0);

    auto snap = model.snapshot();
    // Only eth0 should be in the snapshot
    ASSERT_EQ(snap.networkInterfaces.size(), 1U);
    EXPECT_EQ(snap.networkInterfaces[0].name, "eth0");
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].rxBytesPerSec, 1000.0);
}

TEST(SystemModelTest, PerInterfaceNetworkRatesWithVariableTimeDelta)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    auto iface1 = makeInterfaceCounters("eth0", 1000, 500);

    auto counters1 = makeSystemCounters(
        makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 1000, 500, {iface1});
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 1.0);

    // Second sample 0.5 seconds later
    auto iface1_t2 = makeInterfaceCounters("eth0", 1500, 750);

    auto counters2 = makeSystemCounters(
        makeCpuCounters(200, 0, 100, 1700), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 1500, 750, {iface1_t2});
    model.updateFromCounters(counters2, 1.5); // 0.5 seconds later

    auto snap = model.snapshot();
    ASSERT_EQ(snap.networkInterfaces.size(), 1U);

    // (1500-1000) / 0.5 = 1000 rx/s, (750-500) / 0.5 = 500 tx/s
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].rxBytesPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].txBytesPerSec, 500.0);
}

TEST(SystemModelTest, PerInterfaceMetadataPreserved)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    Platform::SystemCounters::InterfaceCounters iface;
    iface.name = "enp0s31f6";
    iface.displayName = "Intel Ethernet I219-V";
    iface.rxBytes = 1000;
    iface.txBytes = 500;
    iface.isUp = true;
    iface.linkSpeedMbps = 2500;

    auto counters = makeSystemCounters(
        makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 1000, 500, {iface});
    rawProbe->setCounters(counters);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters, 1.0);

    auto snap = model.snapshot();
    ASSERT_EQ(snap.networkInterfaces.size(), 1U);

    // Verify all metadata is preserved in snapshot
    EXPECT_EQ(snap.networkInterfaces[0].name, "enp0s31f6");
    EXPECT_EQ(snap.networkInterfaces[0].displayName, "Intel Ethernet I219-V");
    EXPECT_TRUE(snap.networkInterfaces[0].isUp);
    EXPECT_EQ(snap.networkInterfaces[0].linkSpeedMbps, 2500U);
}

TEST(SystemModelTest, PerInterfaceNetworkEmptyWhenNoInterfaces)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // No interfaces
    auto counters = makeSystemCounters(
        makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 0, 0, {});
    rawProbe->setCounters(counters);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters, 1.0);

    auto snap = model.snapshot();
    EXPECT_TRUE(snap.networkInterfaces.empty());
}

// =============================================================================
// Additional History Accessor Tests
// =============================================================================

TEST(SystemModelTest, CpuIowaitHistoryTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8000, 500), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Second sample with iowait delta
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(2000, 0, 1000, 16000, 1000), makeMemoryCounters(1024, 512)));
    model.refresh();

    auto iowaitHistory = model.cpuIowaitHistory();
    EXPECT_FALSE(iowaitHistory.empty());
}

TEST(SystemModelTest, CpuIdleHistoryTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // First sample
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8000), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Second sample
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(2000, 0, 1000, 16000), makeMemoryCounters(1024, 512)));
    model.refresh();

    auto idleHistory = model.cpuIdleHistory();
    EXPECT_FALSE(idleHistory.empty());
}

TEST(SystemModelTest, MemoryCachedHistoryTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // Memory with cached bytes
    auto mem = makeMemoryCounters(1024ULL * 1024 * 1024, // total
                                  512ULL * 1024 * 1024,  // available
                                  256ULL * 1024 * 1024,  // free
                                  128ULL * 1024 * 1024,  // cached
                                  64ULL * 1024 * 1024);  // buffers
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), mem));

    Domain::SystemModel model(std::move(probe));
    model.refresh();
    model.refresh(); // Need two samples for history

    auto cachedHistory = model.memoryCachedHistory();
    EXPECT_FALSE(cachedHistory.empty());
}

TEST(SystemModelTest, PerInterfaceRxHistoryTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    auto iface = makeInterfaceCounters("eth0", 1000, 500);
    auto counters1 = makeSystemCounters(makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024, 512), 0, {}, 1000, 500, {iface});
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 1.0);

    // Second sample
    auto iface2 = makeInterfaceCounters("eth0", 2000, 1000);
    auto counters2 = makeSystemCounters(makeCpuCounters(200, 0, 100, 1700), makeMemoryCounters(1024, 512), 0, {}, 2000, 1000, {iface2});
    model.updateFromCounters(counters2, 2.0);

    // Query per-interface history
    auto eth0RxHistory = model.netRxHistoryForInterface("eth0");
    EXPECT_FALSE(eth0RxHistory.empty());

    // Non-existent interface should return empty
    auto fakeHistory = model.netRxHistoryForInterface("nonexistent");
    EXPECT_TRUE(fakeHistory.empty());
}

TEST(SystemModelTest, PerInterfaceTxHistoryTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    auto iface = makeInterfaceCounters("eth0", 1000, 500);
    auto counters1 = makeSystemCounters(makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024, 512), 0, {}, 1000, 500, {iface});
    rawProbe->setCounters(counters1);

    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 1.0);

    // Second sample
    auto iface2 = makeInterfaceCounters("eth0", 2000, 1500);
    auto counters2 = makeSystemCounters(makeCpuCounters(200, 0, 100, 1700), makeMemoryCounters(1024, 512), 0, {}, 2000, 1500, {iface2});
    model.updateFromCounters(counters2, 2.0);

    // Query per-interface TX history
    auto eth0TxHistory = model.netTxHistoryForInterface("eth0");
    EXPECT_FALSE(eth0TxHistory.empty());
}

TEST(SystemModelTest, PowerHistoryTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // Setup basic counters
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();
    model.refresh();

    // Power history should exist (even if values are 0)
    auto powerHist = model.powerHistory();
    EXPECT_FALSE(powerHist.empty());
}

TEST(SystemModelTest, BatteryChargeHistoryTracked)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // Setup basic counters
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();
    model.refresh();

    // Battery charge history should exist
    auto chargeHist = model.batteryChargeHistory();
    EXPECT_FALSE(chargeHist.empty());
}
