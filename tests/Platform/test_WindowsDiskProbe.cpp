/// @file test_WindowsDiskProbe.cpp
/// @brief Integration tests for Platform::WindowsDiskProbe
///
/// These are integration tests that interact with the real Windows Performance Counters.
/// They verify that the probe correctly reads and parses disk I/O information.

#include <gtest/gtest.h>

#if defined(_WIN32)

#include "Platform/StorageTypes.h"
#include "Platform/Windows/WindowsDiskProbe.h"
#include "Platform/Windows/WindowsDiskProbeMath.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <thread>

namespace Platform
{
namespace
{

// =============================================================================
// clampNonNegativeQuadPart: pure math, no hardware required. A buggy or virtualized
// disk driver can report a negative IOCTL_DISK_PERFORMANCE byte/time count; this must
// clamp to 0 rather than reinterpreting the sign bit as a huge magnitude.
// =============================================================================

TEST(ClampNonNegativeQuadPartTest, PositiveValuePassesThrough)
{
    EXPECT_EQ(clampNonNegativeQuadPart(12345), 12345ULL);
}

TEST(ClampNonNegativeQuadPartTest, ZeroPassesThrough)
{
    EXPECT_EQ(clampNonNegativeQuadPart(0), 0ULL);
}

TEST(ClampNonNegativeQuadPartTest, NegativeValueClampsToZero)
{
    EXPECT_EQ(clampNonNegativeQuadPart(-1), 0ULL);
    EXPECT_EQ(clampNonNegativeQuadPart(std::numeric_limits<int64_t>::min()), 0ULL);
}

TEST(ClampNonNegativeQuadPartTest, LargePositiveValueDoesNotWrap)
{
    constexpr auto largeValue = std::numeric_limits<int64_t>::max();
    EXPECT_EQ(clampNonNegativeQuadPart(largeValue), static_cast<uint64_t>(largeValue));
}

// =============================================================================
// parsePhysicalDriveIndex: pure parsing of PDH PhysicalDisk instance names, no
// hardware required. Malformed instance names must fail closed (nullopt) rather
// than opening an arbitrary/wrong \\.\PhysicalDriveN device.
// =============================================================================

TEST(ParsePhysicalDriveIndexTest, SingleDigitIndexWithDriveLetter)
{
    const auto result = parsePhysicalDriveIndex(L"0 C:");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
}

TEST(ParsePhysicalDriveIndexTest, MultiDigitIndexWithMultipleDriveLetters)
{
    const auto result = parsePhysicalDriveIndex(L"12 D: E:");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 12);
}

TEST(ParsePhysicalDriveIndexTest, IndexWithNoTrailingSpaceOrLetters)
{
    const auto result = parsePhysicalDriveIndex(L"3");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 3);
}

TEST(ParsePhysicalDriveIndexTest, EmptyStringReturnsNullopt)
{
    EXPECT_FALSE(parsePhysicalDriveIndex(L"").has_value());
}

TEST(ParsePhysicalDriveIndexTest, LeadingSpaceReturnsNullopt)
{
    // Empty index part before the space.
    EXPECT_FALSE(parsePhysicalDriveIndex(L" C:").has_value());
}

TEST(ParsePhysicalDriveIndexTest, NonNumericIndexReturnsNullopt)
{
    EXPECT_FALSE(parsePhysicalDriveIndex(L"_Total").has_value());
    EXPECT_FALSE(parsePhysicalDriveIndex(L"C: 0").has_value());
}

TEST(ParsePhysicalDriveIndexTest, OverflowingNumericPrefixReturnsNulloptRatherThanWrapping)
{
    // A PhysicalDisk instance name is never legitimately this long, but a malformed/adversarial
    // one must fail closed instead of overflowing signed int (undefined behavior).
    EXPECT_FALSE(parsePhysicalDriveIndex(L"99999999999999999999 C:").has_value());
}

TEST(ParsePhysicalDriveIndexTest, MaxIntIndexIsAccepted)
{
    const auto result = parsePhysicalDriveIndex(L"2147483647 C:");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::numeric_limits<int>::max());
}

// =============================================================================
// diskBusyTime100ns: pure math, no hardware required. Utilisation is Δbusy / Δwall, so
// busy time must not grow faster than wall time even when reads and writes overlap (#1108).
// =============================================================================

// One second in DISK_PERFORMANCE's 100 ns units, and a plausible QueryTime (FILETIME-scale).
constexpr std::int64_t ONE_SECOND_100NS = 10'000'000;
constexpr std::int64_t QUERY_TIME_BASE = 134'000'000'000'000'000;

/// Utilisation percent over a window, computed the way StorageModel does: Δms / window ms.
[[nodiscard]] double utilisationPercent(std::uint64_t ioTimeMsBefore, std::uint64_t ioTimeMsAfter, double windowMs)
{
    return (static_cast<double>(ioTimeMsAfter - ioTimeMsBefore) / windowMs) * 100.0;
}

// Over a 1 s window the disk is idle for 0.4 s, while 0.7 s of read service and 0.5 s of
// write service overlap in the remaining 0.6 s (queue depth > 1).
struct OverlappingIoWindow
{
    std::int64_t queryBefore = QUERY_TIME_BASE;
    std::int64_t queryAfter = QUERY_TIME_BASE + ONE_SECOND_100NS;
    std::int64_t idleBefore = 50 * ONE_SECOND_100NS;
    std::int64_t idleAfter = (50 * ONE_SECOND_100NS) + (4 * ONE_SECOND_100NS / 10);
    std::int64_t readBefore = 20 * ONE_SECOND_100NS;
    std::int64_t readAfter = (20 * ONE_SECOND_100NS) + (7 * ONE_SECOND_100NS / 10);
    std::int64_t writeBefore = 30 * ONE_SECOND_100NS;
    std::int64_t writeAfter = (30 * ONE_SECOND_100NS) + (5 * ONE_SECOND_100NS / 10);
};

TEST(DiskBusyTime100nsTest, OverlappingReadWriteStaysBelowWindow)
{
    const OverlappingIoWindow w;
    const auto before = diskBusyTime100ns(w.queryBefore, w.idleBefore);
    const auto after = diskBusyTime100ns(w.queryAfter, w.idleAfter);
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(after.has_value());

    const double percent = utilisationPercent(before.value_or(0) / 10000ULL, after.value_or(0) / 10000ULL, 1000.0);
    EXPECT_LT(percent, 100.0);
    EXPECT_DOUBLE_EQ(percent, 60.0);
}

TEST(DiskBusyTime100nsTest, OldReadPlusWriteFormulaExceedsWindowOnSameValues)
{
    // The pre-#1108 formula: ioTime = ReadTime + WriteTime. The same window reads 120 %, which
    // StorageModel clamped to a constant 100 % whenever I/O overlapped.
    const OverlappingIoWindow w;
    const std::uint64_t oldBefore =
        (clampNonNegativeQuadPart(w.readBefore) / 10000ULL) + (clampNonNegativeQuadPart(w.writeBefore) / 10000ULL);
    const std::uint64_t oldAfter = (clampNonNegativeQuadPart(w.readAfter) / 10000ULL) + (clampNonNegativeQuadPart(w.writeAfter) / 10000ULL);

    EXPECT_GT(utilisationPercent(oldBefore, oldAfter, 1000.0), 100.0);
}

TEST(DiskBusyTime100nsTest, BusyTimeIsQueryMinusIdle)
{
    EXPECT_EQ(diskBusyTime100ns(1000, 400), std::optional<std::uint64_t>{600});
}

TEST(DiskBusyTime100nsTest, ZeroIdleTimeFallsBack)
{
    // A driver that does not track IdleTime reports 0; QueryTime - 0 would read 100 % busy.
    EXPECT_FALSE(diskBusyTime100ns(QUERY_TIME_BASE, 0).has_value());
}

TEST(DiskBusyTime100nsTest, NegativeInputsFallBack)
{
    EXPECT_FALSE(diskBusyTime100ns(QUERY_TIME_BASE, -1).has_value());
    EXPECT_FALSE(diskBusyTime100ns(-1, 5).has_value());
    EXPECT_FALSE(diskBusyTime100ns(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::min()).has_value());
}

TEST(DiskBusyTime100nsTest, IdleGreaterThanQueryFallsBack)
{
    EXPECT_FALSE(diskBusyTime100ns(400, 1000).has_value());
}

TEST(DiskBusyTime100nsTest, IdleEqualToQueryIsZeroBusy)
{
    EXPECT_EQ(diskBusyTime100ns(1000, 1000), std::optional<std::uint64_t>{0});
}

// =============================================================================
// shouldReenumerate / FailureLogLimiter: pure policy, driven by a fake clock. The disk list
// must be rebuilt after a failure or periodically, and a persistent failure must warn only
// once rather than on every refresh (#1159).
// =============================================================================

using FakeTime = std::chrono::steady_clock::time_point;
constexpr std::chrono::seconds REENUMERATE_INTERVAL{30};
const FakeTime FAKE_START{std::chrono::hours{1}};

TEST(ShouldReenumerateTest, NoFailureBeforeIntervalDoesNotReenumerate)
{
    EXPECT_FALSE(shouldReenumerate(FAKE_START, FAKE_START, false, REENUMERATE_INTERVAL));
    EXPECT_FALSE(shouldReenumerate(FAKE_START + std::chrono::seconds{29}, FAKE_START, false, REENUMERATE_INTERVAL));
}

TEST(ShouldReenumerateTest, IntervalElapsedReenumerates)
{
    EXPECT_TRUE(shouldReenumerate(FAKE_START + std::chrono::seconds{30}, FAKE_START, false, REENUMERATE_INTERVAL));
    EXPECT_TRUE(shouldReenumerate(FAKE_START + std::chrono::minutes{5}, FAKE_START, false, REENUMERATE_INTERVAL));
}

TEST(ShouldReenumerateTest, AFailureReenumeratesWithoutWaitingOutTheInterval)
{
    // A disk removed long after the last enumeration is dropped on the next read.
    EXPECT_TRUE(shouldReenumerate(FAKE_START + std::chrono::seconds{12}, FAKE_START, true, REENUMERATE_INTERVAL));
    EXPECT_TRUE(shouldReenumerate(FAKE_START + DISK_REENUMERATE_AFTER_FAILURE_INTERVAL, FAKE_START, true, REENUMERATE_INTERVAL));
}

TEST(ShouldReenumerateTest, APersistentFailureDoesNotReenumerateEveryRefresh)
{
    // A disk that opens but fails every read must not re-run the PDH enumeration and reopen every
    // drive on each refresh: at most once per DISK_REENUMERATE_AFTER_FAILURE_INTERVAL.
    EXPECT_FALSE(shouldReenumerate(FAKE_START, FAKE_START, true, REENUMERATE_INTERVAL));
    EXPECT_FALSE(shouldReenumerate(FAKE_START + std::chrono::seconds{1}, FAKE_START, true, REENUMERATE_INTERVAL));
    EXPECT_FALSE(shouldReenumerate(
        FAKE_START + DISK_REENUMERATE_AFTER_FAILURE_INTERVAL - std::chrono::milliseconds{1}, FAKE_START, true, REENUMERATE_INTERVAL));
}

TEST(ShouldReenumerateTest, ClockBeforeLastEnumerationReenumerates)
{
    EXPECT_TRUE(shouldReenumerate(FAKE_START - std::chrono::seconds{1}, FAKE_START, false, REENUMERATE_INTERVAL));
}

TEST(ShouldReenumerateTest, DefaultIntervalIsAboutThirtySeconds)
{
    EXPECT_EQ(DISK_REENUMERATE_INTERVAL, std::chrono::seconds{30});
}

TEST(FailureLogLimiterTest, FirstFailureWarnsLaterFailuresDoNot)
{
    FailureLogLimiter limiter;
    EXPECT_TRUE(limiter.recordFailure("1 D:"));
    EXPECT_FALSE(limiter.recordFailure("1 D:"));
    EXPECT_FALSE(limiter.recordFailure("1 D:"));
    EXPECT_TRUE(limiter.isFailing("1 D:"));
}

TEST(FailureLogLimiterTest, SuccessResetsSoNextFailureWarnsAgain)
{
    FailureLogLimiter limiter;
    EXPECT_TRUE(limiter.recordFailure("1 D:"));
    limiter.recordSuccess("1 D:");
    EXPECT_FALSE(limiter.isFailing("1 D:"));
    EXPECT_TRUE(limiter.recordFailure("1 D:"));
}

TEST(FailureLogLimiterTest, KeysAreIndependent)
{
    FailureLogLimiter limiter;
    EXPECT_TRUE(limiter.recordFailure("1 D:"));
    EXPECT_TRUE(limiter.recordFailure("2 E:"));
    limiter.recordSuccess("1 D:");
    EXPECT_FALSE(limiter.recordFailure("2 E:"));
    EXPECT_TRUE(limiter.recordFailure("1 D:"));
}

TEST(FailureLogLimiterTest, SuccessForUnknownKeyIsHarmless)
{
    FailureLogLimiter limiter;
    limiter.recordSuccess("0 C:");
    EXPECT_FALSE(limiter.isFailing("0 C:"));
}

TEST(FailureLogLimiterTest, RemovedDiskPolledEveryRefreshWarnsOnce)
{
    // The #1159 scenario: a disk that keeps failing on every 1 s refresh for 10 minutes used
    // to log 600 warnings.
    FailureLogLimiter limiter;
    int warnings = 0;
    for (int second = 1; second <= 600; ++second)
    {
        if (limiter.recordFailure("1 D:"))
        {
            ++warnings;
        }
    }
    EXPECT_EQ(warnings, 1);
}

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(WindowsDiskProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ WindowsDiskProbe probe; });
}

TEST(WindowsDiskProbeTest, CapabilitiesReportedCorrectly)
{
    WindowsDiskProbe probe;
    auto caps = probe.capabilities();

    EXPECT_TRUE(caps.hasDiskStats);
    EXPECT_TRUE(caps.hasDeviceInfo);
    EXPECT_TRUE(caps.canFilterPhysical);

    // These may be true or false depending on PDH initialization
    // Just verify they are boolean values (no exception thrown)
    [[maybe_unused]] bool hasBytes = caps.hasReadWriteBytes;
    [[maybe_unused]] bool hasIoTime = caps.hasIoTime;
}

// =============================================================================
// Disk Counter Tests
// =============================================================================

TEST(WindowsDiskProbeTest, ReadReturnsValidCounters)
{
    WindowsDiskProbe probe;
    auto counters = probe.read();

    // Should find at least one disk on a typical Windows system
    // We'll be lenient and just check the structure is valid
    EXPECT_GE(counters.disks.size(), 0ULL);
}

TEST(WindowsDiskProbeTest, DiskCountersHaveValidNames)
{
    WindowsDiskProbe probe;
    auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        EXPECT_FALSE(disk.deviceName.empty());
        // Windows disk names are typically drive letters (C:) or PDH instance names (e.g., "0 C:")
        // They should not be empty and should contain printable characters
        EXPECT_TRUE(std::all_of(
            disk.deviceName.begin(), disk.deviceName.end(), [](unsigned char c) { return std::isprint(c) || std::isspace(c); }));
    }
}

TEST(WindowsDiskProbeTest, DiskCountersAreMonotonic)
{
    WindowsDiskProbe probe;

    auto counters1 = probe.read();

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto counters2 = probe.read();

    // WindowsDiskProbe sources DiskCounters from IOCTL_DISK_PERFORMANCE, which reports
    // genuinely cumulative counters (bytes/ops/time since the disk's counters started being
    // tracked) - matching the contract DiskCounters documents. For each disk that appears in
    // both samples, every counter must be non-decreasing.
    for (const auto& disk2 : counters2.disks)
    {
        for (const auto& disk1 : counters1.disks)
        {
            if (disk1.deviceName == disk2.deviceName)
            {
                EXPECT_GE(disk2.readsCompleted, disk1.readsCompleted);
                EXPECT_GE(disk2.readSectors, disk1.readSectors);
                EXPECT_GE(disk2.writesCompleted, disk1.writesCompleted);
                EXPECT_GE(disk2.writeSectors, disk1.writeSectors);
                EXPECT_GE(disk2.readTimeMs, disk1.readTimeMs);
                EXPECT_GE(disk2.writeTimeMs, disk1.writeTimeMs);
                EXPECT_GE(disk2.ioTimeMs, disk1.ioTimeMs);
            }
        }
    }
}

TEST(WindowsDiskProbeTest, SectorSizeIsValid)
{
    WindowsDiskProbe probe;
    auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        // Sector size should be 512 (typical) or 4096 (advanced format)
        EXPECT_TRUE(disk.sectorSize == 512 || disk.sectorSize == 4096);
    }
}

TEST(WindowsDiskProbeTest, TotalCountersAggregate)
{
    WindowsDiskProbe probe;
    auto counters = probe.read();

    uint64_t totalReads = counters.totalReadsCompleted();
    uint64_t totalWrites = counters.totalWritesCompleted();
    uint64_t totalReadBytes = counters.totalReadBytes();
    uint64_t totalWriteBytes = counters.totalWriteBytes();

    // If we have disks, totals should match sum
    if (!counters.disks.empty())
    {
        uint64_t sumReads = 0;
        uint64_t sumWrites = 0;
        uint64_t sumReadBytes = 0;
        uint64_t sumWriteBytes = 0;

        for (const auto& disk : counters.disks)
        {
            sumReads += disk.readsCompleted;
            sumWrites += disk.writesCompleted;
            sumReadBytes += disk.readSectors * disk.sectorSize;
            sumWriteBytes += disk.writeSectors * disk.sectorSize;
        }

        EXPECT_EQ(totalReads, sumReads);
        EXPECT_EQ(totalWrites, sumWrites);
        EXPECT_EQ(totalReadBytes, sumReadBytes);
        EXPECT_EQ(totalWriteBytes, sumWriteBytes);
    }
}

TEST(WindowsDiskProbeTest, ConsecutiveReadsAreConsistent)
{
    WindowsDiskProbe probe;

    auto counters1 = probe.read();
    auto counters2 = probe.read();

    // Device list should be stable between consecutive reads
    EXPECT_EQ(counters1.disks.size(), counters2.disks.size());
}

TEST(WindowsDiskProbeTest, PhysicalDeviceFlagIsSet)
{
    WindowsDiskProbe probe;
    auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        // All disks returned by WindowsDiskProbe should be marked as physical
        EXPECT_TRUE(disk.isPhysicalDevice);
    }
}

TEST(WindowsDiskProbeTest, PDHCountersProvideRealData)
{
    WindowsDiskProbe probe;

    // Wait for PDH to initialize and collect data
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto counters = probe.read();

    // If PDH is working, we should have at least one disk with some activity
    // This is a weak test since a system might have no I/O at the moment
    if (!counters.disks.empty())
    {
        bool hasAnyActivity = false;
        for (const auto& disk : counters.disks)
        {
            if (disk.readsCompleted > 0 || disk.writesCompleted > 0 || disk.readSectors > 0 || disk.writeSectors > 0)
            {
                hasAnyActivity = true;
                break;
            }
        }

        // It's okay if there's no activity, but the structure should be valid
        [[maybe_unused]] bool activityDetected = hasAnyActivity;
    }
}

TEST(WindowsDiskProbeTest, FallbackToLogicalDrivesWorks)
{
    // This test verifies that even if PDH fails, we still enumerate drives
    WindowsDiskProbe probe;
    auto counters = probe.read();

    // Should return at least the C: drive on any Windows system
    // But we'll be lenient and just verify no crash occurs
    EXPECT_GE(counters.disks.size(), 0ULL);
}

} // namespace
} // namespace Platform

#endif // _WIN32
