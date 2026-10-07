/// @file test_ProcessHistoryHelpers.cpp
/// @brief Tests for how Process Details takes the selected process's samples into its history:
/// one point per published generation at the generation's own sample time (#1098), and NaN for a
/// value the probe could not read (#1110).

#include "App/Panels/ProcessDetailsPanel_GpuHelpers.h"
#include "App/Panels/ProcessDetailsPanel_HistoryHelpers.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace App::Detail
{
namespace
{

struct Recorded
{
    double timeSeconds = 0.0;
    std::int32_t pid = 0;
    bool gapBefore = false;
};

[[nodiscard]] Domain::ProcessSample makeSample(std::uint64_t version, double timeSeconds, std::int32_t pid, std::uint64_t key)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.pid = pid;
    snapshot.uniqueKey = key;
    return {.snapshot = std::make_shared<const Domain::ProcessSnapshot>(snapshot),
            .version = version,
            .sampleTimeSeconds = timeSeconds,
            .ioCountersSupported = true,
            .networkCountersSupported = true,
            .gpuPerProcessSupported = true,
            .gpuUtilizationSupported = true,
            .gpuReadFailed = false};
}

[[nodiscard]] std::vector<Recorded>
take(const std::vector<Domain::ProcessSample>& samples, std::int32_t pid, std::uint64_t& key, SampleIntake& intake)
{
    std::vector<Recorded> recorded;
    takeSamples(samples,
                pid,
                key,
                intake,
                [&recorded](const Domain::ProcessSample& sample, bool gapBefore)
                { recorded.push_back({.timeSeconds = sample.sampleTimeSeconds, .pid = sample.snapshot->pid, .gapBefore = gapBefore}); });
    return recorded;
}

TEST(ProcessHistoryHelpersTest, EveryGenerationIsRecordedAtItsOwnSampleTime)
{
    // #1098: the version advanced by 2 between two frames. The pane used to record only the newest,
    // stamped with the frame's time; now both are recorded, each at the time the model sampled it.
    std::uint64_t key = 0xABCU;
    SampleIntake intake;
    auto recorded = take({makeSample(1, 10.0, 42, 0xABCU)}, 42, key, intake);
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_DOUBLE_EQ(recorded[0].timeSeconds, 10.0);

    recorded = take({makeSample(2, 10.1, 42, 0xABCU), makeSample(3, 10.2, 42, 0xABCU)}, 42, key, intake);
    ASSERT_EQ(recorded.size(), 2U);
    EXPECT_DOUBLE_EQ(recorded[0].timeSeconds, 10.1);
    EXPECT_DOUBLE_EQ(recorded[1].timeSeconds, 10.2);
    EXPECT_FALSE(recorded[0].gapBefore);
    EXPECT_FALSE(recorded[1].gapBefore);
    EXPECT_EQ(intake.lastVersion, 3U);
    EXPECT_TRUE(intake.present);
}

TEST(ProcessHistoryHelpersTest, AlreadySeenGenerationsAreNotRecordedAgain)
{
    std::uint64_t key = 0;
    SampleIntake intake;
    static_cast<void>(take({makeSample(1, 1.0, 42, 7), makeSample(2, 2.0, 42, 7)}, 42, key, intake));
    const auto recorded = take({makeSample(2, 2.0, 42, 7), makeSample(3, 3.0, 42, 7)}, 42, key, intake);
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_DOUBLE_EQ(recorded[0].timeSeconds, 3.0);
}

TEST(ProcessHistoryHelpersTest, GenerationsNoLongerKeptAreMarkedAsAGap)
{
    // More generations arrived unread than the model keeps: the first one available is not the one
    // after the last taken in, so the point after them is flagged for a gap.
    std::uint64_t key = 7;
    SampleIntake intake;
    static_cast<void>(take({makeSample(5, 5.0, 42, 7)}, 42, key, intake));
    const auto recorded = take({makeSample(9, 9.0, 42, 7), makeSample(10, 10.0, 42, 7)}, 42, key, intake);
    ASSERT_EQ(recorded.size(), 2U);
    EXPECT_TRUE(recorded[0].gapBefore);
    EXPECT_FALSE(recorded[1].gapBefore);
}

TEST(ProcessHistoryHelpersTest, TheFirstSampleAfterASelectionIsNotAGap)
{
    std::uint64_t key = 0;
    SampleIntake intake;
    const auto recorded = take({makeSample(40, 4.0, 42, 7)}, 42, key, intake);
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_FALSE(recorded[0].gapBefore);
}

TEST(ProcessHistoryHelpersTest, AProcessSelectedByPidAdoptsItsKeyAndIgnoresAReuseOfThePid)
{
    std::uint64_t key = 0;
    SampleIntake intake;
    static_cast<void>(take({makeSample(1, 1.0, 42, 7)}, 42, key, intake));
    EXPECT_EQ(key, 7U);

    // The PID now belongs to a different process: not recorded, and the selected one is gone.
    const auto recorded = take({makeSample(2, 2.0, 42, 8)}, 42, key, intake);
    EXPECT_TRUE(recorded.empty());
    EXPECT_FALSE(intake.present);
    EXPECT_EQ(intake.lastVersion, 2U);
}

TEST(ProcessHistoryHelpersTest, AGenerationWithoutTheProcessClearsPresent)
{
    std::uint64_t key = 7;
    SampleIntake intake;
    static_cast<void>(take({makeSample(1, 1.0, 42, 7)}, 42, key, intake));
    ASSERT_TRUE(intake.present);
    const auto recorded = take({Domain::ProcessSample{.snapshot = nullptr,
                                                      .version = 2,
                                                      .sampleTimeSeconds = 2.0,
                                                      .ioCountersSupported = true,
                                                      .networkCountersSupported = true,
                                                      .gpuPerProcessSupported = true,
                                                      .gpuUtilizationSupported = true,
                                                      .gpuReadFailed = false}},
                               42,
                               key,
                               intake);
    EXPECT_TRUE(recorded.empty());
    EXPECT_FALSE(intake.present);
}

TEST(ProcessHistoryHelpersTest, AnUnreadValueIsAGapNotAZero)
{
    // #1110: a value the probe could not read is NaN in the history, drawn as a gap.
    EXPECT_DOUBLE_EQ(readingOrGap(true, 12.0), 12.0);
    EXPECT_DOUBLE_EQ(readingOrGap(true, 0.0), 0.0);
    EXPECT_TRUE(std::isnan(readingOrGap(false, 0.0)));
}

// #1210: a probe without network (or I/O) counters still marks each snapshot available, with a rate
// of 0. That is no reading, so it is recorded as a gap, not a measured zero.
TEST(ProcessHistoryHelpersTest, ARateIsAReadingOnlyWhereTheProbeSupportsIt)
{
    EXPECT_TRUE(rateIsReading(/*probeSupports=*/true, /*readThisSample=*/true));
    EXPECT_FALSE(rateIsReading(false, true));
    EXPECT_FALSE(rateIsReading(true, false));
    EXPECT_TRUE(std::isnan(readingOrGap(rateIsReading(false, true), 0.0)));
}

TEST(ProcessHistoryHelpersTest, EachSampleIsJudgedByItsOwnGenerationsSupport)
{
    // #1210: a batch spanning the generation where the probe withdrew its network counters. The
    // earlier sample's network reading stays a reading; the later one is a gap.
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>();
    snapshot->ioAvailable = true;
    snapshot->networkAvailable = true;

    const Domain::ProcessSample before{.snapshot = snapshot,
                                       .version = 1,
                                       .sampleTimeSeconds = 1.0,
                                       .ioCountersSupported = true,
                                       .networkCountersSupported = true,
                                       .gpuPerProcessSupported = true,
                                       .gpuUtilizationSupported = true,
                                       .gpuReadFailed = false};
    const Domain::ProcessSample after{.snapshot = snapshot,
                                      .version = 2,
                                      .sampleTimeSeconds = 2.0,
                                      .ioCountersSupported = true,
                                      .networkCountersSupported = false,
                                      .gpuPerProcessSupported = true,
                                      .gpuUtilizationSupported = true,
                                      .gpuReadFailed = false};

    EXPECT_TRUE(rateReadings(before).network);
    EXPECT_TRUE(rateReadings(before).io);
    EXPECT_FALSE(rateReadings(after).network);
    EXPECT_TRUE(rateReadings(after).io);

    // An unreadable rate is no reading whatever the support; no snapshot, no readings.
    auto unread = std::make_shared<Domain::ProcessSnapshot>();
    unread->ioAvailable = false;
    unread->networkAvailable = false;
    const Domain::ProcessSample unreadSample{.snapshot = unread,
                                             .version = 3,
                                             .sampleTimeSeconds = 3.0,
                                             .ioCountersSupported = true,
                                             .networkCountersSupported = true,
                                             .gpuPerProcessSupported = true,
                                             .gpuUtilizationSupported = true,
                                             .gpuReadFailed = false};
    EXPECT_FALSE(rateReadings(unreadSample).io);
    EXPECT_FALSE(rateReadings(unreadSample).network);
    const Domain::ProcessSample absent{.snapshot = nullptr,
                                       .version = 4,
                                       .sampleTimeSeconds = 4.0,
                                       .ioCountersSupported = true,
                                       .networkCountersSupported = true,
                                       .gpuPerProcessSupported = true,
                                       .gpuUtilizationSupported = true,
                                       .gpuReadFailed = false};
    EXPECT_FALSE(rateReadings(absent).io);
}

TEST(ProcessHistoryHelpersTest, EachSampleCarriesItsOwnGenerationsGpuSupport)
{
    // #1210: a batch spanning the generation in which the GPU model lost per-process utilization (on
    // re-enumeration, on its own sampler). The earlier sample's utilization stays a reading; after a
    // gain, an earlier unsupported 0 does not become a measurement.
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>();
    const Domain::ProcessSample before{.snapshot = snapshot,
                                       .version = 1,
                                       .sampleTimeSeconds = 1.0,
                                       .ioCountersSupported = true,
                                       .networkCountersSupported = true,
                                       .gpuPerProcessSupported = true,
                                       .gpuUtilizationSupported = true,
                                       .gpuReadFailed = false};
    const Domain::ProcessSample after{.snapshot = snapshot,
                                      .version = 2,
                                      .sampleTimeSeconds = 2.0,
                                      .ioCountersSupported = true,
                                      .networkCountersSupported = true,
                                      .gpuPerProcessSupported = true,
                                      .gpuUtilizationSupported = false,
                                      .gpuReadFailed = false};
    const Domain::ProcessSample none{.snapshot = snapshot,
                                     .version = 3,
                                     .sampleTimeSeconds = 3.0,
                                     .ioCountersSupported = true,
                                     .networkCountersSupported = true,
                                     .gpuPerProcessSupported = false,
                                     .gpuUtilizationSupported = true,
                                     .gpuReadFailed = false};

    EXPECT_TRUE(rateReadings(before).gpuUtilization);
    EXPECT_TRUE(rateReadings(before).gpuPerProcess);
    EXPECT_FALSE(rateReadings(after).gpuUtilization);
    EXPECT_TRUE(rateReadings(after).gpuPerProcess);
    // No per-process data means no utilization either, whatever the other flag says.
    EXPECT_FALSE(rateReadings(none).gpuPerProcess);
    EXPECT_FALSE(rateReadings(none).gpuUtilization);
}

TEST(ProcessHistoryHelpersTest, AFailedGpuReadIsAGapButKeepsSupport)
{
    // #1210: the sample's GPU figures are not readings (a gap, N/A), but the GPU tab must not say the
    // system cannot report per-process GPU usage.
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>();
    const Domain::ProcessSample failed{.snapshot = snapshot,
                                       .version = 1,
                                       .sampleTimeSeconds = 1.0,
                                       .ioCountersSupported = true,
                                       .networkCountersSupported = true,
                                       .gpuPerProcessSupported = true,
                                       .gpuUtilizationSupported = true,
                                       .gpuReadFailed = true};
    const SampleRateReadings readings = rateReadings(failed);
    EXPECT_FALSE(readings.gpuPerProcess);
    EXPECT_FALSE(readings.gpuUtilization);
    EXPECT_TRUE(readings.gpuSupported);
    EXPECT_NE(gpuTabContent(readings.gpuSupported, false), GpuTabContent::Unavailable);

    // A history of only that sample's gap holds no reading: the tab says so rather than "no usage".
    const double gap = std::numeric_limits<double>::quiet_NaN();
    const std::vector<double> gapsOnly{readingOrGap(readings.gpuUtilization, 0.0), readingOrGap(readings.gpuPerProcess, 0.0), gap};
    EXPECT_FALSE(hasAnyReading(gapsOnly));
    EXPECT_EQ(gpuTabContent(readings.gpuSupported, false, hasAnyReading(gapsOnly)), GpuTabContent::NoReadings);
}

TEST(ProcessHistoryHelpersTest, GpuFieldsNotReadYetAreNotReadings)
{
    // #1210: a process that started between throttled GPU merges: no GPU reading, support kept.
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>();
    snapshot->gpuFieldsRead = false;
    const Domain::ProcessSample sample{.snapshot = snapshot,
                                       .version = 1,
                                       .sampleTimeSeconds = 1.0,
                                       .ioCountersSupported = true,
                                       .networkCountersSupported = true,
                                       .gpuPerProcessSupported = true,
                                       .gpuUtilizationSupported = true,
                                       .gpuReadFailed = false};
    const SampleRateReadings readings = rateReadings(sample);
    EXPECT_FALSE(readings.gpuPerProcess);
    EXPECT_FALSE(readings.gpuUtilization);
    EXPECT_TRUE(readings.gpuSupported);
}

TEST(ProcessHistoryHelpersTest, HistoriesOfOnlyGapsAreNoData)
{
    const double gap = std::numeric_limits<double>::quiet_NaN();
    const std::vector<double> gaps{gap, gap, gap};
    const std::vector<double> none;
    const std::vector<double> zeros{gap, 0.0, 0.0}; // A measured idle is data

    EXPECT_FALSE(hasAnyReading(gaps));
    EXPECT_FALSE(hasAnyReading(none));
    EXPECT_TRUE(hasAnyReading(zeros));

    // Every sample adds a point to every history, so non-empty is not enough for the tab's charts.
    EXPECT_FALSE(hasNetworkOrIoReadings(gaps, gaps, gaps, gaps));
    EXPECT_TRUE(hasNetworkOrIoReadings(gaps, gaps, zeros, gaps)); // Network supported, I/O not
    EXPECT_TRUE(hasNetworkOrIoReadings(zeros, gaps, gaps, gaps)); // I/O supported, network not
}

TEST(ProcessHistoryHelpersTest, ASampleAcceptedEarlierInTheBatchCountsAsSeen)
{
    // #1290 review: right after selection (no snapshot yet), a batch that records the process and then
    // ends with it absent (exited, or its PID reused) means it exited -- not "not seen yet".
    EXPECT_TRUE(exitedAfterBatch(false, false, true));
    EXPECT_TRUE(exitedAfterBatch(false, true, false));
    EXPECT_FALSE(exitedAfterBatch(false, false, false)); // never seen: just selected
    EXPECT_FALSE(exitedAfterBatch(true, true, true));    // still present
}

} // namespace
} // namespace App::Detail
