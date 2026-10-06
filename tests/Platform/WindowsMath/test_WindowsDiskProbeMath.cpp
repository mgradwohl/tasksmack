/// @file test_WindowsDiskProbeMath.cpp
/// @brief Unit tests for WindowsDiskProbeMath.h's pure counter, parsing and re-enumeration logic
///
/// WindowsDiskProbeMath.h includes no Windows header, so these tests build and run on every
/// platform, including Linux CI's sanitizer and coverage jobs (#1133). Tests that need the real
/// probe stay in test_WindowsDiskProbe.cpp.

#include "Platform/Windows/WindowsDiskProbeMath.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

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
// advanceDiskBusy: pure math, no hardware required. Utilisation is Δbusy / Δelapsed, so busy
// time must not grow faster than elapsed time even when reads and writes overlap (#1108), and it
// must follow a monotonic clock, not DISK_PERFORMANCE's adjustable QueryTime.
// =============================================================================

// One second in DISK_PERFORMANCE's 100 ns units, and a plausible monotonic clock reading.
constexpr std::int64_t ONE_SECOND_100NS = 10'000'000;
constexpr std::int64_t ELAPSED_BASE = 7'000 * ONE_SECOND_100NS;

/// Utilisation percent over a window, computed the way StorageModel does: Δms / window ms.
[[nodiscard]] double utilisationPercent(std::uint64_t ioTimeMsBefore, std::uint64_t ioTimeMsAfter, double windowMs)
{
    return (static_cast<double>(ioTimeMsAfter - ioTimeMsBefore) / windowMs) * 100.0;
}

// Over a 1 s window the disk is idle for 0.4 s, while 0.7 s of read service and 0.5 s of
// write service overlap in the remaining 0.6 s (queue depth > 1).
struct OverlappingIoWindow
{
    std::int64_t elapsedBefore = ELAPSED_BASE;
    std::int64_t elapsedAfter = ELAPSED_BASE + ONE_SECOND_100NS;
    std::int64_t idleBefore = 50 * ONE_SECOND_100NS;
    std::int64_t idleAfter = (50 * ONE_SECOND_100NS) + (4 * ONE_SECOND_100NS / 10);
    std::int64_t readBefore = 20 * ONE_SECOND_100NS;
    std::int64_t readAfter = (20 * ONE_SECOND_100NS) + (7 * ONE_SECOND_100NS / 10);
    std::int64_t writeBefore = 30 * ONE_SECOND_100NS;
    std::int64_t writeAfter = (30 * ONE_SECOND_100NS) + (5 * ONE_SECOND_100NS / 10);
};

TEST(AdvanceDiskBusyTest, OverlappingReadWriteStaysBelowWindow)
{
    const OverlappingIoWindow w;
    DiskBusyClock clock;
    const auto before = advanceDiskBusy(clock, w.elapsedBefore, w.idleBefore);
    const auto after = advanceDiskBusy(clock, w.elapsedAfter, w.idleAfter);
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(after.has_value());

    const double percent = utilisationPercent(before.value_or(0) / 10000ULL, after.value_or(0) / 10000ULL, 1000.0);
    EXPECT_LT(percent, 100.0);
    EXPECT_DOUBLE_EQ(percent, 60.0);
}

TEST(AdvanceDiskBusyTest, OldReadPlusWriteFormulaExceedsWindowOnSameValues)
{
    // The pre-#1108 formula: ioTime = ReadTime + WriteTime. The same window reads 120 %, which
    // StorageModel clamped to a constant 100 % whenever I/O overlapped.
    const OverlappingIoWindow w;
    const std::uint64_t oldBefore =
        (clampNonNegativeQuadPart(w.readBefore) / 10000ULL) + (clampNonNegativeQuadPart(w.writeBefore) / 10000ULL);
    const std::uint64_t oldAfter = (clampNonNegativeQuadPart(w.readAfter) / 10000ULL) + (clampNonNegativeQuadPart(w.writeAfter) / 10000ULL);

    EXPECT_GT(utilisationPercent(oldBefore, oldAfter, 1000.0), 100.0);
}

// #1108 review: QueryTime is adjustable system time. With busy = QueryTime - IdleTime, a +5 s clock
// correction during a 60 %-busy second added 5 s of "busy" time (clamped to 100 %), and a backward
// one read as idle. Busy time now follows the caller's monotonic clock, which a correction leaves
// alone, so the same second reads 60 % whatever the system clock did.
TEST(AdvanceDiskBusyTest, SystemClockCorrectionsDoNotReadAsDiskActivity)
{
    const OverlappingIoWindow w;
    constexpr std::int64_t CLOCK_STEP = 5 * ONE_SECOND_100NS;

    // The old formula, with QueryTime stepping forward by the correction as well as the second.
    const std::int64_t oldBusyBefore = (ELAPSED_BASE + 100) - w.idleBefore;
    const std::int64_t oldBusyAfter = (ELAPSED_BASE + 100 + ONE_SECOND_100NS + CLOCK_STEP) - w.idleAfter;
    EXPECT_GT(utilisationPercent(
                  static_cast<std::uint64_t>(oldBusyBefore) / 10000ULL, static_cast<std::uint64_t>(oldBusyAfter) / 10000ULL, 1000.0),
              100.0);

    DiskBusyClock clock;
    const auto before = advanceDiskBusy(clock, w.elapsedBefore, w.idleBefore);
    const auto after = advanceDiskBusy(clock, w.elapsedAfter, w.idleAfter);
    EXPECT_DOUBLE_EQ(utilisationPercent(before.value_or(0) / 10000ULL, after.value_or(0) / 10000ULL, 1000.0), 60.0);
}

TEST(AdvanceDiskBusyTest, BusyIsElapsedLessIdleGrowthSinceTheFirstRead)
{
    DiskBusyClock clock;
    EXPECT_EQ(advanceDiskBusy(clock, 1000, 400), std::optional<std::uint64_t>{0});
    EXPECT_EQ(advanceDiskBusy(clock, 2000, 700), std::optional<std::uint64_t>{700});  // 1000 elapsed, 300 idle
    EXPECT_EQ(advanceDiskBusy(clock, 2500, 1200), std::optional<std::uint64_t>{700}); // All idle: no change
}

TEST(AdvanceDiskBusyTest, ZeroOrNegativeIdleTimeFallsBack)
{
    // A driver that does not track IdleTime reports 0; elapsed - 0 would read 100 % busy.
    DiskBusyClock clock;
    EXPECT_FALSE(advanceDiskBusy(clock, ELAPSED_BASE, 0).has_value());
    EXPECT_FALSE(advanceDiskBusy(clock, ELAPSED_BASE, -1).has_value());
    EXPECT_FALSE(advanceDiskBusy(clock, ELAPSED_BASE, std::numeric_limits<std::int64_t>::min()).has_value());
}

TEST(AdvanceDiskBusyTest, NeverDecreasesWhenIdleOutpacesElapsed)
{
    // Idle growing faster than the clock (skew between the two) reads as no busy time, not negative.
    DiskBusyClock clock;
    ASSERT_TRUE(advanceDiskBusy(clock, 1000, 100).has_value());
    EXPECT_EQ(advanceDiskBusy(clock, 2000, 600), std::optional<std::uint64_t>{500});
    EXPECT_EQ(advanceDiskBusy(clock, 2100, 900), std::optional<std::uint64_t>{500});
}

TEST(AdvanceDiskBusyTest, AnIdleCounterResetRebaselinesWithoutLosingBusyTime)
{
    // IdleTime going backwards (the counter reset, e.g. the device came back) starts a new
    // baseline; busy time carries on from where it was rather than jumping or going backwards.
    DiskBusyClock clock;
    ASSERT_TRUE(advanceDiskBusy(clock, 1000, 100).has_value());
    EXPECT_EQ(advanceDiskBusy(clock, 2000, 600), std::optional<std::uint64_t>{500});
    EXPECT_EQ(advanceDiskBusy(clock, 3000, 50), std::optional<std::uint64_t>{500});   // Reset: new baseline
    EXPECT_EQ(advanceDiskBusy(clock, 4000, 350), std::optional<std::uint64_t>{1200}); // 500 + (1000 - 300)
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

} // namespace
} // namespace Platform
