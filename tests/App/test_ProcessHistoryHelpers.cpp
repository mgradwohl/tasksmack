/// @file test_ProcessHistoryHelpers.cpp
/// @brief Tests for how Process Details takes the selected process's samples into its history:
/// one point per published generation at the generation's own sample time (#1098), and NaN for a
/// value the probe could not read (#1110).

#include "App/Panels/ProcessDetailsPanel_HistoryHelpers.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
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
    return {.snapshot = std::make_shared<const Domain::ProcessSnapshot>(snapshot), .version = version, .sampleTimeSeconds = timeSeconds};
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
    const auto recorded = take({Domain::ProcessSample{.snapshot = nullptr, .version = 2, .sampleTimeSeconds = 2.0}}, 42, key, intake);
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

} // namespace
} // namespace App::Detail
