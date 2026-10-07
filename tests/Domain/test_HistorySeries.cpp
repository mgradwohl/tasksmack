/// @file test_HistorySeries.cpp
/// @brief The models' series APIs for history preloads (#1413): SystemModel::updateFromCounterSeries(),
/// StorageModel::sampleSeries() and ProcessModel::appendSystemHistory() apply many samples with one
/// publish, keep the history in time order, and leave the model ready for live samples.

#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "Mocks/MockDiskProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/StorageTypes.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace
{

using TestMocks::makeCpuCounters;
using TestMocks::makeMemoryAtUsage;
using TestMocks::makeSystemCounters;

/// SystemCounters for step @p step: a quarter of every tick busy.
[[nodiscard]] Platform::SystemCounters systemCountersAt(std::uint64_t step)
{
    const std::uint64_t ticks = step * 100;
    return makeSystemCounters(makeCpuCounters(ticks / 4, 0, 0, ticks - (ticks / 4)), makeMemoryAtUsage(40.0));
}

[[nodiscard]] Platform::SystemDiskCounters diskCountersAt(std::uint64_t step)
{
    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.readSectors = step * 2048;
    disk.writeSectors = step * 1024;
    disk.readsCompleted = step * 4;
    disk.writesCompleted = step * 2;
    counters.disks.push_back(disk);
    return counters;
}

constexpr double START_SECONDS = 5000.0;
constexpr double STEP_SECONDS = static_cast<double>(Domain::Sampling::REFRESH_INTERVAL_MIN_MS) / 1000.0;

} // namespace

TEST(SystemModelSeriesTest, AppliesEverySampleWithOnePublish)
{
    auto probe = std::make_unique<TestMocks::MockSystemProbe>();
    probe->setCapabilities(TestMocks::makeFullSystemCapabilities());
    Domain::SystemModel model(std::move(probe));
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MAX);

    constexpr std::size_t READINGS = 50;
    std::size_t index = 0;
    model.updateFromCounterSeries(
        [&](Platform::SystemCounters& counters, double& nowSeconds)
        {
            if (index >= READINGS)
            {
                return false;
            }
            counters = systemCountersAt(index);
            nowSeconds = START_SECONDS + (static_cast<double>(index) * STEP_SECONDS);
            ++index;
            return true;
        });

    // The first reading is the delta baseline; every later one is a history sample.
    EXPECT_EQ(model.timestamps().size(), READINGS - 1);
    EXPECT_EQ(model.publicationVersion(), 1U);
    const auto publication = model.publication();
    ASSERT_EQ(publication->cpuHistory.size(), READINGS - 1);
    EXPECT_NEAR(publication->cpuHistory.back(), 25.0F, 0.5F);

    // Live samples continue the series.
    model.updateFromCounters(systemCountersAt(READINGS), START_SECONDS + (static_cast<double>(READINGS) * STEP_SECONDS));
    EXPECT_EQ(model.timestamps().size(), READINGS);
    EXPECT_EQ(model.publicationVersion(), 2U);
}

TEST(SystemModelSeriesTest, SkipsSamplesNotLaterThanTheNewest)
{
    Domain::SystemModel model(std::make_unique<TestMocks::MockSystemProbe>());
    model.updateFromCounters(systemCountersAt(10), START_SECONDS);
    model.updateFromCounters(systemCountersAt(11), START_SECONDS + STEP_SECONDS);
    ASSERT_EQ(model.timestamps().size(), 1U);

    const std::vector<double> times = {START_SECONDS - 1.0, START_SECONDS + STEP_SECONDS, START_SECONDS + (2.0 * STEP_SECONDS)};
    std::size_t index = 0;
    model.updateFromCounterSeries(
        [&](Platform::SystemCounters& counters, double& nowSeconds)
        {
            if (index >= times.size())
            {
                return false;
            }
            counters = systemCountersAt(12 + index);
            nowSeconds = times[index++];
            return true;
        });
    const auto timestamps = model.timestamps();
    ASSERT_EQ(timestamps.size(), 2U);
    EXPECT_DOUBLE_EQ(timestamps.back(), START_SECONDS + (2.0 * STEP_SECONDS));
}

TEST(SystemModelSeriesTest, SkipsSamplesNotLaterThanALoneSeedReading)
{
    // One seed reading: a previous reading exists but the history is still empty.
    Domain::SystemModel model(std::make_unique<TestMocks::MockSystemProbe>());
    model.updateFromCounters(systemCountersAt(10), START_SECONDS);
    ASSERT_TRUE(model.timestamps().empty());

    const std::vector<double> times = {START_SECONDS - 1.0, START_SECONDS, START_SECONDS + STEP_SECONDS};
    std::size_t index = 0;
    model.updateFromCounterSeries(
        [&](Platform::SystemCounters& counters, double& nowSeconds)
        {
            if (index >= times.size())
            {
                return false;
            }
            counters = systemCountersAt(11 + index);
            nowSeconds = times[index++];
            return true;
        });
    const auto timestamps = model.timestamps();
    ASSERT_EQ(timestamps.size(), 1U);
    EXPECT_DOUBLE_EQ(timestamps.front(), START_SECONDS + STEP_SECONDS);
    const auto publication = model.publication();
    ASSERT_EQ(publication->cpuHistory.size(), 1U);
    EXPECT_GE(publication->cpuHistory.front(), 0.0F);
    EXPECT_LE(publication->cpuHistory.front(), 100.0F);
}

TEST(SystemModelSeriesTest, EmptySeriesPublishesNothing)
{
    Domain::SystemModel model(std::make_unique<TestMocks::MockSystemProbe>());
    model.updateFromCounterSeries([](Platform::SystemCounters& /*counters*/, double& /*nowSeconds*/) { return false; });
    EXPECT_EQ(model.publicationVersion(), 0U);
    EXPECT_TRUE(model.timestamps().empty());
}

TEST(StorageModelSeriesTest, AppliesEverySampleWithOnePublish)
{
    auto probe = std::make_unique<Mocks::MockDiskProbe>();
    probe->setCapabilities(Platform::DiskCapabilities{.hasDiskStats = true, .hasReadWriteBytes = true});
    Domain::StorageModel model(std::move(probe));
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MAX);

    constexpr std::size_t READINGS = 40;
    const auto start = std::chrono::steady_clock::time_point{} + std::chrono::seconds(5000);
    std::size_t index = 0;
    model.sampleSeries(
        [&](Platform::SystemDiskCounters& counters, std::chrono::steady_clock::time_point& now)
        {
            if (index >= READINGS)
            {
                return false;
            }
            counters = diskCountersAt(index);
            now = start + (std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS) * index);
            ++index;
            return true;
        });

    // Every reading is a timestamp; the first has no rates yet (a gap).
    EXPECT_EQ(model.historyTimestamps().size(), READINGS);
    EXPECT_EQ(model.publicationVersion(), 1U);
    const auto publication = model.publication();
    ASSERT_EQ(publication->perDiskHistory.size(), 1U);
    ASSERT_EQ(publication->perDiskHistory.front().readBytesPerSec.size(), READINGS);
    // 2048 sectors of 512 bytes per 100 ms.
    EXPECT_NEAR(publication->perDiskHistory.front().readBytesPerSec.back(), 2048.0 * 512.0 * 10.0, 1.0);

    // A sample at or before the newest is skipped.
    index = 0;
    model.sampleSeries(
        [&](Platform::SystemDiskCounters& counters, std::chrono::steady_clock::time_point& now)
        {
            if (index++ > 0)
            {
                return false;
            }
            counters = diskCountersAt(READINGS);
            now = start;
            return true;
        });
    EXPECT_EQ(model.historyTimestamps().size(), READINGS);
    EXPECT_EQ(model.publicationVersion(), 1U);
}

TEST(ProcessModelSeriesTest, AppendsSystemHistoryAndAdvancesItsGenerationOnce)
{
    Domain::ProcessModel model(std::make_unique<TestMocks::MockProcessProbe>());
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MAX);

    constexpr std::size_t SAMPLES = 30;
    std::size_t index = 0;
    model.appendSystemHistory(
        [&](Domain::ProcessSystemHistorySample& sample)
        {
            if (index >= SAMPLES)
            {
                return false;
            }
            sample = Domain::ProcessSystemHistorySample{.timeSeconds = START_SECONDS + (static_cast<double>(index) * STEP_SECONDS),
                                                        .netSentBytesPerSec = 1.0,
                                                        .netReceivedBytesPerSec = 2.0,
                                                        .pageFaultsPerSec = 3.0,
                                                        .threadCount = 400.0 + static_cast<double>(index),
                                                        .handleCount = 5.0,
                                                        .powerWatts = 6.0};
            ++index;
            return true;
        });

    Domain::ProcessSystemHistories histories;
    ASSERT_TRUE(model.tryCopySystemHistoriesIfNewer(0, histories));
    EXPECT_EQ(histories.version, 1U);
    ASSERT_EQ(histories.timestamps.size(), SAMPLES);
    ASSERT_EQ(histories.threadCount.size(), SAMPLES);
    EXPECT_DOUBLE_EQ(histories.threadCount.back(), 400.0 + static_cast<double>(SAMPLES - 1));
    EXPECT_DOUBLE_EQ(histories.power.front(), 6.0);
    EXPECT_EQ(model.systemNetRecvHistory().size(), SAMPLES);

    // Out-of-order samples are skipped, and nothing appended leaves the generation alone.
    model.appendSystemHistory(
        [&, done = false](Domain::ProcessSystemHistorySample& sample) mutable
        {
            if (done)
            {
                return false;
            }
            done = true;
            sample.timeSeconds = START_SECONDS;
            return true;
        });
    EXPECT_FALSE(model.tryCopySystemHistoriesIfNewer(histories.version, histories));
    EXPECT_EQ(model.historyTimestamps().size(), SAMPLES);
}
