/// @file test_ProcessEnergyAttribution.cpp
/// @brief Tests for Domain::ProcessEnergy, per-interval RAPL energy attribution (#1093)

#include "Domain/ProcessEnergyAttribution.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace Domain
{
namespace
{

using ProcessEnergy::Attributor;
using ProcessEnergy::energyDeltaUj;

Platform::ProcessCounters process(std::int32_t pid, std::uint64_t cpuTime, std::uint64_t startTimeTicks = 1)
{
    Platform::ProcessCounters counters{};
    counters.pid = pid;
    counters.startTimeTicks = startTimeTicks;
    counters.userTime = cpuTime;
    return counters;
}

TEST(ProcessEnergyTest, EnergyDeltaHandlesTheCounterWrap)
{
    EXPECT_EQ(energyDeltaUj(1'000, 1'500, 10'000), 500U);
    // Wrapped past max_energy_range_uj: 9'900 -> 10'000 -> 0 -> 400.
    EXPECT_EQ(energyDeltaUj(9'900, 400, 10'000), 500U);
    // A decrease with no known range isn't trusted.
    EXPECT_EQ(energyDeltaUj(9'900, 400, 0), 0U);
}

TEST(ProcessEnergyTest, FirstSampleCreditsNothing)
{
    Attributor attributor;
    std::vector<Platform::ProcessCounters> processes{process(1, 500), process(2, 900)};
    attributor.attribute(processes, 200'000'000'000ULL, 0);

    EXPECT_EQ(processes[0].energyMicrojoules, 0U);
    EXPECT_EQ(processes[1].energyMicrojoules, 0U);
}

TEST(ProcessEnergyTest, IdleLongLivedProcessIsChargedNothing)
{
    // A daemon with a huge lifetime CPU time but none this interval used to be charged its lifetime
    // share of current package power.
    Attributor attributor;
    std::vector<Platform::ProcessCounters> first{process(1, 1'000'000), process(2, 100)};
    attributor.attribute(first, 50'000, 0);

    std::vector<Platform::ProcessCounters> second{process(1, 1'000'000), process(2, 200)};
    attributor.attribute(second, 51'000, 0);

    EXPECT_EQ(second[0].energyMicrojoules, 0U);
    EXPECT_EQ(second[1].energyMicrojoules, 1'000U);
}

TEST(ProcessEnergyTest, IntervalEnergyIsSharedByIntervalCpuAndSumsToTheDelta)
{
    Attributor attributor;
    std::vector<Platform::ProcessCounters> first{process(1, 100), process(2, 100), process(3, 100)};
    attributor.attribute(first, 0, 0);

    std::vector<Platform::ProcessCounters> second{process(1, 130), process(2, 160), process(3, 100)};
    attributor.attribute(second, 9'000, 0);

    EXPECT_EQ(second[0].energyMicrojoules, 3'000U);
    EXPECT_EQ(second[1].energyMicrojoules, 6'000U);
    EXPECT_EQ(second[2].energyMicrojoules, 0U);
}

TEST(ProcessEnergyTest, ExitingProcessDoesNotSpikeTheSurvivors)
{
    Attributor attributor;
    std::vector<Platform::ProcessCounters> first{process(1, 100), process(2, 100), process(3, 5'000'000)};
    attributor.attribute(first, 0, 0);

    // The busy process 3 exits; 1 and 2 used 50 ticks each.
    std::vector<Platform::ProcessCounters> second{process(1, 150), process(2, 150)};
    attributor.attribute(second, 1'000, 0);

    EXPECT_EQ(second[0].energyMicrojoules, 500U);
    EXPECT_EQ(second[1].energyMicrojoules, 500U);
}

TEST(ProcessEnergyTest, TotalsAccumulateAcrossIntervalsAndSurviveAWrap)
{
    Attributor attributor;
    std::vector<Platform::ProcessCounters> sample{process(1, 0)};
    attributor.attribute(sample, 9'000, 10'000);

    sample = {process(1, 10)};
    attributor.attribute(sample, 9'600, 10'000); // +600
    EXPECT_EQ(sample[0].energyMicrojoules, 600U);

    sample = {process(1, 20)};
    attributor.attribute(sample, 100, 10'000); // wrapped: +500
    EXPECT_EQ(sample[0].energyMicrojoules, 1'100U);
}

TEST(ProcessEnergyTest, FailedReadKeepsTotalsAndRestartsTheBaseline)
{
    // Different processes are busy before and after the failure, so crediting the energy that
    // spans the failed read on the next interval's shares would show up as a spike on process 2.
    Attributor attributor;
    std::vector<Platform::ProcessCounters> sample{process(1, 0), process(2, 0)};
    attributor.attribute(sample, 1'000, 0);

    sample = {process(1, 10), process(2, 0)}; // process 1 busy
    attributor.attribute(sample, 2'000, 0);
    ASSERT_EQ(sample[0].energyMicrojoules, 1'000U);
    ASSERT_EQ(sample[1].energyMicrojoules, 0U);

    sample = {process(1, 10), process(2, 10)}; // read fails while process 2 is busy
    attributor.attribute(sample, std::nullopt, 0);
    EXPECT_EQ(sample[0].energyMicrojoules, 1'000U); // unchanged: reads as 0 W
    EXPECT_EQ(sample[1].energyMicrojoules, 0U);

    sample = {process(1, 10), process(2, 20)}; // first good read: only a new baseline
    attributor.attribute(sample, 5'000, 0);
    EXPECT_EQ(sample[0].energyMicrojoules, 1'000U);
    EXPECT_EQ(sample[1].energyMicrojoules, 0U); // no catch-up spike from the 3,000 uJ gap

    sample = {process(1, 10), process(2, 30)};
    attributor.attribute(sample, 6'000, 0); // normal interval again
    EXPECT_EQ(sample[0].energyMicrojoules, 1'000U);
    EXPECT_EQ(sample[1].energyMicrojoules, 1'000U);
}

TEST(ProcessEnergyTest, NewlyStartedProcessesShareIsNotGivenToOthers)
{
    // An established process uses 10 ticks while a new one uses 90: the established one gets 10% of
    // the interval's energy, not all of it (#1217 review).
    Attributor attributor;
    std::vector<Platform::ProcessCounters> first{process(1, 100)};
    attributor.attribute(first, 0, 0);

    std::vector<Platform::ProcessCounters> second{process(1, 110), process(2, 90)};
    attributor.attribute(second, 1'000, 0);

    EXPECT_EQ(second[0].energyMicrojoules, 100U);
    EXPECT_EQ(second[1].energyMicrojoules, 0U); // first seen: not credited
}

TEST(ProcessEnergyTest, WorkByProcessesThatExitedIsNotChargedToSurvivors)
{
    // A process uses 900 ticks and exits between samples while two survivors use 10 each. The
    // system busy counter (920 ticks) is the denominator, so each survivor gets 10/920 of the
    // energy rather than half (#1217 review).
    Attributor attributor;
    std::vector<Platform::ProcessCounters> first{process(1, 100), process(2, 100), process(3, 0)};
    attributor.attribute(first, 0, 0, 5'000);

    std::vector<Platform::ProcessCounters> second{process(1, 110), process(2, 110)};
    attributor.attribute(second, 9'200, 0, 5'920);

    EXPECT_EQ(second[0].energyMicrojoules, 100U);
    EXPECT_EQ(second[1].energyMicrojoules, 100U);
}

TEST(ProcessEnergyTest, ReusedPidIsANewProcess)
{
    Attributor attributor;
    std::vector<Platform::ProcessCounters> first{process(7, 100, 1), process(8, 100)};
    attributor.attribute(first, 0, 0);

    // PID 7 now belongs to a different process (new start time): it's a new process, so it gets no
    // credit on its first sample, but its 900 ticks still count, leaving process 8 its 100 of 1,000.
    std::vector<Platform::ProcessCounters> second{process(7, 900, 2), process(8, 200)};
    attributor.attribute(second, 4'000, 0);

    EXPECT_EQ(second[0].energyMicrojoules, 0U);
    EXPECT_EQ(second[1].energyMicrojoules, 400U);
}

} // namespace
} // namespace Domain
