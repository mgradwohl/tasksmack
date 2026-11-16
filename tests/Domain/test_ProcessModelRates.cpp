/// @file test_ProcessModelRates.cpp
/// @brief Domain::ProcessModel: network, socket-traffic, power and I/O rates, edge cases, the builder, handle counts and start times.
/// Split from test_ProcessModel.cpp by feature (#3850); the shared helpers are in ProcessModelTestSupport.h.

#include "Domain/GPUModel.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/GPUTypes.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ImageSignature.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SchedPolicy.h"
#include "ProcessModelTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Use shared mock from TestMocks namespace
using ProcessModelTesting::makeCounter;
using ProcessModelTesting::ManualClock;
using ProcessModelTesting::OVERFLOW_TEST_MARGIN;
using ProcessModelTesting::systemHistories;
using TestMocks::makeProcessCounters;
using TestMocks::MockProcessProbe;

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

namespace
{

// Drives a ProcessModel's sample times through the injectable clock, so the network rate tests
// get exact intervals instead of depending on a real sleep.
struct NetworkRateFixture
{
    Domain::ProcessModel::Clock::time_point currentTime;
    MockProcessProbe* probe = nullptr;
    std::unique_ptr<Domain::ProcessModel> model;
    std::uint64_t totalCpuTime = 100000;

    NetworkRateFixture()
    {
        auto owned = std::make_unique<MockProcessProbe>();
        probe = owned.get();
        model = std::make_unique<Domain::ProcessModel>(std::move(owned), [this] { return currentTime; });
    }

    // Advances the clock by @p elapsed (none for the first sample), then samples one process
    // whose open connections have sent and received the given byte totals.
    // @p netSampleTimeNs: when the probe read the network counters (0 = with this refresh).
    auto sample(std::chrono::milliseconds elapsed, std::uint64_t sent, std::uint64_t received, std::uint64_t netSampleTimeNs = 0)
        -> Domain::ProcessSnapshot
    {
        currentTime += elapsed;
        totalCpuTime += 100000;
        probe->setCounters({});
        probe->withProcess(100, "network_proc").withNetworkCounters(100, sent, received).withNetworkSampleTime(100, netSampleTimeNs);
        probe->setTotalCpuTime(totalCpuTime);
        model->refresh();
        const auto snaps = model->snapshots();
        EXPECT_EQ(snaps.size(), 1U);
        return snaps.empty() ? Domain::ProcessSnapshot{} : snaps.front();
    }
};

} // namespace

TEST(ProcessModelTest, NetworkRatesCalculatedFromDeltas)
{
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 1000, 2000);

    const auto snap = fixture.sample(std::chrono::milliseconds{500}, 2000, 4000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 2000.0);     // 1000 bytes / 0.5 s
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 4000.0); // 2000 bytes / 0.5 s
}

TEST(ProcessModelTest, NetworkRatesFollowTheLastIntervalNotTheLifetimeAverage)
{
    // #1036: the rate was (bytes now - bytes when first seen) / time since first seen, so a
    // burst decayed slowly instead of dropping to zero when the transfer stopped.
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    auto snap = fixture.sample(std::chrono::seconds{1}, 1'000'000, 3'000'000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 1'000'000.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 3'000'000.0);

    // Transfer stopped: the lifetime average would still read 500 KB/s and 1.5 MB/s here.
    snap = fixture.sample(std::chrono::seconds{1}, 1'000'000, 3'000'000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);

    // A later, smaller transfer shows at its own rate, not diluted by the time already watched.
    snap = fixture.sample(std::chrono::seconds{1}, 1'010'000, 3'020'000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 10'000.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 20'000.0);
}

TEST(ProcessModelTest, NetworkRatesHandleCounterDecrease)
{
    // The counters are sums over live connections; one closing makes the sum drop. That interval
    // reads 0, not a wrapped (huge) or negative rate, and the next interval is measured from the
    // lower sum.
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 2000, 4000);

    auto snap = fixture.sample(std::chrono::seconds{1}, 500, 1000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);

    snap = fixture.sample(std::chrono::seconds{1}, 1500, 1500);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 500.0);
}

TEST(ProcessModelTest, NetworkRatesZeroWhenTheReadStampGoesBackwards)
{
    // #1395: a probe whose read stamp went back (a clock reset under it) gives no interval to measure
    // over: that refresh reads 0, not a rate over a negative or wrapped time.
    NetworkRateFixture fixture;
    constexpr std::uint64_t SECOND_NS = 1'000'000'000;
    fixture.sample(std::chrono::milliseconds{0}, 1000, 2000, 10 * SECOND_NS);
    auto snap = fixture.sample(std::chrono::seconds{1}, 3000, 6000, 11 * SECOND_NS);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 2000.0);

    snap = fixture.sample(std::chrono::seconds{1}, 5000, 10000, 5 * SECOND_NS);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);
}

TEST(ProcessModelTest, NetworkRatesZeroForImplausiblyShortInterval)
{
    // An interval under half the minimum refresh interval (the seed refresh followed at once by
    // the sampler's first) is treated as no previous data, so it cannot divide by a tiny time.
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 1000, 2000);

    auto snap = fixture.sample(std::chrono::milliseconds{1}, 5000, 10000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);

    // From a nonzero rate (#1063 review): a too-short interval resets the rate to 0, not the last
    // rate republished -- only a repeated cached read from a probe that stamps its reads holds it.
    snap = fixture.sample(std::chrono::seconds{1}, 6000, 12000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 1000.0);
    snap = fixture.sample(std::chrono::milliseconds{1}, 6000, 12000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);
}

TEST(ProcessModelTest, NetworkRatesUseTheProbeReadIntervalWhenItCaches)
{
    // #1063 review: the Linux probe caches its socket query for 500 ms. With 100 ms refreshes a steady
    // 1 MB/s transfer read as four refreshes of 0 and then one of 5 MB/s when the rate was taken over
    // the refresh interval. Over the time between the probe's reads, it is 1 MB/s throughout.
    constexpr std::uint64_t MS = 1'000'000; // ns
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 0, 0, 1000 * MS);

    constexpr auto REFRESH = std::chrono::milliseconds{100};
    auto snap = fixture.sample(REFRESH, 0, 0, 1000 * MS); // still the cached read
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);

    // Fresh read 500 ms after the first: 500 KB over 0.5 s.
    snap = fixture.sample(REFRESH, 0, 500'000, 1500 * MS);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000'000.0);

    // Cached refreshes after it hold the rate rather than reading 0.
    for (int i = 0; i < 4; ++i)
    {
        snap = fixture.sample(REFRESH, 0, 500'000, 1500 * MS);
        EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000'000.0);
    }

    // The next fresh read: another 500 KB over 0.5 s, not 5x the rate.
    snap = fixture.sample(REFRESH, 0, 1'000'000, 2000 * MS);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000'000.0);

    // Transfer stopped: the next fresh read drops it to 0.
    snap = fixture.sample(REFRESH, 0, 1'000'000, 2500 * MS);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);
}

TEST(ProcessModelTest, NetworkRatesAboveSanityCeilingAreDropped)
{
    // A connection that appears carrying traffic from before it was first seen can add far more
    // bytes than one interval could carry; such a rate is reported as 0, not as a spike.
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    const auto huge = static_cast<std::uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_DEFAULT) * 2U;
    const auto snap = fixture.sample(std::chrono::seconds{1}, huge, 1000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1000.0);
}

TEST(ProcessModelTest, ConfiguredSanityCeilingRaisesTheLimit)
{
    // [metrics] max_sane_rate_bps (#1123): raised for a fast link, a rate above the default 100 Gbps
    // ceiling but below the configured one is reported, not dropped.
    NetworkRateFixture fixture;
    fixture.model->setMaxSaneNetworkRate(Domain::Sampling::MAX_SANE_RATE_BPS_MAX);
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    const auto aboveDefault = static_cast<std::uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_DEFAULT) * 2U;
    const auto snap = fixture.sample(std::chrono::seconds{1}, aboveDefault, 0);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, static_cast<double>(aboveDefault));
}

TEST(ProcessModelTest, ConfiguredSanityCeilingLowersTheLimit)
{
    NetworkRateFixture fixture;
    fixture.model->setMaxSaneNetworkRate(Domain::Sampling::MAX_SANE_RATE_BPS_MIN);
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    const auto aboveMin = static_cast<std::uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_MIN) * 2U;
    const auto snap = fixture.sample(std::chrono::seconds{1}, aboveMin, 1000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1000.0);
}

TEST(ProcessModelTest, ConfiguredSanityCeilingIsClamped)
{
    // Out-of-range values (a NaN, a ceiling of 0) take the nearest bound rather than disabling
    // network rates or the check.
    NetworkRateFixture fixture;
    fixture.model->setMaxSaneNetworkRate(0.0);
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    const auto belowMin = static_cast<std::uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_MIN) / 2U;
    auto snap = fixture.sample(std::chrono::seconds{1}, belowMin, 0);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, static_cast<double>(belowMin));

    fixture.model->setMaxSaneNetworkRate(std::numeric_limits<double>::quiet_NaN());
    snap = fixture.sample(std::chrono::seconds{1}, belowMin * 2U, 0);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, static_cast<double>(belowMin));
}

namespace
{

// Drives a ProcessModel whose probe reports per-connection readings (readSocketTraffic(), as the
// Linux probe does) instead of per-process network counters, so the model accumulates them (#1099).
struct SocketTrafficFixture
{
    static constexpr std::uint64_t MS = 1'000'000; // ns
    static constexpr std::int32_t PID = 100;

    Domain::ProcessModel::Clock::time_point currentTime;
    MockProcessProbe* probe = nullptr;
    std::unique_ptr<Domain::ProcessModel> model;
    std::uint64_t totalCpuTime = 100000;

    SocketTrafficFixture()
    {
        auto owned = std::make_unique<MockProcessProbe>();
        probe = owned.get();
        probe->withProcess(PID, "network_proc");
        model = std::make_unique<Domain::ProcessModel>(std::move(owned), [this] { return currentTime; });
    }

    // Advances the clock by one second, then refreshes with `reading` as the probe's socket reading.
    auto sample(Platform::SocketTrafficReading reading) -> Domain::ProcessSnapshot
    {
        currentTime += std::chrono::seconds{1};
        totalCpuTime += 100000;
        probe->setTotalCpuTime(totalCpuTime);
        probe->setSocketTraffic(std::move(reading));
        model->refresh();
        const auto snaps = model->snapshots();
        EXPECT_EQ(snaps.size(), 1U);
        return snaps.empty() ? Domain::ProcessSnapshot{} : snaps.front();
    }
};

} // namespace

TEST(ProcessModelTest, AClosingSocketDoesNotZeroTheProcessNetworkRate)
{
    // #1099: the per-process counter was the sum over the process's live sockets, so when socket 12
    // closed it dropped from 6000 to 3000 and the process read 0 B/s for the interval in which
    // socket 11 moved 2000 bytes. Accumulated from per-socket deltas it only ever grows.
    constexpr std::uint64_t MS = SocketTrafficFixture::MS;
    constexpr std::int32_t PID = SocketTrafficFixture::PID;
    SocketTrafficFixture fixture;
    fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 1'000}, {.key = 12, .pid = PID, .bytesReceived = 5'000}},
                    .sampleTimeNs = 1000 * MS});

    auto snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 3'000}}, .sampleTimeNs = 2000 * MS}); // 12 closed
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 2'000.0);

    snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 6'000}}, .sampleTimeNs = 3000 * MS});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 3'000.0);
}

TEST(ProcessModelTest, AnOlderSocketReadingIsNotFolded)
{
    // A reading older than the last one folded must not rewind the socket baselines: folded, socket
    // 11 back at 1000 and then at 4000 again would count the 2000 bytes from 1000 to 3000 twice.
    constexpr std::uint64_t MS = SocketTrafficFixture::MS;
    constexpr std::int32_t PID = SocketTrafficFixture::PID;
    SocketTrafficFixture fixture;
    fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 1'000}}, .sampleTimeNs = 1000 * MS});
    auto snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 3'000}}, .sampleTimeNs = 2000 * MS});
    ASSERT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 2'000.0);

    snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 1'000}}, .sampleTimeNs = 1500 * MS});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 2'000.0) << "the stale reading holds the last rate";

    snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 4'000}}, .sampleTimeNs = 3000 * MS});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000.0) << "1000 bytes over the second since the last folded reading";
}

TEST(ProcessModelTest, AFailedSocketReadingHoldsTheLastNetworkRate)
{
    // A failed reading (sampleTimeNs 0) republishes the last totals and their time: the rate holds,
    // and the next reading is measured over the time since the last good one, not as a burst.
    constexpr std::uint64_t MS = SocketTrafficFixture::MS;
    constexpr std::int32_t PID = SocketTrafficFixture::PID;
    SocketTrafficFixture fixture;
    fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 0}}, .sampleTimeNs = 1000 * MS});
    auto snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 1'000}}, .sampleTimeNs = 2000 * MS});
    ASSERT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000.0);

    snap = fixture.sample({});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000.0);

    snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 3'000}}, .sampleTimeNs = 4000 * MS});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000.0); // 2000 bytes over the 2 s since the last good reading
}

// =============================================================================
// Power Usage Calculation Tests
// =============================================================================

TEST(ProcessModelTest, FirstRefreshShowsZeroPowerUsage)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(100, "test_proc").withPowerUsage(100, 1'000'000); // 1M microjoules
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // First sample has no previous data, so power should be 0
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 0.0);
}

TEST(ProcessModelTest, PowerUsageCalculationFromEnergyDelta)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First refresh: energy = 1,000,000 microjoules (1 joule)
    rawProbe->withProcess(100, "power_proc").withPowerUsage(100, 1'000'000);
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(100));

    // Second refresh: energy increased by 100,000 microjoules (0.1 joule)
    // If 100ms passed, power = 0.1J / 0.1s = 1W
    rawProbe->withProcess(100, "power_proc").withPowerUsage(100, 1'100'000);
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // 0.1 J over exactly 0.1 s
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 1.0);
}

// #1093: a probe that reports a package energy counter (Linux RAPL) has it shared out per interval
// by each process's CPU time in that interval, under ProcessModel's sampling lock.
TEST(ProcessModelTest, PackageEnergyIsSharedByIntervalCpuTime)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withProcess(1, "busy").withCpuTime(1, 100, 0);
    rawProbe->withProcess(2, "idle_daemon").withCpuTime(2, 1'000'000, 0); // lots of lifetime CPU
    rawProbe->setTotalCpuTime(10'000);
    rawProbe->setPackageEnergy(
        Platform::PackageEnergyReading{.energyUj = 5'000'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {}});

    Domain::ProcessModel::Clock::time_point now{};
    Domain::ProcessModel model(std::move(probe), [&now] { return now; });
    model.refresh();

    // One second later: the busy process used all the CPU, and the package used 2 J.
    now += std::chrono::seconds(1);
    rawProbe->withCpuTime(1, 200, 0);
    rawProbe->setTotalCpuTime(10'100);
    rawProbe->setPackageEnergy(
        Platform::PackageEnergyReading{.energyUj = 7'000'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {}});
    model.refresh();

    const auto snaps = model.snapshots();
    const auto busy = std::ranges::find_if(snaps, [](const Domain::ProcessSnapshot& s) { return s.pid == 1; });
    const auto idle = std::ranges::find_if(snaps, [](const Domain::ProcessSnapshot& s) { return s.pid == 2; });
    ASSERT_NE(busy, snaps.end());
    ASSERT_NE(idle, snaps.end());
    EXPECT_DOUBLE_EQ(busy->powerWatts, 2.0);
    EXPECT_DOUBLE_EQ(idle->powerWatts, 0.0);
}

// #3344: package-derived power divides by the time between the package counter's own reads, which come after the
// socket read: with enumerations at 0 s and 1 s and socket reads taking 400 ms, then 50 ms, a constant 10 W package
// is read at 0.4 s (4 J) and 1.05 s (10.5 J), so 6.5 J over 0.65 s is 10 W, not 6.5 W over the enumerations' 1 s.
TEST(ProcessModelTest, PackagePowerDividesByTheTimeBetweenItsOwnReads)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withProcess(1, "busy").withCpuTime(1, 100, 0);
    rawProbe->setTotalCpuTime(10'000);
    rawProbe->setPackageEnergy(
        Platform::PackageEnergyReading{.energyUj = 4'000'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {}});

    Domain::ProcessModel::Clock::time_point now{};
    auto socketReadTakes = std::chrono::milliseconds(400);
    rawProbe->setOnSocketRead([&now, &socketReadTakes] { now += socketReadTakes; });
    Domain::ProcessModel model(std::move(probe), [&now] { return now; });
    model.refresh();

    now = Domain::ProcessModel::Clock::time_point{std::chrono::seconds(1)};
    socketReadTakes = std::chrono::milliseconds(50);
    rawProbe->withCpuTime(1, 200, 0);
    rawProbe->setTotalCpuTime(10'100);
    rawProbe->setPackageEnergy(
        Platform::PackageEnergyReading{.energyUj = 10'500'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {}});
    model.refresh();

    const auto snaps = model.snapshots();
    const auto busy = std::ranges::find_if(snaps, [](const Domain::ProcessSnapshot& s) { return s.pid == 1; });
    ASSERT_NE(busy, snaps.end());
    EXPECT_NEAR(busy->powerWatts, 10.0, 1e-9);
}

// #3344: a process carried over a sample, then read again, takes its package-derived power over the time between the
// package reads of its last real sample and this one, not between the enumerations. A constant 10 W package is read
// at 0.4 s (4 J), 1.1 s (11 J) and 2.05 s (20.5 J), the socket reads taking 400, 100 and 50 ms: all 16.5 J since the
// first read are the process's, over 1.65 s, so 10 W, not 8.25 W over the enumerations' 2 s.
TEST(ProcessModelTest, AHeldOverProcessTakesItsPackagePowerOverThePackageReads)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    Platform::ProcessCounters busy = makeCounter(1, "busy", 'R', 100, 0);
    rawProbe->setCounters({busy});
    rawProbe->setTotalCpuTime(10'000);
    rawProbe->setPackageEnergy(
        Platform::PackageEnergyReading{.energyUj = 4'000'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {}});

    Domain::ProcessModel::Clock::time_point now{};
    auto socketReadTakes = std::chrono::milliseconds(400);
    rawProbe->setOnSocketRead([&now, &socketReadTakes] { now += socketReadTakes; });
    Domain::ProcessModel model(std::move(probe), [&now] { return now; });
    model.refresh();

    // Carried over: its record isn't a fresh read, though the package's energy is shared out by it as ever.
    now = Domain::ProcessModel::Clock::time_point{std::chrono::seconds(1)};
    socketReadTakes = std::chrono::milliseconds(100);
    Platform::ProcessCounters carried = makeCounter(1, "busy", 'R', 200, 0);
    carried.heldOver = true;
    rawProbe->setCounters({carried});
    rawProbe->setTotalCpuTime(10'100);
    rawProbe->setPackageEnergy(
        Platform::PackageEnergyReading{.energyUj = 11'000'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {}});
    model.refresh();

    now = Domain::ProcessModel::Clock::time_point{std::chrono::seconds(2)};
    socketReadTakes = std::chrono::milliseconds(50);
    busy.userTime = 300;
    rawProbe->setCounters({busy});
    rawProbe->setTotalCpuTime(10'200);
    rawProbe->setPackageEnergy(
        Platform::PackageEnergyReading{.energyUj = 20'500'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {}});
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_NEAR(snaps[0].powerWatts, 10.0, 1e-9);
}

// #2444 (#2479): a multi-socket machine's other packages reach the attribution: package 0 used 2 J and
// package 1 3 J, so the busy process draws 5 W, not package 0's 2 W.
TEST(ProcessModelTest, EveryPackagesEnergyIsSharedOut)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withProcess(1, "busy").withCpuTime(1, 100, 0);
    rawProbe->setTotalCpuTime(10'000);
    rawProbe->setPackageEnergy(Platform::PackageEnergyReading{
        .energyUj = 5'000'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {{.energyUj = 1'000'000, .maxRangeUj = 0}}});

    Domain::ProcessModel::Clock::time_point now{};
    Domain::ProcessModel model(std::move(probe), [&now] { return now; });
    model.refresh();

    now += std::chrono::seconds(1);
    rawProbe->withCpuTime(1, 200, 0);
    rawProbe->setTotalCpuTime(10'100);
    rawProbe->setPackageEnergy(Platform::PackageEnergyReading{
        .energyUj = 7'000'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {{.energyUj = 4'000'000, .maxRangeUj = 0}}});
    model.refresh();

    const auto snaps = model.snapshots();
    const auto busy = std::ranges::find_if(snaps, [](const Domain::ProcessSnapshot& s) { return s.pid == 1; });
    ASSERT_NE(busy, snaps.end());
    EXPECT_DOUBLE_EQ(busy->powerWatts, 5.0);
}

TEST(ProcessModelTest, PowerUsageWithZeroEnergyDelta)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Process with constant energy (no power consumption)
    rawProbe->withProcess(100, "idle_proc").withPowerUsage(100, 1'000'000);
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

    // Energy unchanged
    rawProbe->withProcess(100, "idle_proc").withPowerUsage(100, 1'000'000);
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 0.0);
}

TEST(ProcessModelTest, PowerUsageHandlesEnergyCounterReset)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First reading
    rawProbe->withProcess(100, "proc").withPowerUsage(100, 5'000'000);
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

    // Counter decreased (reset or wrap) - should be handled gracefully
    rawProbe->withProcess(100, "proc").withPowerUsage(100, 1'000'000);
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // When energy counter decreases, no power is calculated (0 or skipped)
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 0.0);
}

TEST(ProcessModelTest, PowerUsageWithoutEnergyData)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withProcess(100, "no_power_proc");
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

    rawProbe->withProcess(100, "no_power_proc");
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 0.0);
}

TEST(ProcessModelTest, BuilderPatternWithPowerUsage)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(123, "power_test").withPowerUsage(123, 5'000'000).withCpuTime(123, 1000, 500);
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].pid, 123);
    EXPECT_EQ(snaps[0].name, "power_test");
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

// #3344: a rate's interval is between the two samples' acquisitions, taken as enumerate() returns, not when the
// snapshots are computed: a socket-traffic read that takes 400 ms one time and 50 ms the next doesn't change it.
TEST(ProcessModelTest, RatesDivideByTheTimeBetweenReadsNotByTheReadsThatFollow)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    Platform::ProcessCounters first = makeCounter(100, "reader", 'R', 1000, 500);
    first.readBytes = 1024 * 1024;
    rawProbe->setCounters({first});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel::Clock::time_point now{};
    auto socketReadTakes = std::chrono::milliseconds(400);
    rawProbe->setOnSocketRead([&now, &socketReadTakes] { now += socketReadTakes; });
    Domain::ProcessModel model(std::move(probe), [&now] { return now; });
    model.refresh(); // read at 0 s; the socket read ends at 0.4 s

    now = Domain::ProcessModel::Clock::time_point{std::chrono::seconds(1)};
    socketReadTakes = std::chrono::milliseconds(50);
    Platform::ProcessCounters second = makeCounter(100, "reader", 'R', 2000, 1000);
    second.readBytes = 2 * 1024 * 1024;
    rawProbe->setCounters({second});
    rawProbe->setTotalCpuTime(200000);
    model.refresh(); // read at 1 s; the socket read ends at 1.05 s

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // 1 MB over the 1 s between the reads, not over the 0.65 s between the socket reads' ends (1.54 MB/s).
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 1024.0 * 1024.0);
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

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(100));

    // Second sample: process has read 3 MB total (delta = 2 MB), written 1.5 MB total (delta = 1 MB)
    Platform::ProcessCounters c2 = makeCounter(100, "test_proc", 'R', 2000, 1000);
    c2.readBytes = 3 * 1024 * 1024;
    c2.writeBytes = 1536 * 1024;

    rawProbe->setCounters({c2});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Read delta = 2 MB, write delta = 1 MB, over exactly 0.1 s
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 20.0 * 1024.0 * 1024.0);
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 10.0 * 1024.0 * 1024.0);
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

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

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

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(100));

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

    // proc_a read 1 MB in 0.1 s, wrote nothing
    EXPECT_DOUBLE_EQ(snapA->ioReadBytesPerSec, 10.0 * 1024.0 * 1024.0);
    EXPECT_DOUBLE_EQ(snapA->ioWriteBytesPerSec, 0.0);

    // proc_b wrote 2 MB in 0.1 s, read nothing
    EXPECT_DOUBLE_EQ(snapB->ioReadBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snapB->ioWriteBytesPerSec, 20.0 * 1024.0 * 1024.0);
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

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

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

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

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
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 0.0); // No valid previous data
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 0.0);
}

// =============================================================================
// Handle Count Pass-Through Tests
// =============================================================================

TEST(ProcessModelTest, HandleCountIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "handles_test", 'R', 1000, 500);
    counter.handleCount = 42;
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].handleCount, 42);
}

TEST(ProcessModelTest, HandleCountZeroIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "no_handles", 'R', 1000, 500);
    counter.handleCount = 0;
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].handleCount, 0);
}

// =============================================================================
// Start Time Epoch Pass-Through Tests
// =============================================================================

TEST(ProcessModelTest, StartTimeEpochIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "epoch_test", 'R', 1000, 500);
    counter.startTimeEpoch = 1704067200; // 2024-01-01 00:00:00 UTC
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].startTimeEpoch, 1704067200);
}

TEST(ProcessModelTest, StartTimeEpochZeroIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "no_epoch", 'R', 1000, 500);
    counter.startTimeEpoch = 0; // Unknown/unavailable
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].startTimeEpoch, 0);
}

TEST(ProcessModelTest, StartTimeTicksArePassedThrough)
{
    // The raw start time is what process actions verify before acting (#973). If the model ever
    // stopped copying it, every action from the UI would carry an unknown identity and be refused,
    // and neither the dispatch tests (hand-built snapshots) nor the platform contract tests
    // (which bypass the model) would notice.
    auto probe = std::make_unique<MockProcessProbe>();
    constexpr uint64_t START_TICKS = 133'987'654'321'000'000ULL; // A FILETIME-sized value
    probe->setCounters({makeCounter(100, "ticks_test", 'R', 1000, 500, START_TICKS)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].startTimeTicks, START_TICKS);
}
