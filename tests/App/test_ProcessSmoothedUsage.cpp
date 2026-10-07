/// @file test_ProcessSmoothedUsage.cpp
/// @brief Tests for Process Details' smoothed NowBar values (#1179): easing toward a step, the
/// first sample and no-time-passed snaps, reset, unavailable readings, NaN inputs and clamping.

#include "App/Panels/ProcessDetailsPanel_HistoryHelpers.h"
#include "App/Panels/ProcessSmoothedUsage.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "UI/ChartSmoothing.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace App::Detail
{
namespace
{

constexpr std::chrono::milliseconds REFRESH{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
constexpr float FRAME_SECONDS = 0.05F; // One 20 FPS frame: well under the smoothing time constant
constexpr float LONG_GAP_SECONDS = 60.0F;
constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

/// Every reading available, as from a probe that supports them all.
constexpr SampleRateReadings ALL_READINGS{.io = true, .network = true, .gpuPerProcess = true, .gpuUtilization = true, .gpuSupported = true};

[[nodiscard]] Domain::ProcessSnapshot sampleAt(double cpuPercent, std::uint64_t memoryBytes)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.cpuPercent = cpuPercent;
    snapshot.cpuUserPercent = cpuPercent;
    snapshot.cpuSystemPercent = 0.0;
    snapshot.memoryBytes = memoryBytes;
    snapshot.virtualBytes = memoryBytes * 2;
    snapshot.memoryPercent = 10.0;
    snapshot.threadCount = 8;
    snapshot.handleCount = 100;
    snapshot.ioReadBytesPerSec = 1000.0;
    snapshot.ioWriteBytesPerSec = 2000.0;
    snapshot.netSentBytesPerSec = 300.0;
    snapshot.netReceivedBytesPerSec = 400.0;
    snapshot.pageFaultsPerSec = 5.0;
    snapshot.powerWatts = 1.5;
    snapshot.gpuUtilPercent = 20.0;
    snapshot.gpuMemoryBytes = 4096;
    snapshot.sharedBytes = memoryBytes / 4;
    snapshot.gdiObjectCount = 30;
    return snapshot;
}

TEST(ProcessSmoothedUsageTest, TheFirstSampleIsTakenOutright)
{
    ProcessSmoothedUsage usage;
    EXPECT_FALSE(usage.initialized);

    usage.update(sampleAt(40.0, 1000), ALL_READINGS, FRAME_SECONDS, REFRESH);

    EXPECT_TRUE(usage.initialized);
    EXPECT_DOUBLE_EQ(usage.cpuPercent, 40.0);
    EXPECT_DOUBLE_EQ(usage.cpuUserPercent, 40.0);
    EXPECT_DOUBLE_EQ(usage.residentBytes, 1000.0);
    EXPECT_DOUBLE_EQ(usage.virtualBytes, 2000.0);
    EXPECT_DOUBLE_EQ(usage.threadCount, 8.0);
    EXPECT_DOUBLE_EQ(usage.handleCount, 100.0);
    EXPECT_DOUBLE_EQ(usage.ioReadBytesPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(usage.netRecvBytesPerSec, 400.0);
    EXPECT_DOUBLE_EQ(usage.gpuUtilPercent, 20.0);
    EXPECT_DOUBLE_EQ(usage.gpuMemoryBytes, 4096.0);
    EXPECT_DOUBLE_EQ(usage.gdiObjectCount, 30.0);
    EXPECT_DOUBLE_EQ(usage.memorySharedBytes, 250.0);
    EXPECT_TRUE(usage.ioAvailable);
    EXPECT_TRUE(usage.networkAvailable);
    EXPECT_TRUE(usage.gdiInitialized);
    // Not smoothed: 10 % of RAM over 1000 bytes.
    EXPECT_DOUBLE_EQ(usage.memoryPercentPerByte, 0.01);
}

TEST(ProcessSmoothedUsageTest, ALaterSampleEasesByTheChartAlpha)
{
    ProcessSmoothedUsage usage;
    usage.update(sampleAt(0.0, 1000), ALL_READINGS, FRAME_SECONDS, REFRESH);
    usage.update(sampleAt(100.0, 3000), ALL_READINGS, FRAME_SECONDS, REFRESH);

    const double alpha = UI::Widgets::computeAlpha(FRAME_SECONDS, REFRESH);
    ASSERT_GT(alpha, 0.0);
    ASSERT_LT(alpha, 1.0);
    EXPECT_DOUBLE_EQ(usage.cpuPercent, 100.0 * alpha);
    EXPECT_DOUBLE_EQ(usage.residentBytes, 1000.0 + (2000.0 * alpha));
}

TEST(ProcessSmoothedUsageTest, AStepIsApproachedMonotonicallyAndConvergesOverTime)
{
    ProcessSmoothedUsage usage;
    usage.update(sampleAt(0.0, 1000), ALL_READINGS, FRAME_SECONDS, REFRESH);

    double previous = usage.cpuPercent;
    for (int frame = 0; frame < 200; ++frame)
    {
        SCOPED_TRACE("frame " + std::to_string(frame));
        usage.update(sampleAt(80.0, 1000), ALL_READINGS, FRAME_SECONDS, REFRESH);
        EXPECT_GE(usage.cpuPercent, previous);
        EXPECT_LE(usage.cpuPercent, 80.0);
        previous = usage.cpuPercent;
    }
    // 200 frames of 50 ms is 10 s, many time constants: the value has all but arrived.
    EXPECT_NEAR(usage.cpuPercent, 80.0, 1e-6);
}

TEST(ProcessSmoothedUsageTest, NoTimePassedSnapsToTheSample)
{
    for (const float deltaSeconds : {0.0F, -1.0F})
    {
        SCOPED_TRACE(deltaSeconds);
        ProcessSmoothedUsage usage;
        usage.update(sampleAt(0.0, 1000), ALL_READINGS, FRAME_SECONDS, REFRESH);
        usage.update(sampleAt(70.0, 5000), ALL_READINGS, deltaSeconds, REFRESH);

        EXPECT_DOUBLE_EQ(usage.cpuPercent, 70.0);
        EXPECT_DOUBLE_EQ(usage.residentBytes, 5000.0);
        EXPECT_DOUBLE_EQ(usage.ioWriteBytesPerSec, 2000.0);
    }
}

TEST(ProcessSmoothedUsageTest, ALongGapAllButReachesTheSample)
{
    ProcessSmoothedUsage usage;
    usage.update(sampleAt(0.0, 1000), ALL_READINGS, FRAME_SECONDS, REFRESH);
    usage.update(sampleAt(90.0, 1000), ALL_READINGS, LONG_GAP_SECONDS, REFRESH);

    // alpha is clamped to at most 1, so a long gap never overshoots the sample.
    EXPECT_LE(usage.cpuPercent, 90.0);
    EXPECT_NEAR(usage.cpuPercent, 90.0, 1e-9);
}

TEST(ProcessSmoothedUsageTest, ResetMakesTheNextSampleSnap)
{
    ProcessSmoothedUsage usage;
    usage.update(sampleAt(10.0, 1000), ALL_READINGS, FRAME_SECONDS, REFRESH);
    usage.reset();

    EXPECT_FALSE(usage.initialized);
    EXPECT_DOUBLE_EQ(usage.cpuPercent, 0.0);
    EXPECT_FALSE(usage.ioAvailable);
    EXPECT_FALSE(usage.gdiInitialized);

    // The new selection's first sample is not eased from the previous process's values.
    usage.update(sampleAt(60.0, 8000), ALL_READINGS, FRAME_SECONDS, REFRESH);
    EXPECT_DOUBLE_EQ(usage.cpuPercent, 60.0);
    EXPECT_DOUBLE_EQ(usage.residentBytes, 8000.0);
}

TEST(ProcessSmoothedUsageTest, AnUnavailableReadingHoldsItsValueAndTheNextStartsAfresh)
{
    ProcessSmoothedUsage usage;
    usage.update(sampleAt(0.0, 1000), ALL_READINGS, FRAME_SECONDS, REFRESH);

    auto unread = sampleAt(0.0, 1000);
    unread.handleCountAvailable = false;
    unread.handleCount = 0;
    unread.gdiObjectCount.reset();
    unread.ioReadBytesPerSec = 0.0;
    unread.netSentBytesPerSec = 0.0;
    usage.update(unread, SampleRateReadings{}, FRAME_SECONDS, REFRESH);

    // Not eased toward the placeholder zeros: the values stay, marked unavailable.
    EXPECT_FALSE(usage.handleCountAvailable);
    EXPECT_FALSE(usage.ioAvailable);
    EXPECT_FALSE(usage.networkAvailable);
    EXPECT_FALSE(usage.gpuUtilAvailable);
    EXPECT_FALSE(usage.gpuMemoryAvailable);
    EXPECT_FALSE(usage.gdiInitialized);
    EXPECT_DOUBLE_EQ(usage.handleCount, 100.0);
    EXPECT_DOUBLE_EQ(usage.ioReadBytesPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(usage.netSentBytesPerSec, 300.0);
    EXPECT_DOUBLE_EQ(usage.gpuUtilPercent, 20.0);
    EXPECT_DOUBLE_EQ(usage.gpuMemoryBytes, 4096.0);
    EXPECT_DOUBLE_EQ(usage.gdiObjectCount, 30.0);

    // The next reading is taken outright rather than eased from the held value.
    auto back = sampleAt(0.0, 1000);
    back.handleCount = 500;
    back.ioReadBytesPerSec = 9000.0;
    back.gdiObjectCount = 70;
    usage.update(back, ALL_READINGS, FRAME_SECONDS, REFRESH);
    EXPECT_TRUE(usage.handleCountAvailable);
    EXPECT_TRUE(usage.ioAvailable);
    EXPECT_DOUBLE_EQ(usage.handleCount, 500.0);
    EXPECT_DOUBLE_EQ(usage.ioReadBytesPerSec, 9000.0);
    EXPECT_DOUBLE_EQ(usage.gdiObjectCount, 70.0);
}

[[nodiscard]] Domain::ProcessSnapshot sampleWithNaNs()
{
    auto sample = sampleAt(NOT_A_NUMBER, 1000);
    sample.cpuSystemPercent = NOT_A_NUMBER;
    sample.pageFaultsPerSec = NOT_A_NUMBER;
    sample.powerWatts = NOT_A_NUMBER;
    sample.gpuUtilPercent = NOT_A_NUMBER;
    sample.ioReadBytesPerSec = NOT_A_NUMBER;
    sample.netSentBytesPerSec = NOT_A_NUMBER;
    return sample;
}

TEST(ProcessSmoothedUsageTest, NaNInAFirstSampleReadsAsZero)
{
    ProcessSmoothedUsage usage;
    usage.update(sampleWithNaNs(), ALL_READINGS, FRAME_SECONDS, REFRESH);

    EXPECT_DOUBLE_EQ(usage.cpuPercent, 0.0);
    EXPECT_DOUBLE_EQ(usage.cpuUserPercent, 0.0);
    EXPECT_DOUBLE_EQ(usage.cpuSystemPercent, 0.0);
    EXPECT_DOUBLE_EQ(usage.pageFaultsPerSec, 0.0);
    EXPECT_DOUBLE_EQ(usage.powerWatts, 0.0);
    EXPECT_DOUBLE_EQ(usage.gpuUtilPercent, 0.0);
    EXPECT_DOUBLE_EQ(usage.ioReadBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(usage.netSentBytesPerSec, 0.0);
}

/// One value before and after an update, named for the failure trace.
struct EasedValue
{
    const char* name;
    double was;
    double now;
};

TEST(ProcessSmoothedUsageTest, NaNInALaterSampleEasesTowardZero)
{
    ProcessSmoothedUsage usage;
    usage.update(sampleAt(50.0, 1000), ALL_READINGS, FRAME_SECONDS, REFRESH);
    const ProcessSmoothedUsage before = usage;
    usage.update(sampleWithNaNs(), ALL_READINGS, FRAME_SECONDS, REFRESH);

    // A NaN target is read as 0, so each value eases down toward it and never becomes NaN.
    const std::array values{
        EasedValue{.name = "cpu", .was = before.cpuPercent, .now = usage.cpuPercent},
        EasedValue{.name = "cpu user", .was = before.cpuUserPercent, .now = usage.cpuUserPercent},
        EasedValue{.name = "page faults", .was = before.pageFaultsPerSec, .now = usage.pageFaultsPerSec},
        EasedValue{.name = "power", .was = before.powerWatts, .now = usage.powerWatts},
        EasedValue{.name = "gpu util", .was = before.gpuUtilPercent, .now = usage.gpuUtilPercent},
        EasedValue{.name = "io read", .was = before.ioReadBytesPerSec, .now = usage.ioReadBytesPerSec},
        EasedValue{.name = "net sent", .was = before.netSentBytesPerSec, .now = usage.netSentBytesPerSec},
    };
    for (const auto& value : values)
    {
        SCOPED_TRACE(value.name);
        EXPECT_FALSE(std::isnan(value.now));
        EXPECT_GT(value.now, 0.0);
        EXPECT_LT(value.now, value.was);
    }
    EXPECT_DOUBLE_EQ(usage.cpuSystemPercent, 0.0); // Was 0 already
}

TEST(ProcessSmoothedUsageTest, ValuesStayWithinTheirRanges)
{
    auto sample = sampleAt(250.0, 4000);
    sample.cpuUserPercent = 180.0;
    sample.cpuSystemPercent = -5.0;
    sample.gpuUtilPercent = 140.0;
    sample.pageFaultsPerSec = -3.0;
    sample.powerWatts = -2.0;
    sample.ioWriteBytesPerSec = -100.0;
    sample.virtualBytes = 1000; // Below resident: virtual is raised to resident

    for (const bool afterAnotherSample : {false, true})
    {
        SCOPED_TRACE(afterAnotherSample ? "eased" : "first");
        ProcessSmoothedUsage usage;
        if (afterAnotherSample)
        {
            usage.update(sampleAt(50.0, 4000), ALL_READINGS, FRAME_SECONDS, REFRESH);
        }
        usage.update(sample, ALL_READINGS, LONG_GAP_SECONDS, REFRESH);

        EXPECT_LE(usage.cpuPercent, 100.0);
        EXPECT_NEAR(usage.cpuPercent, 100.0, 1e-9);
        EXPECT_LE(usage.cpuUserPercent, 100.0);
        EXPECT_GE(usage.cpuSystemPercent, 0.0);
        EXPECT_LE(usage.gpuUtilPercent, 100.0);
        EXPECT_GE(usage.pageFaultsPerSec, 0.0);
        EXPECT_GE(usage.powerWatts, 0.0);
        EXPECT_GE(usage.ioWriteBytesPerSec, 0.0);
        EXPECT_GE(usage.virtualBytes, usage.residentBytes);
    }
}

TEST(ProcessSmoothedUsageTest, MemoryPercentPerByteIsZeroWithoutAShareOrASize)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.memoryPercent = 25.0;
    snapshot.memoryBytes = 0;
    EXPECT_DOUBLE_EQ(memoryPercentPerByte(snapshot), 0.0);

    snapshot.memoryBytes = 500;
    snapshot.memoryPercent = 0.0;
    EXPECT_DOUBLE_EQ(memoryPercentPerByte(snapshot), 0.0);

    // A share above 100 % is clamped.
    snapshot.memoryPercent = 150.0;
    EXPECT_DOUBLE_EQ(memoryPercentPerByte(snapshot), 100.0 / 500.0);
}

} // namespace
} // namespace App::Detail
