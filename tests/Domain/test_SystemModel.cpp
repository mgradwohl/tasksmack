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
#include "Domain/SystemSnapshot.h"
#include "Mocks/MockProbes.h"
#include "Platform/PowerTypes.h"
#include "Platform/SystemTypes.h"
#include "PublicationLatency.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// Use shared mock from TestMocks namespace
using TestMocks::makeCpuCounters;
using TestMocks::makeInterfaceCounters;
using TestMocks::makeMemoryCounters;
using TestMocks::makeSystemCounters;
using TestMocks::MockPowerProbe;
using TestMocks::MockSystemProbe;

// =============================================================================
// Platform::CpuCounters Tests (SystemTypes.h)
// =============================================================================

TEST(CpuCountersTest, TotalCalculatesAllComponentsExceptGuest)
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

    // total = 100 + 20 + 50 + 800 + 10 + 5 + 3 + 7 = 995: guest and guestNice are already
    // inside user and nice, so adding them again would count them twice (#1157)
    EXPECT_EQ(c.total(), 995);
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

    // active = 100 + 20 + 50 + 5 + 3 + 7 = 185
    // (excludes idle=800 and iowait=10, and guest/guestNice, which user/nice already hold)
    EXPECT_EQ(c.active(), 185);
    EXPECT_EQ(c.idleTotal(), 810);
    EXPECT_EQ(c.active() + c.idleTotal(), c.total());
}

TEST(CpuCountersTest, GuestTimeIsCountedOnce)
{
    // Linux adds guest time to user (and guest_nice to nice) as well as reporting it on its own,
    // so a core that spent its whole interval running a VM reports user == guest. Counting the
    // guest field again made that core look like it ran for twice as long as it did (#1157).
    Platform::CpuCounters c;
    c.user = 400;
    c.guest = 400; // all of user was guest time
    c.nice = 50;
    c.guestNice = 50; // all of nice was niced guest time
    c.idle = 550;

    EXPECT_EQ(c.total(), 1000);
    EXPECT_EQ(c.active(), 450);
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

TEST(SystemModelTest, SampleDelegatesToRefresh)
{
    // sample() is the ISamplable override BackgroundSampler calls in production;
    // every other test in this file drives refresh() directly, so this is the
    // only coverage of sample()'s own body.
    auto probe = std::make_unique<MockSystemProbe>();
    const auto cpu = makeCpuCounters(0, 0, 0, 10000);
    const std::vector<Platform::CpuCounters> perCore(4, cpu);
    probe->setCounters(makeSystemCounters(cpu, makeMemoryCounters(16ULL * 1024 * 1024 * 1024, 8ULL * 1024 * 1024 * 1024), 0, perCore));

    Domain::SystemModel model(std::move(probe));
    static_cast<Domain::ISamplable&>(model).sample();

    auto snap = model.snapshot();
    EXPECT_EQ(snap.coreCount, 4);
}

TEST(SystemModelTest, PublishesCoherentVersionedState)
{
    Domain::SystemModel model(nullptr);
    const auto counters = makeSystemCounters(makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1000, 400));

    model.updateFromCounters(counters, 1.0);
    const auto first = model.publication();
    model.updateFromCounters(counters, 2.0);
    const auto second = model.publication();

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->version, 1);
    EXPECT_EQ(second->version, 2);
    EXPECT_EQ(first->timestamps.size(), first->cpuHistory.size());
    EXPECT_EQ(second->timestamps.size(), second->memoryHistory.size());
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

    // Old kernel without MemAvailable
    // total=100, free=20, cached=30, buffers=10
    // used = 100 - 20 - 30 - 10 = 40
    auto mem = makeMemoryCounters(100, 0, 20, 30, 10);
    mem.hasAvailableBytes = false;
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), mem));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_EQ(snap.memoryUsedBytes, 40);
    EXPECT_DOUBLE_EQ(snap.memoryUsedPercent, 40.0);
}

TEST(SystemModelTest, MemAvailableZeroIsMemoryExhaustedNotMissing)
{
    // Under severe pressure the kernel reports MemAvailable: 0. That used to switch to the legacy
    // formula exactly when memory ran out, understating use (#1143).
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), makeMemoryCounters(100, 0, 20, 30, 10)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.snapshot().memoryUsedBytes, 100U);
}

TEST(SystemModelTest, MemoryUsedNeverWrapsBelowZero)
{
    // A container (LXCFS) can report available above total; the unsigned subtraction wrapped to
    // about 16 EiB used (#1143). The legacy formula saturates too.
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), makeMemoryCounters(100, 150)));
    Domain::SystemModel model(std::move(probe));
    model.refresh();
    EXPECT_EQ(model.snapshot().memoryUsedBytes, 0U);

    auto legacyProbe = std::make_unique<MockSystemProbe>();
    auto* rawLegacy = legacyProbe.get();
    auto legacy = makeMemoryCounters(100, 0, 60, 50, 10);
    legacy.hasAvailableBytes = false;
    rawLegacy->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), legacy));
    Domain::SystemModel legacyModel(std::move(legacyProbe));
    legacyModel.refresh();
    EXPECT_EQ(legacyModel.snapshot().memoryUsedBytes, 0U);

    // Parts whose sum wraps (UINT64_MAX + 1 == 0) must not read as all memory used.
    auto wrappingProbe = std::make_unique<MockSystemProbe>();
    auto* rawWrapping = wrappingProbe.get();
    auto wrapping = makeMemoryCounters(100, 0, std::numeric_limits<std::uint64_t>::max(), 1, 0);
    wrapping.hasAvailableBytes = false;
    rawWrapping->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), wrapping));
    Domain::SystemModel wrappingModel(std::move(wrappingProbe));
    wrappingModel.refresh();
    EXPECT_EQ(wrappingModel.snapshot().memoryUsedBytes, 0U);
}

namespace SystemModelTestSupport
{
/// A power probe that records when it was read. In a named namespace, not an anonymous one: mocks
/// built with std::make_unique follow the repository's test convention of external linkage.
class TimedPowerProbe : public Platform::IPowerProbe
{
  public:
    explicit TimedPowerProbe(std::chrono::steady_clock::time_point* readAt) : m_ReadAt(readAt)
    {}
    [[nodiscard]] Platform::PowerCounters read() override
    {
        *m_ReadAt = std::chrono::steady_clock::now();
        return {};
    }
    [[nodiscard]] Platform::PowerCapabilities capabilities() const override
    {
        return {};
    }

  private:
    std::chrono::steady_clock::time_point* m_ReadAt;
};
} // namespace SystemModelTestSupport

TEST(SystemModelTest, SampleIsStampedBeforeThePowerRead)
{
    // The power read has its own variable latency; stamped after it, the rate interval jittered
    // with it (#1144). The timestamp must come from before the power probe is read.
    auto probe = std::make_unique<MockSystemProbe>();
    probe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), makeMemoryCounters(100, 50)));
    std::chrono::steady_clock::time_point powerReadAt;
    Domain::SystemModel model(std::move(probe), std::make_unique<SystemModelTestSupport::TimedPowerProbe>(&powerReadAt));
    model.refresh();
    model.refresh();

    const auto timestamps = model.timestamps();
    ASSERT_FALSE(timestamps.empty());
    EXPECT_LE(timestamps.back(), std::chrono::duration<double>(powerReadAt.time_since_epoch()).count());
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

TEST(SystemModelTest, SwapFreeAboveTotalReadsAsZeroUsed)
{
    // A probe once reported more free swap than total (#1026); the unsigned subtraction then
    // wrapped to ~2^64 and swap read 100 %. Used must clamp at 0 instead.
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    const auto mem = makeMemoryCounters(8ULL * 1024 * 1024 * 1024,
                                        4ULL * 1024 * 1024 * 1024,
                                        0,
                                        0,
                                        0,
                                        4ULL * 1024 * 1024 * 1024, // 4 GB swap total
                                        5ULL * 1024 * 1024 * 1024  // 5 GB "free"
    );
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), mem));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    const auto snap = model.snapshot();
    EXPECT_EQ(snap.swapUsedBytes, 0ULL);
    EXPECT_DOUBLE_EQ(snap.swapUsedPercent, 0.0);
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

TEST(SystemModelTest, CpuPercentCountsIowaitAsIdle)
{
    // iowait is a CPU with nothing to run while it waits for I/O: idle time, as on Windows, which
    // has no iowait at all. It is still reported as its own breakdown (#1157).
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8000, 300, 200), makeMemoryCounters(1024, 512)));
    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Delta: user=1000, system=500, idle=7000, iowait=1000, steal=500 (total=10000)
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(2000, 0, 1000, 15000, 1300, 700), makeMemoryCounters(1024, 512)));
    model.refresh();

    const auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.cpuTotal.totalPercent, 20.0); // user + system + steal; was 30 with iowait counted busy
    EXPECT_DOUBLE_EQ(snap.cpuTotal.idlePercent, 70.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.iowaitPercent, 10.0);
}

TEST(SystemModelTest, CpuPercentCountsGuestTimeOnce)
{
    // The issue's VM host: a guest pins 4 of 8 cores, so half of all CPU time is guest time, which
    // the kernel reports in user *and* in guest. The chart must read 50%, not 67% (#1157).
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    auto withGuest = [](std::uint64_t user, std::uint64_t idle, std::uint64_t guest)
    {
        auto c = makeCpuCounters(user, 0, 0, idle);
        c.guest = guest;
        return c;
    };

    rawProbe->setCounters(makeSystemCounters(withGuest(0, 0, 0), makeMemoryCounters(1024, 512)));
    Domain::SystemModel model(std::move(probe));
    model.refresh();

    rawProbe->setCounters(makeSystemCounters(withGuest(4000, 4000, 4000), makeMemoryCounters(1024, 512)));
    model.refresh();

    const auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.cpuTotal.totalPercent, 50.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.userPercent, 50.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.idlePercent, 50.0);
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

TEST(SystemModelTest, PerCoreHistoryStaysAlignedOnCoreCountDecrease)
{
    // Establish two cores over three samples, then simulate a transient probe
    // read that reports only one core.  The retained ring for core 1 must
    // receive a NaN placeholder (a gap, #1146) so every core series stays the same length
    // as the timestamp axis.  A subsequent sample with two cores again must
    // resume normal values.
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // Sample 1: baseline (no delta yet, first refresh is discarded)
    std::vector<Platform::CpuCounters> cores1 = {makeCpuCounters(0, 0, 0, 10000), makeCpuCounters(0, 0, 0, 10000)};
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 20000), makeMemoryCounters(1024, 512), 0, cores1));
    Domain::SystemModel model(std::move(probe));
    model.refresh(); // establishes baseline, no history entry

    // Sample 2: both cores active → 1 entry per core ring
    std::vector<Platform::CpuCounters> cores2 = {makeCpuCounters(1000, 0, 1000, 8000), makeCpuCounters(2000, 0, 2000, 6000)};
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(3000, 0, 3000, 14000), makeMemoryCounters(1024, 512), 0, cores2));
    model.refresh();

    // Verify both rings have 1 entry after sample 2
    {
        auto ts = model.timestamps();
        auto cores = model.perCoreHistory();
        ASSERT_EQ(cores.size(), 2);
        EXPECT_EQ(ts.size(), cores[0].size());
        EXPECT_EQ(ts.size(), cores[1].size());
    }

    // Sample 3: only one core reported (simulates transient decrease)
    std::vector<Platform::CpuCounters> cores3 = {makeCpuCounters(2000, 0, 2000, 16000)};
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(4000, 0, 4000, 28000), makeMemoryCounters(1024, 512), 0, cores3));
    model.refresh();

    {
        auto ts = model.timestamps();
        auto cores = model.perCoreHistory();
        ASSERT_EQ(cores.size(), 2);
        // All series must be the same length as the timestamp axis
        EXPECT_EQ(cores[0].size(), ts.size());
        EXPECT_EQ(cores[1].size(), ts.size());
        // Absent core 1 must have received a NaN placeholder (a gap, not a fake 0%) for this sample
        EXPECT_TRUE(std::isnan(cores[1].back()));
    }

    // Sample 4: two cores return → core 1 still gets NaN this sample because
    // m_PrevCounters has no core 1 from sample 3, so no delta is computable for
    // core 1 yet.  What matters is that the ring stays aligned.
    std::vector<Platform::CpuCounters> cores4 = {makeCpuCounters(3000, 0, 3000, 24000), makeCpuCounters(4000, 0, 4000, 22000)};
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(7000, 0, 7000, 46000), makeMemoryCounters(1024, 512), 0, cores4));
    model.refresh();

    {
        auto ts = model.timestamps();
        auto cores = model.perCoreHistory();
        ASSERT_EQ(cores.size(), 2);
        EXPECT_EQ(cores[0].size(), ts.size());
        EXPECT_EQ(cores[1].size(), ts.size());
        // Core 1 still has no reading (prev counters had no core 1); alignment is the key invariant
        EXPECT_TRUE(std::isnan(cores[1].back()));
    }
}

TEST(SystemModelTest, OfflineInteriorCoreKeepsEveryOtherCoreOnItsOwnHistory)
{
    // /proc/stat lists online CPUs only: with cpu2 offline the per-core list is cpu0, cpu1, cpu3.
    // Matching by list position diffed cpu3 against the previous sample's cpu2 and pushed it into
    // core 2's history, shifting every later core onto the wrong label, with the gap landing on
    // the last core. Matched by core id, cpu3 keeps its own series and cpu2 gets the gap (#1229).
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // Core i runs at (i + 1) * 10% over every 1000-tick interval, so each core's line is distinct.
    constexpr std::uint64_t TICKS_PER_SAMPLE = 1000;
    const auto coreAt = [](std::size_t coreId, std::uint64_t sample)
    {
        const std::uint64_t busyPerSample = (coreId + 1) * 100;
        auto core = makeCpuCounters(busyPerSample * sample, 0, 0, (TICKS_PER_SAMPLE - busyPerSample) * sample);
        core.coreId = coreId;
        return core;
    };
    const auto setSample = [&](std::uint64_t sample, const std::vector<std::size_t>& onlineIds)
    {
        auto counters = makeSystemCounters(makeCpuCounters(0, 0, 0, TICKS_PER_SAMPLE * 4 * sample), makeMemoryCounters(1024, 512));
        for (const std::size_t id : onlineIds)
        {
            counters.cpuPerCore.push_back(coreAt(id, sample));
        }
        rawProbe->setCounters(counters);
    };

    setSample(0, {0, 1, 2, 3});
    Domain::SystemModel model(std::move(probe));
    model.refresh(); // baseline
    setSample(1, {0, 1, 2, 3});
    model.refresh();
    setSample(2, {0, 1, 3}); // cpu2 offline
    model.refresh();

    {
        const auto snap = model.snapshot();
        EXPECT_EQ(snap.coreCount, 3); // online cores
        ASSERT_EQ(snap.cpuPerCore.size(), 4U);
        EXPECT_DOUBLE_EQ(snap.cpuPerCore[0].totalPercent, 10.0);
        EXPECT_DOUBLE_EQ(snap.cpuPerCore[1].totalPercent, 20.0);
        EXPECT_TRUE(std::isnan(snap.cpuPerCore[2].totalPercent));
        EXPECT_DOUBLE_EQ(snap.cpuPerCore[3].totalPercent, 40.0);
    }

    setSample(3, {0, 1, 2, 3}); // cpu2 back online
    model.refresh();
    setSample(4, {0, 1, 2, 3});
    model.refresh();

    const auto ts = model.timestamps();
    const auto cores = model.perCoreHistory();
    ASSERT_EQ(ts.size(), 4U);
    ASSERT_EQ(cores.size(), 4U);
    for (const auto& core : cores)
    {
        ASSERT_EQ(core.size(), ts.size());
    }

    // cpu0, cpu1 and cpu3 keep their own load through the middle sample.
    for (std::size_t i = 0; i < ts.size(); ++i)
    {
        EXPECT_FLOAT_EQ(cores[0][i], 10.0F) << "sample " << i;
        EXPECT_FLOAT_EQ(cores[1][i], 20.0F) << "sample " << i;
        EXPECT_FLOAT_EQ(cores[3][i], 40.0F) << "sample " << i;
    }
    // cpu2: its reading, then a gap while offline, a gap on the sample it returns (no previous
    // counters to diff against), then its own reading again.
    EXPECT_FLOAT_EQ(cores[2][0], 30.0F);
    EXPECT_TRUE(std::isnan(cores[2][1]));
    EXPECT_TRUE(std::isnan(cores[2][2]));
    EXPECT_FLOAT_EQ(cores[2][3], 30.0F);
}

TEST(SystemModelTest, ImplausibleCoreIdIsDropped)
{
    // Per-core slots are indexed by core id; a malformed id must not size every per-core vector
    // to billions of entries (#1229).
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    const auto setSample = [&](std::uint64_t busy, std::uint64_t idle)
    {
        auto counters = makeSystemCounters(makeCpuCounters(busy, 0, 0, idle),
                                           makeMemoryCounters(1024, 512),
                                           0,
                                           {makeCpuCounters(busy, 0, 0, idle), makeCpuCounters(busy, 0, 0, idle)});
        counters.cpuPerCore[1].coreId = std::numeric_limits<std::size_t>::max();
        rawProbe->setCounters(counters);
    };

    setSample(0, 0);
    Domain::SystemModel model(std::move(probe));
    model.refresh();
    setSample(500, 500);
    model.refresh();

    const auto snap = model.snapshot();
    ASSERT_EQ(snap.cpuPerCore.size(), 1U);
    EXPECT_DOUBLE_EQ(snap.cpuPerCore[0].totalPercent, 50.0);
    EXPECT_EQ(model.perCoreHistory().size(), 1U);
    EXPECT_EQ(snap.coreCount, 1); // the dropped id isn't counted as a core either
}

namespace
{

/// Sets a sample whose per-core list reports exactly `onlineIds`, each core 10% busy per 1000 ticks.
void setCoreSample(MockSystemProbe& probe, std::uint64_t sample, const std::vector<std::size_t>& onlineIds)
{
    auto counters = makeSystemCounters(makeCpuCounters(0, 0, 0, 1000 * sample), makeMemoryCounters(1024, 512));
    for (const std::size_t id : onlineIds)
    {
        auto core = makeCpuCounters(100 * sample, 0, 0, 900 * sample);
        core.coreId = id;
        counters.cpuPerCore.push_back(core);
    }
    probe.setCounters(counters);
}

} // namespace

TEST(SystemModelTest, SeenCoreIdsListOnlyTheIdsTheProbeReported)
{
    // Ids 4 and 5 are never reported (a Windows group's reserved hot-add capacity, or Linux CPUs
    // never online). The per-core slots run to the highest id, but only the seven reported ids are
    // published as seen, so the CPU Cores grid charts seven cores, not nine (#1262).
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    const std::vector<std::size_t> reported{0, 1, 2, 3, 6, 7, 8};

    setCoreSample(*rawProbe, 0, reported);
    Domain::SystemModel model(std::move(probe));
    model.refresh();
    setCoreSample(*rawProbe, 1, reported);
    model.refresh();

    const auto publication = model.publication();
    ASSERT_NE(publication, nullptr);
    EXPECT_EQ(publication->snapshot.seenCoreIds, reported);
    EXPECT_EQ(publication->snapshot.cpuPerCore.size(), 9U); // slots still indexed by id (#1229)
    EXPECT_EQ(publication->snapshot.coreCount, 7);
    EXPECT_EQ(model.snapshot().seenCoreIds, reported);
}

TEST(SystemModelTest, SeenCoreIdsAreKnownFromTheFirstSample)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    setCoreSample(*rawProbe, 0, {2, 0, 1}); // listed out of order: published ascending
    Domain::SystemModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.snapshot().seenCoreIds, (std::vector<std::size_t>{0, 1, 2}));
}

TEST(SystemModelTest, ACoreSeenThenOfflineStaysInSeenCoreIds)
{
    // A CPU that goes offline keeps its chart, with a gap (#1229): its id stays seen, both for an
    // interior CPU and for the highest one, whose slot the snapshot no longer needs.
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    setCoreSample(*rawProbe, 0, {0, 1, 2, 3});
    Domain::SystemModel model(std::move(probe));
    model.refresh();
    setCoreSample(*rawProbe, 1, {0, 1, 2, 3});
    model.refresh();
    setCoreSample(*rawProbe, 2, {0, 1, 3}); // cpu2 offline
    model.refresh();
    EXPECT_EQ(model.snapshot().seenCoreIds, (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_TRUE(std::isnan(model.snapshot().cpuPerCore[2].totalPercent));

    setCoreSample(*rawProbe, 3, {0, 1}); // cpu3 offline too
    model.refresh();
    setCoreSample(*rawProbe, 4, {0, 1});
    model.refresh();
    EXPECT_EQ(model.snapshot().seenCoreIds, (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(model.perCoreHistory().size(), 4U); // history keeps their slots for the charts
}

TEST(SystemModelTest, ImplausibleCoreIdIsNotSeen)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    setCoreSample(*rawProbe, 0, {0, std::numeric_limits<std::size_t>::max()});
    Domain::SystemModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.snapshot().seenCoreIds, (std::vector<std::size_t>{0}));
}

TEST(SystemModelTest, HotAddedCoreIsBackfilledWithGaps)
{
    // A core that appears mid-run gets NaN for the samples before it existed (a gap, not a fake
    // 0%, #1146), so its ring stays aligned with the timestamp axis.
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    const auto oneCore = [&](std::uint64_t busy, std::uint64_t idle)
    {
        rawProbe->setCounters(
            makeSystemCounters(makeCpuCounters(busy, 0, 0, idle), makeMemoryCounters(1024, 512), 0, {makeCpuCounters(busy, 0, 0, idle)}));
    };
    const auto twoCores = [&](std::uint64_t busy, std::uint64_t idle)
    {
        rawProbe->setCounters(makeSystemCounters(makeCpuCounters(2 * busy, 0, 0, 2 * idle),
                                                 makeMemoryCounters(1024, 512),
                                                 0,
                                                 {makeCpuCounters(busy, 0, 0, idle), makeCpuCounters(busy, 0, 0, idle)}));
    };

    oneCore(0, 0);
    Domain::SystemModel model(std::move(probe));
    model.refresh(); // baseline
    oneCore(100, 100);
    model.refresh();
    oneCore(200, 200);
    model.refresh();
    twoCores(300, 300); // the previous sample had one core, so core 1 has no delta yet
    model.refresh();
    twoCores(400, 400);
    model.refresh();

    const auto ts = model.timestamps();
    const auto cores = model.perCoreHistory();
    ASSERT_EQ(cores.size(), 2U);
    ASSERT_EQ(cores[0].size(), ts.size());
    ASSERT_EQ(cores[1].size(), ts.size());
    ASSERT_GE(ts.size(), 2U);
    for (std::size_t i = 0; i + 1 < cores[1].size(); ++i)
    {
        EXPECT_TRUE(std::isnan(cores[1][i])) << "sample " << i;
        EXPECT_FALSE(std::isnan(cores[0][i])) << "sample " << i;
    }
    EXPECT_FLOAT_EQ(cores[1].back(), 50.0F);
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

TEST(SystemModelTest, RegressedCpuFieldDoesNotUnderflowIntoPinned100Percent)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    // user=1000 nice=0 system=500 idle=8500 iowait=100 steal=0 -> total=10100
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500, 100, 0), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // Total still increases (10100 -> 11650), but iowait regresses (100 -> 50), simulating a
    // per-core hotplug reindex or a transiently stale counter. Without a rollback guard, the
    // unsigned subtraction wraps and the final std::clamp silently pins iowaitPercent at 100%
    // instead of reporting 0% for the regressed field.
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(2000, 0, 600, 9000, 50, 0), makeMemoryCounters(1024, 512)));
    model.refresh();

    auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.cpuTotal.iowaitPercent, 0.0);
}

TEST(SystemModelTest, RegressedIowaitDoesNotCancelIdleGrowth)
{
    // #1157: total = 100% - (idle + iowait). If iowait regresses by more than idle grew, one delta of
    // (idle + iowait) clamps to 0 and reports the CPU 100% busy although idle grew. Each part is
    // rollback-guarded on its own instead.
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    // user=1000 system=500 idle=8500 iowait=1000 -> total 11000
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(1000, 0, 500, 8500, 1000, 0), makeMemoryCounters(1024, 512)));
    Domain::SystemModel model(std::move(probe));
    model.refresh();

    // user +1500, idle +500, iowait 1000 -> 400 (regressed, counted as 0). idle + iowait went
    // 9500 -> 9400, which a combined delta would clamp to 0 (100% busy). The denominator is the sum
    // of the guarded per-field deltas, 2000, not total()'s 1400 in which the rollback cancelled
    // 600 ticks of real growth.
    rawProbe->setCounters(makeSystemCounters(makeCpuCounters(2500, 0, 500, 9000, 400, 0), makeMemoryCounters(1024, 512)));
    model.refresh();

    const auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.cpuTotal.iowaitPercent, 0.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.idlePercent, 25.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.userPercent, 75.0);
    EXPECT_DOUBLE_EQ(snap.cpuTotal.totalPercent, 75.0);
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

    // NaN maps to the minimum instead of passing through the clamp (#1325)
    model.setMaxHistorySeconds(std::numeric_limits<double>::quiet_NaN());
    EXPECT_DOUBLE_EQ(model.maxHistorySeconds(), Domain::Sampling::HISTORY_SECONDS_MIN);
}

// #1176: maxHistorySeconds() reads under the lock setMaxHistorySeconds() writes under, so reading it
// while another thread changes it (and samples) is no data race. Run under the tsan preset to check;
// elsewhere it checks every value read is one of those written.
TEST(SystemModelTest, MaxHistorySecondsIsSafeToReadWhileItChanges)
{
    auto probe = std::make_unique<MockSystemProbe>();
    probe->setCounters(makeSystemCounters(makeCpuCounters(100, 0, 50, 500), makeMemoryCounters(1024, 512)));
    Domain::SystemModel model(std::move(probe));
    model.refresh();

    constexpr double SHORT_WINDOW = 60.0;
    constexpr double LONG_WINDOW = 600.0;
    constexpr int ITERATIONS = 500;
    std::atomic<bool> done{false};
    std::thread writer(
        [&model, &done]()
        {
            for (int i = 0; i < ITERATIONS; ++i)
            {
                model.setMaxHistorySeconds((i % 2 == 0) ? SHORT_WINDOW : LONG_WINDOW);
                model.refresh();
            }
            done = true;
        });

    bool onlyWrittenValues = true;
    while (!done)
    {
        const double seconds = model.maxHistorySeconds();
        onlyWrittenValues = onlyWrittenValues &&
                            (seconds == SHORT_WINDOW || seconds == LONG_WINDOW || seconds == Domain::Sampling::HISTORY_SECONDS_DEFAULT);
    }
    writer.join();

    EXPECT_TRUE(onlyWrittenValues);
    EXPECT_DOUBLE_EQ(model.maxHistorySeconds(), LONG_WINDOW); // the last write
}

// #1145: a window change republishes the trimmed history at once instead of leaving the old window's
// data, scale and peaks on show until the next sample.
TEST(SystemModelTest, ShrinkingTheHistoryWindowRepublishesTheTrimmedHistory)
{
    auto probe = std::make_unique<MockSystemProbe>();
    const auto counters = makeSystemCounters(makeCpuCounters(100, 0, 50, 500), makeMemoryCounters(1024, 512));
    Domain::SystemModel model(std::move(probe));
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_DEFAULT);

    for (int i = 0; i <= 100; ++i)
    {
        model.updateFromCounters(counters, static_cast<double>(i));
    }
    const std::uint64_t versionBefore = model.publicationVersion();
    ASSERT_GT(model.publication()->timestamps.size(), 30U);

    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);

    EXPECT_GT(model.publicationVersion(), versionBefore);
    const auto publication = model.publication();
    EXPECT_EQ(publication->version, model.publicationVersion());
    ASSERT_FALSE(publication->timestamps.empty());
    // t = 90..100, plus the sample kept just before the cutoff (#1016).
    EXPECT_DOUBLE_EQ(publication->timestamps.front(), 100.0 - Domain::Sampling::HISTORY_SECONDS_MIN - 1.0);
    EXPECT_EQ(publication->timestamps.size(), static_cast<std::size_t>(Domain::Sampling::HISTORY_SECONDS_MIN) + 2U);
    EXPECT_EQ(publication->cpuHistory.size(), publication->timestamps.size());
}

TEST(SystemModelTest, ChangingTheHistoryWindowBeforeAnySamplePublishesNothing)
{
    Domain::SystemModel model(std::make_unique<MockSystemProbe>());
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);
    EXPECT_EQ(model.publicationVersion(), 0U);
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

    // Time-based trimming keeps the samples within the 10-second window plus the newest one
    // before it (#1016): cutoff = 15 - 10 = 5, so samples t=4..15 remain (12 entries).
    EXPECT_EQ(rxHistory.size(), 12U);
    EXPECT_EQ(timestamps.size(), 12U);

    // Verify window boundaries
    if (!timestamps.empty())
    {
        EXPECT_DOUBLE_EQ(timestamps.back(), 15.0);
        EXPECT_DOUBLE_EQ(timestamps.front(), 4.0);
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

namespace
{
/// Total rates after two samples one second apart, in which each interface moves `rxDelta` rx and
/// `txDelta` tx bytes; each entry is an interface name and whether it is virtual.
Domain::SystemSnapshot totalAfterOneSecond(const std::vector<std::pair<std::string, bool>>& interfaces, uint64_t rxDelta, uint64_t txDelta)
{
    std::vector<Platform::SystemCounters::InterfaceCounters> before;
    std::vector<Platform::SystemCounters::InterfaceCounters> after;
    for (const auto& [name, isVirtual] : interfaces)
    {
        auto first = makeInterfaceCounters(name, 1000, 1000);
        first.isVirtual = isVirtual;
        auto second = makeInterfaceCounters(name, 1000 + rxDelta, 1000 + txDelta);
        second.isVirtual = isVirtual;
        before.push_back(first);
        after.push_back(second);
    }
    const auto cpu = makeCpuCounters(100, 0, 50, 850);
    const auto memory = makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024);
    auto probe = std::make_unique<MockSystemProbe>();
    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(makeSystemCounters(cpu, memory, 0, {}, 0, 0, before), 1.0);
    model.updateFromCounters(makeSystemCounters(makeCpuCounters(200, 0, 100, 1700), memory, 0, {}, 0, 0, after), 2.0);
    return model.snapshot();
}
} // namespace

TEST(SystemModelTest, NetworkTotalLeavesOutVirtualInterfaces)
{
    // #1106: a VPN tunnel (or a docker bridge, a WSL vEthernet) carries traffic that also crosses the
    // hardware NIC; summing both showed about twice the real throughput.
    const auto snap = totalAfterOneSecond({{"eth0", false}, {"wg0", true}, {"docker0", true}}, 5000, 700);
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 5000.0);
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 700.0);

    // ...while each virtual interface keeps its own rate and stays marked for the UI.
    ASSERT_EQ(snap.networkInterfaces.size(), 3U);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[1].rxBytesPerSec, 5000.0);
    EXPECT_FALSE(snap.networkInterfaces[0].isVirtual);
    EXPECT_TRUE(snap.networkInterfaces[1].isVirtual);
}

namespace
{
/// Feed `model` one sample of interfaces (name, rx, tx) at `nowSeconds`.
void sampleInterfaces(Domain::SystemModel& model,
                      const std::vector<std::tuple<std::string, uint64_t, uint64_t>>& interfaces,
                      double nowSeconds)
{
    std::vector<Platform::SystemCounters::InterfaceCounters> counters;
    counters.reserve(interfaces.size());
    for (const auto& [name, rx, tx] : interfaces)
    {
        counters.push_back(makeInterfaceCounters(name, rx, tx));
    }
    const auto cpu = makeCpuCounters(100, 0, 50, 850);
    const auto memory = makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024);
    model.updateFromCounters(makeSystemCounters(cpu, memory, 0, {}, 0, 0, counters), nowSeconds);
}
} // namespace

TEST(SystemModelTest, AnInterfaceCounterJumpAboveTheCeilingIsAGapNotASpike)
{
    // #1291: max_sane_rate_bps guarded only per-process rates; a driver reset or reinitialised
    // interface counter plotted an absurd spike that blew out the network chart's scale.
    constexpr uint64_t BASE = 1'000'000;
    const auto jump = static_cast<uint64_t>(2.0 * Domain::Sampling::MAX_SANE_RATE_BPS_DEFAULT); // 2x the ceiling in 1 s
    Domain::SystemModel model(std::make_unique<MockSystemProbe>());
    sampleInterfaces(model, {{"eth0", BASE, BASE}, {"wlan0", BASE, BASE}}, 1.0);
    sampleInterfaces(model, {{"eth0", BASE + 5'000, BASE + 700}, {"wlan0", BASE + 100, BASE + 10}}, 2.0);
    sampleInterfaces(model, {{"eth0", BASE + 5'000 + jump, BASE + 1'400}, {"wlan0", BASE + 200, BASE + 20}}, 3.0);

    const auto snap = model.snapshot();
    ASSERT_EQ(snap.networkInterfaces.size(), 2U);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].rxBytesPerSec, 0.0) << "the glitch is not traffic";
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].txBytesPerSec, 700.0) << "the other direction is measured";
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[1].rxBytesPerSec, 100.0);

    const auto eth0Rx = model.netRxHistoryForInterface("eth0");
    const auto eth0Tx = model.netTxHistoryForInterface("eth0");
    ASSERT_EQ(eth0Rx.size(), 2U);
    EXPECT_FLOAT_EQ(eth0Rx[0], 5'000.0F);
    EXPECT_TRUE(std::isnan(eth0Rx[1])) << "a gap, not a spike or a false 0";
    EXPECT_FLOAT_EQ(eth0Tx[1], 700.0F);
    EXPECT_FLOAT_EQ(model.netRxHistoryForInterface("wlan0")[1], 100.0F);

    const auto totalRx = model.netRxHistory();
    const auto totalTx = model.netTxHistory();
    ASSERT_EQ(totalRx.size(), 2U);
    EXPECT_FLOAT_EQ(totalRx[0], 5'100.0F);
    EXPECT_TRUE(std::isnan(totalRx[1])) << "a Total missing a counted interface's sample is a gap too";
    EXPECT_FLOAT_EQ(totalTx[1], 710.0F);
}

TEST(SystemModelTest, TheConfiguredNetworkCeilingAppliesToInterfaceRates)
{
    // The ceiling is [metrics] max_sane_rate_bps, shared with ProcessModel; lowered to its minimum,
    // a 2 GB/s interface sample is dropped, and one just under the ceiling is kept.
    Domain::SystemModel model(std::make_unique<MockSystemProbe>());
    model.setMaxSaneNetworkRate(Domain::Sampling::MAX_SANE_RATE_BPS_MIN);
    const auto ceiling = static_cast<uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_MIN);
    sampleInterfaces(model, {{"eth0", 0, 0}}, 1.0);
    sampleInterfaces(model, {{"eth0", 2 * ceiling, ceiling}}, 2.0);

    const auto snap = model.snapshot();
    ASSERT_EQ(snap.networkInterfaces.size(), 1U);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].rxBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].txBytesPerSec, Domain::Sampling::MAX_SANE_RATE_BPS_MIN) << "at the ceiling is kept";
    EXPECT_TRUE(std::isnan(model.netRxHistoryForInterface("eth0")[0]));
}

TEST(SystemModelTest, TheNetworkCeilingAppliesToTheAggregateCounterFallback)
{
    // #1337: a probe with no per-interface counters falls back to the summed netRxBytes/netTxBytes.
    // That branch applies the same ceiling (#1291): above it the snapshot reads 0 and the history
    // has a NaN gap; exactly at it the rate is kept.
    Domain::SystemModel model(std::make_unique<MockSystemProbe>());
    model.setMaxSaneNetworkRate(Domain::Sampling::MAX_SANE_RATE_BPS_MIN);
    const auto ceiling = static_cast<uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_MIN);
    const auto cpu = makeCpuCounters(100, 0, 50, 850);
    const auto memory = makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024);
    const auto sampleAggregate = [&](uint64_t rx, uint64_t tx, double nowSeconds)
    {
        model.updateFromCounters(makeSystemCounters(cpu, memory, 0, {}, rx, tx, {}), nowSeconds);
    };

    sampleAggregate(0, 0, 1.0);
    sampleAggregate(ceiling + 1, ceiling, 2.0); // rx 1 B/s over the ceiling, tx exactly at it

    auto snap = model.snapshot();
    ASSERT_TRUE(snap.networkInterfaces.empty()) << "the fallback branch needs a probe with no interfaces";
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 0.0) << "over the ceiling is a glitch, not traffic";
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, Domain::Sampling::MAX_SANE_RATE_BPS_MIN) << "at the ceiling is kept";

    auto rxHistory = model.netRxHistory();
    auto txHistory = model.netTxHistory();
    ASSERT_EQ(rxHistory.size(), 1U);
    ASSERT_EQ(txHistory.size(), 1U);
    EXPECT_TRUE(std::isnan(rxHistory[0])) << "a gap, not a spike or a false 0";
    EXPECT_FLOAT_EQ(txHistory[0], static_cast<float>(Domain::Sampling::MAX_SANE_RATE_BPS_MIN));

    // The other way round on the next sample, and an ordinary rate after the glitch is measured again.
    sampleAggregate((2 * ceiling) + 1, (3 * ceiling) + 1, 3.0);
    sampleAggregate((2 * ceiling) + 1'001, (3 * ceiling) + 501, 4.0);

    snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 1'000.0);
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 500.0);
    rxHistory = model.netRxHistory();
    txHistory = model.netTxHistory();
    ASSERT_EQ(rxHistory.size(), 3U);
    ASSERT_EQ(txHistory.size(), 3U);
    EXPECT_FLOAT_EQ(rxHistory[1], static_cast<float>(Domain::Sampling::MAX_SANE_RATE_BPS_MIN));
    EXPECT_TRUE(std::isnan(txHistory[1]));
    EXPECT_FLOAT_EQ(rxHistory[2], 1'000.0F);
    EXPECT_FLOAT_EQ(txHistory[2], 500.0F);
}

TEST(SystemModelTest, InterfaceSnapshotsSayWhetherThePlatformClassifiedThem)
{
    // #1260: the UI follows the platform's isVirtual flag where the platform could classify the
    // interface and falls back to the name only where it couldn't, so the snapshot carries which.
    auto classified = makeInterfaceCounters("wg0", 1000, 1000);
    classified.isVirtual = true;
    classified.isVirtualKnown = true;
    const auto unclassified = makeInterfaceCounters("veth0", 1000, 1000);
    const auto cpu = makeCpuCounters(100, 0, 50, 850);
    const auto memory = makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024);
    Domain::SystemModel model(std::make_unique<MockSystemProbe>());
    model.updateFromCounters(makeSystemCounters(cpu, memory, 0, {}, 0, 0, {classified, unclassified}), 1.0);

    const auto snap = model.snapshot();
    ASSERT_EQ(snap.networkInterfaces.size(), 2U);
    EXPECT_TRUE(snap.networkInterfaces[0].isVirtual);
    EXPECT_TRUE(snap.networkInterfaces[0].isVirtualKnown);
    EXPECT_FALSE(snap.networkInterfaces[1].isVirtual);
    EXPECT_FALSE(snap.networkInterfaces[1].isVirtualKnown);
}

TEST(SystemModelTest, NetworkTotalCountsEveryInterfaceWhenAllAreVirtual)
{
    // Inside a container eth0 is a veth: with no hardware interface the Total must not read 0.
    const auto snap = totalAfterOneSecond({{"eth0", true}, {"eth1", true}}, 300, 100);
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 600.0);
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 200.0);
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

TEST(SystemModelTest, TotalNetworkRateDoesNotSpikeWhenAnInterfaceAppears)
{
    // #1030: an interface appearing used to deliver its whole lifetime byte count into the Total
    // in one sample (here 10 GB, as WSL's vEthernet adapter did). Total is now the sum of the
    // per-interface rates, and a new interface has no rate until its second sample.
    auto probe = std::make_unique<MockSystemProbe>();
    const auto mem = makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024);
    const auto wifi1 = makeInterfaceCounters("Wi-Fi", 1'000'000, 500'000);
    const auto counters1 = makeSystemCounters(makeCpuCounters(100, 0, 50, 850), mem, 0, {}, 1'000'000, 500'000, {wifi1});
    probe->setCounters(counters1);
    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 1.0);

    constexpr std::uint64_t WSL_LIFETIME_RX = 10'000'000'000ULL;
    const auto wifi2 = makeInterfaceCounters("Wi-Fi", 1'002'000, 501'000);
    const auto vEthernet = makeInterfaceCounters("vEthernet (WSL)", WSL_LIFETIME_RX, 1'000'000'000ULL);
    const auto counters2 = makeSystemCounters(
        makeCpuCounters(200, 0, 100, 1700), mem, 0, {}, 1'002'000 + WSL_LIFETIME_RX, 501'000 + 1'000'000'000ULL, {wifi2, vEthernet});
    model.updateFromCounters(counters2, 2.0);

    const auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 2000.0);
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 1000.0);
}

TEST(SystemModelTest, TotalNetworkRateKeepsTheRemainingInterfacesWhenOneDisappears)
{
    // The opposite edge: an interface vanishing used to read as a counter rollback and drop the
    // Total to 0 for that sample, although the remaining interfaces were still moving data.
    auto probe = std::make_unique<MockSystemProbe>();
    const auto mem = makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024);
    const auto counters1 = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                              mem,
                                              0,
                                              {},
                                              3000,
                                              1500,
                                              {makeInterfaceCounters("eth0", 1000, 500), makeInterfaceCounters("wlan0", 2000, 1000)});
    probe->setCounters(counters1);
    Domain::SystemModel model(std::move(probe));
    model.updateFromCounters(counters1, 1.0);

    const auto counters2 =
        makeSystemCounters(makeCpuCounters(200, 0, 100, 1700), mem, 0, {}, 2000, 1000, {makeInterfaceCounters("eth0", 2000, 1000)});
    model.updateFromCounters(counters2, 2.0);

    const auto snap = model.snapshot();
    EXPECT_DOUBLE_EQ(snap.netRxBytesPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(snap.netTxBytesPerSec, 500.0);
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

// Regression test for #776: per-interface history maps must not retain an entry forever
// once its interface has been gone longer than the configured history window (Docker/VPN/
// WiFi interfaces churn over a long-running session). Pruning is time-based (matching
// trimHistory()'s own wall-clock cutoff), not a refresh-cycle count, so a single later sample
// far enough in the future exercises the prune threshold directly -- no loop needed. (An
// earlier version of this test looped hundreds of times because the prune threshold was
// refresh-cycle-based; that was itself a bug caught in review -- historyCapacityForSeconds()
// sizes ring buffers for the fastest *supported* cadence, not the actual one, so a cycle-count
// threshold retained stale entries far longer than the window at any slower cadence.)
TEST(SystemModelTest, PerInterfaceHistoryPrunedAfterExtendedAbsence)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();

    Domain::SystemModel model(std::move(probe));
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);

    auto iface1 = makeInterfaceCounters("eth0", 1000, 500);
    auto iface2 = makeInterfaceCounters("wlan0", 2000, 1000);
    auto countersWithBoth = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                               makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                               0,
                                               {},
                                               3000,
                                               1500,
                                               {iface1, iface2});
    rawProbe->setCounters(countersWithBoth);
    // History only starts accumulating once a previous sample exists to compute deltas
    // against, so two calls are needed before any history buffer is populated.
    model.updateFromCounters(countersWithBoth, 1.0);
    model.updateFromCounters(countersWithBoth, 2.0);

    // wlan0 has just been seen, so it should have real history.
    EXPECT_FALSE(model.netRxHistoryForInterface("wlan0").empty());
    EXPECT_FALSE(model.netTxHistoryForInterface("wlan0").empty());

    // wlan0 disappears (e.g. Wi-Fi disabled) for longer than the entire history window.
    auto countersWithoutWlan = makeSystemCounters(
        makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 3000, 1500, {iface1});
    model.updateFromCounters(countersWithoutWlan, 2.0 + Domain::Sampling::HISTORY_SECONDS_MIN + 1.0);

    // wlan0's entry must be fully pruned (empty, not merely zero-padded), so the map doesn't
    // retain one entry per interface name forever.
    EXPECT_TRUE(model.netRxHistoryForInterface("wlan0").empty());
    EXPECT_TRUE(model.netTxHistoryForInterface("wlan0").empty());

    // eth0, still present every cycle, must be unaffected.
    EXPECT_FALSE(model.netRxHistoryForInterface("eth0").empty());
}

// #1015: a sample where an interface was not present is a NaN gap in its history, not a false 0 --
// both the backfill before it first appears and the placeholder while it is absent.
TEST(SystemModelTest, PerInterfaceHistoryRecordsMissingSamplesAsNaN)
{
    auto probe = std::make_unique<MockSystemProbe>();
    Domain::SystemModel model(std::move(probe));

    const auto eth0 = makeInterfaceCounters("eth0", 1000, 500);
    const auto wlan0 = makeInterfaceCounters("wlan0", 2000, 1000);
    const auto withoutWlan = makeSystemCounters(
        makeCpuCounters(100, 0, 50, 850), makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024), 0, {}, 1000, 500, {eth0});
    const auto withWlan = makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                             makeMemoryCounters(1024ULL * 1024 * 1024, 512ULL * 1024 * 1024),
                                             0,
                                             {},
                                             3000,
                                             1500,
                                             {eth0, wlan0});

    // History starts with the second sample (the first has nothing to take a delta against).
    model.updateFromCounters(withoutWlan, 1.0);
    model.updateFromCounters(withoutWlan, 2.0); // history[0]: wlan0 not seen yet
    model.updateFromCounters(withWlan, 3.0);    // history[1]: wlan0 present
    model.updateFromCounters(withoutWlan, 4.0); // history[2]: wlan0 gone

    const auto timestamps = model.timestamps();
    const auto rx = model.netRxHistoryForInterface("wlan0");
    const auto tx = model.netTxHistoryForInterface("wlan0");
    ASSERT_EQ(rx.size(), timestamps.size());
    ASSERT_EQ(tx.size(), timestamps.size());
    ASSERT_EQ(rx.size(), 3U);

    EXPECT_TRUE(std::isnan(rx[0]));
    EXPECT_TRUE(std::isnan(tx[0]));
    EXPECT_TRUE(std::isfinite(rx[1]));
    EXPECT_TRUE(std::isfinite(tx[1]));
    EXPECT_TRUE(std::isnan(rx[2]));
    EXPECT_TRUE(std::isnan(tx[2]));

    // eth0, present every sample, has no gaps.
    for (const float value : model.netRxHistoryForInterface("eth0"))
    {
        EXPECT_TRUE(std::isfinite(value));
    }
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

// =============================================================================
// Power Status (computePowerStatus via refresh)
// =============================================================================

TEST(SystemModelTest, PowerStatus_NoBattery_WhenNoPowerProbe)
{
    auto probe = std::make_unique<MockSystemProbe>();
    probe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), makeMemoryCounters(1024, 512)));

    Domain::SystemModel model(std::move(probe)); // no power probe
    model.refresh();

    EXPECT_FALSE(model.snapshot().power.hasBattery);
}

TEST(SystemModelTest, PowerStatus_HasBattery_WhenPowerProbeReportsIt)
{
    auto sysProbe = std::make_unique<MockSystemProbe>();
    sysProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), makeMemoryCounters(1024, 512)));

    auto powerProbe = std::make_unique<MockPowerProbe>();
    Platform::PowerCapabilities caps;
    caps.hasBattery = true;
    powerProbe->setCapabilities(caps);

    Platform::PowerCounters counters;
    counters.state = Platform::BatteryState::Discharging;
    counters.isOnAc = false;
    counters.chargePercent = 75;
    counters.powerNowW = 12.5;
    counters.healthPercent = 95;
    counters.technology = "Li-ion";
    counters.model = "TestBattery";
    counters.timeToEmptySec = 7200;
    powerProbe->setCounters(counters);

    Domain::SystemModel model(std::move(sysProbe), std::move(powerProbe));
    model.refresh();

    const auto& power = model.snapshot().power;
    EXPECT_TRUE(power.hasBattery);
    EXPECT_FALSE(power.isOnAc);
    EXPECT_FALSE(power.isCharging);
    EXPECT_TRUE(power.isDischarging);
    EXPECT_FALSE(power.isFull);
    EXPECT_FALSE(power.isNotCharging);
    EXPECT_EQ(power.chargePercent, 75);
    EXPECT_DOUBLE_EQ(power.powerWatts, 12.5);
    EXPECT_EQ(power.healthPercent, 95);
    EXPECT_EQ(power.technology, "Li-ion");
    EXPECT_EQ(power.model, "TestBattery");
    EXPECT_EQ(power.timeToEmptySec, 7200ULL);
}

TEST(SystemModelTest, PowerStatus_Charging)
{
    auto sysProbe = std::make_unique<MockSystemProbe>();
    sysProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), makeMemoryCounters(1024, 512)));

    auto powerProbe = std::make_unique<MockPowerProbe>();
    Platform::PowerCapabilities caps;
    caps.hasBattery = true;
    powerProbe->setCapabilities(caps);

    Platform::PowerCounters counters;
    counters.state = Platform::BatteryState::Charging;
    counters.isOnAc = true;
    counters.chargePercent = 50;
    counters.timeToFullSec = 3600;
    powerProbe->setCounters(counters);

    Domain::SystemModel model(std::move(sysProbe), std::move(powerProbe));
    model.refresh();

    const auto& power = model.snapshot().power;
    EXPECT_TRUE(power.hasBattery);
    EXPECT_TRUE(power.isOnAc);
    EXPECT_TRUE(power.isCharging);
    EXPECT_FALSE(power.isDischarging);
    EXPECT_FALSE(power.isFull);
    EXPECT_EQ(power.timeToFullSec, 3600ULL);
}

TEST(SystemModelTest, PowerStatus_Full)
{
    auto sysProbe = std::make_unique<MockSystemProbe>();
    sysProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), makeMemoryCounters(1024, 512)));

    auto powerProbe = std::make_unique<MockPowerProbe>();
    Platform::PowerCapabilities caps;
    caps.hasBattery = true;
    powerProbe->setCapabilities(caps);

    Platform::PowerCounters counters;
    counters.state = Platform::BatteryState::Full;
    counters.isOnAc = true;
    counters.chargePercent = 100;
    powerProbe->setCounters(counters);

    Domain::SystemModel model(std::move(sysProbe), std::move(powerProbe));
    model.refresh();

    const auto& power = model.snapshot().power;
    EXPECT_TRUE(power.hasBattery);
    EXPECT_TRUE(power.isOnAc);
    EXPECT_FALSE(power.isCharging);
    EXPECT_FALSE(power.isDischarging);
    EXPECT_TRUE(power.isFull);
}

TEST(SystemModelTest, PowerStatus_NoBatteryKeepsTheAdapterReport)
{
    // With no battery, isOnAc is still the adapter's own report (#1109), online or not.
    for (const bool online : {true, false})
    {
        auto sysProbe = std::make_unique<MockSystemProbe>();
        sysProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), makeMemoryCounters(1024, 512)));
        auto powerProbe = std::make_unique<MockPowerProbe>();
        powerProbe->setCapabilities(Platform::PowerCapabilities{});
        Platform::PowerCounters counters;
        counters.state = Platform::BatteryState::NotPresent;
        counters.isOnAc = online;
        powerProbe->setCounters(counters);

        Domain::SystemModel model(std::move(sysProbe), std::move(powerProbe));
        model.refresh();

        const auto& power = model.snapshot().power;
        EXPECT_FALSE(power.hasBattery);
        EXPECT_EQ(power.isOnAc, online);
    }
}

TEST(SystemModelTest, PowerStatus_NotCharging)
{
    // Plugged in but held below full (#1158): its own state, not Full or Discharging.
    auto sysProbe = std::make_unique<MockSystemProbe>();
    sysProbe->setCounters(makeSystemCounters(makeCpuCounters(0, 0, 0, 1000), makeMemoryCounters(1024, 512)));

    auto powerProbe = std::make_unique<MockPowerProbe>();
    Platform::PowerCapabilities caps;
    caps.hasBattery = true;
    powerProbe->setCapabilities(caps);

    Platform::PowerCounters counters;
    counters.state = Platform::BatteryState::NotCharging;
    counters.isOnAc = true;
    counters.chargePercent = 80;
    powerProbe->setCounters(counters);

    Domain::SystemModel model(std::move(sysProbe), std::move(powerProbe));
    model.refresh();

    const auto& power = model.snapshot().power;
    EXPECT_TRUE(power.isOnAc);
    EXPECT_TRUE(power.isNotCharging);
    EXPECT_FALSE(power.isFull);
    EXPECT_FALSE(power.isDischarging);
    EXPECT_EQ(power.chargePercent, 80);
}

// ==========================================================================
// Publication Handoff (#868)
// ==========================================================================

namespace
{

/// Counters with `interfaceCount` interfaces named veth0..vethN-1, each counter advanced by `step`.
[[nodiscard]] Platform::SystemCounters makeManyInterfaces(std::size_t interfaceCount, std::uint64_t step)
{
    std::vector<Platform::SystemCounters::InterfaceCounters> interfaces;
    interfaces.reserve(interfaceCount);
    for (std::size_t i = 0; i < interfaceCount; ++i)
    {
        interfaces.push_back(makeInterfaceCounters("veth" + std::to_string(i), step * 1000, step * 500));
    }
    return makeSystemCounters(
        makeCpuCounters(step * 100, 0, step * 50, step * 850), makeMemoryCounters(1024, 512), 0, {}, 0, 0, std::move(interfaces));
}

/// Every series in one generation is aligned to its timestamps.
[[nodiscard]] bool isAligned(const Domain::SystemPublication& publication)
{
    const std::size_t n = publication.timestamps.size();
    const auto aligned = [n](const auto& entry)
    {
        return entry.second.size() == n;
    };
    return publication.cpuHistory.size() == n && publication.memoryHistory.size() == n && publication.netRxHistory.size() == n &&
           publication.perInterfaceRxHistory.size() == publication.perInterfaceTxHistory.size() &&
           std::ranges::all_of(publication.perInterfaceRxHistory, aligned) &&
           std::ranges::all_of(publication.perInterfaceTxHistory, aligned);
}

} // namespace

TEST(SystemModelTest, PublicationDoesNotWaitForTheWriterToCopyHistory)
{
    // #868: the update path copied every history ring into the new publication while holding the
    // lock publication() needs, so a UI-thread read landing then waited for most of the write -- one
    // slow read per generation. With the copy outside the lock a read waits for a pointer swap at
    // most. Enough interfaces and history that the copy dominates a write; see PublicationLatency.h.
    constexpr std::size_t INTERFACES = 128;
    constexpr std::size_t PREFILL_SAMPLES = 400;
    constexpr std::size_t WRITES = 40;
    constexpr double STEP_SECONDS = static_cast<double>(Domain::Sampling::REFRESH_INTERVAL_MIN_MS) / 1000.0;

    Domain::SystemModel model(std::make_unique<MockSystemProbe>());
    double now = 1000.0;
    std::uint64_t step = 0;
    for (std::size_t i = 0; i < PREFILL_SAMPLES; ++i)
    {
        model.updateFromCounters(makeManyInterfaces(INTERFACES, ++step), now += STEP_SECONDS);
    }

    const auto result = TestPublication::measure(
        WRITES,
        [&](std::size_t) { model.updateFromCounters(makeManyInterfaces(INTERFACES, ++step), now += STEP_SECONDS); },
        [&] { return model.publication(); },
        [&] { return model.publicationVersion(); },
        isAligned);

    EXPECT_EQ(result.versionRegressions, 0U);
    EXPECT_EQ(result.versionAheadOfPointer, 0U);
    EXPECT_EQ(result.inconsistentReads, 0U);
    EXPECT_EQ(model.publicationVersion(), PREFILL_SAMPLES + WRITES);
    EXPECT_GE(result.reads, WRITES);
    // Before #868 about one read per write waited out the copy. A quarter allows for scheduler noise.
    EXPECT_LE(result.slowReads, WRITES / 4) << "median write " << result.medianWriteMs << " ms, slowest read " << result.maxReadMs
                                            << " ms over " << result.reads << " reads";
}

TEST(SystemModelTest, ConcurrentWritersPublishEveryGenerationInOrder)
{
    // The sampler thread (updates) and the UI thread (setMaxHistorySeconds) both publish. They are
    // serialised, so generations are committed in version order with none lost, and a reader never
    // sees a regressed or misaligned one (#868).
    constexpr std::size_t INTERFACES = 4;
    constexpr int SAMPLES = 300;
    constexpr int RESIZES = 300;
    constexpr double STEP_SECONDS = static_cast<double>(Domain::Sampling::REFRESH_INTERVAL_MIN_MS) / 1000.0;

    Domain::SystemModel model(std::make_unique<MockSystemProbe>());
    model.updateFromCounters(makeManyInterfaces(INTERFACES, 1), 1000.0); // publish once so resizes republish

    std::atomic<bool> stop{false};
    std::thread sampler(
        [&]
        {
            double now = 1000.0;
            for (int i = 0; i < SAMPLES; ++i)
            {
                model.updateFromCounters(makeManyInterfaces(INTERFACES, static_cast<std::uint64_t>(i) + 2), now += STEP_SECONDS);
            }
        });
    std::thread resizer(
        [&]
        {
            for (int i = 0; i < RESIZES; ++i)
            {
                model.setMaxHistorySeconds((i % 2 == 0) ? Domain::Sampling::HISTORY_SECONDS_MIN
                                                        : Domain::Sampling::HISTORY_SECONDS_DEFAULT);
            }
        });
    std::thread reader(
        [&]
        {
            std::uint64_t lastSeen = 0;
            while (!stop.load())
            {
                const std::uint64_t announced = model.publicationVersion();
                const auto publication = model.publication();
                EXPECT_GE(publication->version, lastSeen);
                EXPECT_GE(publication->version, announced);
                EXPECT_TRUE(isAligned(*publication));
                lastSeen = publication->version;
            }
        });
    sampler.join();
    resizer.join();
    stop.store(true);
    reader.join();

    EXPECT_EQ(model.publicationVersion(), static_cast<std::uint64_t>(1 + SAMPLES + RESIZES));
    EXPECT_EQ(model.publication()->version, model.publicationVersion());
}

// ==========================================================================
// Interface Lookups at Container-Host Scale (#1415)
// ==========================================================================

TEST(SystemModelTest, InterfaceRatesAndGapsHoldAtHundredsOfChurningInterfaces)
{
    // Rates are matched to the previous sample by name and gaps by presence, through sorted
    // indexes rather than linear scans (#1415). Same results at container-host scale: 500 veth
    // interfaces, then listed in reverse order with a fifth departing and 100 arriving.
    constexpr std::size_t KNOWN = 500;
    constexpr std::size_t ARRIVING = 100;
    const auto name = [](std::size_t i)
    {
        return "veth" + std::to_string(i);
    };
    const auto departs = [](std::size_t i)
    {
        return i % 5 == 0;
    };
    // Interface i moves (i + 1) * 10 bytes received and (i + 1) * 5 sent per second.
    const auto countersAt = [&](std::uint64_t second, bool churned)
    {
        std::vector<Platform::SystemCounters::InterfaceCounters> interfaces;
        for (std::size_t i = 0; i < KNOWN + ARRIVING; ++i)
        {
            const bool present = churned ? !(i < KNOWN && departs(i)) : (i < KNOWN);
            if (present)
            {
                interfaces.push_back(makeInterfaceCounters(name(i), (i + 1) * 10 * second, (i + 1) * 5 * second));
            }
        }
        if (churned)
        {
            std::ranges::reverse(interfaces);
        }
        return makeSystemCounters(
            makeCpuCounters(100 * second, 0, 50 * second, 850 * second), makeMemoryCounters(1024, 512), 0, {}, 0, 0, std::move(interfaces));
    };

    Domain::SystemModel model(std::make_unique<MockSystemProbe>());
    model.updateFromCounters(countersAt(1, false), 1.0); // no history yet: nothing to take a delta against
    model.updateFromCounters(countersAt(2, false), 2.0); // history[0]
    model.updateFromCounters(countersAt(3, true), 3.0);  // history[1]: churned and reordered
    model.updateFromCounters(countersAt(4, true), 4.0);  // history[2]

    const auto snap = model.snapshot();
    ASSERT_EQ(snap.networkInterfaces.size(), KNOWN - (KNOWN / 5) + ARRIVING);
    for (const auto& iface : snap.networkInterfaces)
    {
        const std::size_t i = std::stoul(iface.name.substr(4));
        EXPECT_DOUBLE_EQ(iface.rxBytesPerSec, static_cast<double>((i + 1) * 10)) << iface.name;
        EXPECT_DOUBLE_EQ(iface.txBytesPerSec, static_cast<double>((i + 1) * 5)) << iface.name;
    }

    const auto publication = model.publication();
    const std::size_t samples = publication->timestamps.size();
    ASSERT_EQ(samples, 3U);
    ASSERT_EQ(publication->perInterfaceRxHistory.size(), KNOWN + ARRIVING);
    for (std::size_t i = 0; i < KNOWN + ARRIVING; ++i)
    {
        const auto& rx = publication->perInterfaceRxHistory.at(name(i));
        const auto rate = static_cast<float>((i + 1) * 10);
        ASSERT_EQ(rx.size(), samples) << name(i);
        if (i < KNOWN && departs(i))
        {
            EXPECT_FLOAT_EQ(rx[0], rate) << name(i);
            EXPECT_TRUE(std::isnan(rx[1]) && std::isnan(rx[2])) << name(i) << " departed: a gap, not a rate";
        }
        else if (i < KNOWN)
        {
            EXPECT_FLOAT_EQ(rx[0], rate) << name(i);
            EXPECT_FLOAT_EQ(rx[1], rate) << name(i);
            EXPECT_FLOAT_EQ(rx[2], rate) << name(i);
        }
        else
        {
            EXPECT_TRUE(std::isnan(rx[0])) << name(i) << " arrived later: backfilled with a gap";
            EXPECT_FLOAT_EQ(rx[1], 0.0F) << name(i) << " arrived: no rate before its second sample";
            EXPECT_FLOAT_EQ(rx[2], rate) << name(i);
        }
    }
}

TEST(SystemModelTest, ARepeatedInterfaceNameTakesItsRateFromTheFirstPreviousEntry)
{
    // The sorted lookup keeps the linear scan's answer when a probe repeats a name (#1415).
    Domain::SystemModel model(std::make_unique<MockSystemProbe>());
    model.updateFromCounters(makeSystemCounters(makeCpuCounters(100, 0, 50, 850),
                                                makeMemoryCounters(1024, 512),
                                                0,
                                                {},
                                                0,
                                                0,
                                                {makeInterfaceCounters("eth0", 1000, 100), makeInterfaceCounters("eth0", 5000, 500)}),
                             1.0);
    model.updateFromCounters(
        makeSystemCounters(
            makeCpuCounters(200, 0, 100, 1700), makeMemoryCounters(1024, 512), 0, {}, 0, 0, {makeInterfaceCounters("eth0", 6000, 600)}),
        2.0);

    // Matched against the first eth0 (1000 / 100 bytes), not the second (5000 / 500).
    const auto snap = model.snapshot();
    ASSERT_EQ(snap.networkInterfaces.size(), 1U);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].rxBytesPerSec, 5000.0);
    EXPECT_DOUBLE_EQ(snap.networkInterfaces[0].txBytesPerSec, 500.0);
}
