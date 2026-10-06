/// @file test_Format.cpp
/// @brief Tests for UI::Format functions
///
/// Tests cover:
/// - CPU affinity mask formatting

#include "UI/Format.h"

#include <gtest/gtest.h>

#include <array>
#include <clocale>
#include <cstdint>
#include <format>
#include <limits>
#include <locale>
#include <random>
#include <string>
#include <utility>
#include <vector>

// =============================================================================
// CPU Affinity Mask Formatting Tests
// =============================================================================

TEST(FormatTest, AffinityMaskZeroShowsDash)
{
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x0), "-");
}

TEST(FormatTest, AffinityMaskSingleCore)
{
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x1), "0");                    // Core 0
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x2), "1");                    // Core 1
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x4), "2");                    // Core 2
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x8), "3");                    // Core 3
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x10), "4");                   // Core 4
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x8000000000000000ULL), "63"); // Core 63
}

TEST(FormatTest, AffinityMaskConsecutiveCores)
{
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x3), "0,1");  // Cores 0,1
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0xF), "0-3");  // Cores 0-3
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0xFF), "0-7"); // Cores 0-7
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0xF0), "4-7"); // Cores 4-7
}

TEST(FormatTest, AffinityMaskNonConsecutiveCores)
{
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x5), "0,2");      // Cores 0,2
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x15), "0,2,4");   // Cores 0,2,4
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x55), "0,2,4,6"); // Cores 0,2,4,6
}

TEST(FormatTest, AffinityMaskMixedRanges)
{
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0xF3), "0,1,4-7");  // Cores 0,1,4-7
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x1F5), "0,2,4-8"); // Cores 0,2,4-8
}

TEST(FormatTest, AffinityMaskAllCores)
{
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0xFFFFFFFFFFFFFFFFULL), "0-63");
}

TEST(FormatTest, AffinityMaskHighCores)
{
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0xF000000000000000ULL), "60-63");
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0x3000000000000000ULL), "60,61");
}

// #1247: affinities wider than 64 processors list exactly the processors allowed.
TEST(FormatTest, AffinityBeyond64Cpus)
{
    using Words = std::vector<std::uint64_t>;
    const auto format = [](const Words& words)
    {
        return UI::Format::formatCpuAffinity(words);
    };
    EXPECT_EQ(format(Words{}), "-");
    EXPECT_EQ(format(Words{0, 1ULL << 6U}), "70");                         // taskset -c 70
    EXPECT_EQ(format(Words{0xF, ~0ULL}), "0-3,64-127");                    // across the word boundary
    EXPECT_EQ(format(Words{0xF, 1ULL << 6U}), "0-3,70");                   // the issue's example
    EXPECT_EQ(format(Words{1ULL << 63U, 1}), "63,64");                     // a pair spanning two words
    EXPECT_EQ(format(Words{0xFULL << 62U, 0x3}), "62-65");                 // a run spanning two words
    EXPECT_EQ(format(Words{~0ULL, ~0ULL, ~0ULL, ~0ULL}), "0-255");         // all of a 256-CPU machine
    EXPECT_EQ(format(Words{0x1, 0, 0, 1ULL << 8U}), "0,200");              // whole zero words skipped
    EXPECT_EQ(format(Words{0, 0, 0}), "-");                                // no processor set
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(0xF), format(Words{0xF})); // the 64-bit form agrees
}
// =============================================================================
// Epoch Time Formatting Tests
// =============================================================================

TEST(FormatTest, EpochDateTimeZeroReturnsEmpty)
{
    EXPECT_EQ(UI::Format::formatEpochDateTime(0), "");
}

TEST(FormatTest, EpochDateTimeKnownValue)
{
    // 2024-01-15 12:00:00 UTC = 1705320000
    // The result depends on local timezone, so we just verify it's non-empty
    // and has the expected format (YYYY-MM-DD HH:MM:SS)
    const auto result = UI::Format::formatEpochDateTime(1705320000);
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result.length(), 19U); // "YYYY-MM-DD HH:MM:SS"
    EXPECT_EQ(result[4], '-');       // Year-month separator
    EXPECT_EQ(result[7], '-');       // Month-day separator
    EXPECT_EQ(result[10], ' ');      // Date-time separator
    EXPECT_EQ(result[13], ':');      // Hour-minute separator
    EXPECT_EQ(result[16], ':');      // Minute-second separator
}

TEST(FormatTest, EpochDateTimeShortZeroReturnsDash)
{
    EXPECT_EQ(UI::Format::formatEpochDateTimeShort(0), "-");
}

TEST(FormatTest, EpochDateTimeShortTodayShowsTime)
{
    // Use current time to test "today" case
    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    const auto result = UI::Format::formatEpochDateTimeShort(now);

    // Today should show "HH:MM:SS" format
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result.length(), 8U); // "HH:MM:SS"
    EXPECT_EQ(result[2], ':');      // Hour-minute separator
    EXPECT_EQ(result[5], ':');      // Minute-second separator
}

TEST(FormatTest, EpochDateTimeShortOlderShowsDate)
{
    // Use a time from several days ago
    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    const auto twoDaysAgo = now - (2 * 24 * 60 * 60);
    const auto result = UI::Format::formatEpochDateTimeShort(twoDaysAgo);

    // Should show "MMM DD HH:MM" format (not "Yesterday" or "HH:MM:SS")
    EXPECT_FALSE(result.empty());
    // Format is "MMM DD HH:MM" which is ~12 chars
    EXPECT_GE(result.length(), 11U);
}

// =============================================================================
// Date/Time Formatting Edge Case Tests
// =============================================================================

TEST(FormatTest, EpochDateTimeHandlesVeryLargeEpoch)
{
    // Test with a very large epoch value that exceeds time_t max on all platforms
    // This tests the guard check in formatEpochDateTime that returns empty for out-of-range values
    constexpr std::uint64_t veryLargeEpoch = 0xFFFFFFFFFFFFFFFFULL;
    const auto result = UI::Format::formatEpochDateTime(veryLargeEpoch);

    // Should return empty string since value exceeds max time_t (even on 64-bit)
    // The guard check at the start of formatEpochDateTime should catch this
    EXPECT_TRUE(result.empty() || result.length() >= 10);
}

TEST(FormatTest, EpochDateTimeShortHandlesVeryLargeEpoch)
{
    // Test with a very large epoch value
    constexpr std::uint64_t veryLargeEpoch = 0xFFFFFFFFFFFFFFFFULL;
    const auto result = UI::Format::formatEpochDateTimeShort(veryLargeEpoch);

    // Should return "-" on failure, or a valid formatted string
    // Either way, it should not crash
    EXPECT_FALSE(result.empty());
}

TEST(FormatTest, EpochDateTimeHandlesYear2038Boundary)
{
    // Test around the 32-bit signed overflow point (Jan 19, 2038 03:14:07 UTC)
    constexpr std::uint64_t year2038 = 2147483647ULL; // Max 32-bit signed value
    const auto result = UI::Format::formatEpochDateTime(year2038);

    // On 64-bit time_t systems (most modern systems), this will succeed
    // On 32-bit time_t systems, the guard check should return empty string
    // Either way, it should not crash
    EXPECT_TRUE(result.empty() || result.length() >= 10);
}

TEST(FormatTest, EpochDateTimeHandlesDistantFuture)
{
    // Test with a date far in the future (year 3000 approximately)
    constexpr std::uint64_t year3000 = 32503680000ULL;
    const auto result = UI::Format::formatEpochDateTime(year3000);

    // Should handle this gracefully
    // Should not crash regardless
    EXPECT_TRUE(result.empty() || result.length() >= 10);
}

// =============================================================================
// Numeric Conversion Tests
// =============================================================================

TEST(FormatTest, ToIntSaturatedClampsToIntMax)
{
    // Values beyond int max should clamp
    // Note: On Windows, long is 32-bit (same as int), so we can only test overflow
    // on platforms where long is larger than int (e.g., Linux where long is 64-bit)
#if LONG_MAX > INT_MAX
    // 64-bit long: can test overflow beyond INT_MAX
    const long largeValue = static_cast<long>(std::numeric_limits<int>::max()) + 1000L;
    EXPECT_EQ(UI::Format::toIntSaturated(largeValue), std::numeric_limits<int>::max());
#else
    // 32-bit long (Windows): test with INT_MAX itself since we can't exceed it
    EXPECT_EQ(UI::Format::toIntSaturated(static_cast<long>(std::numeric_limits<int>::max())), std::numeric_limits<int>::max());
#endif
}

TEST(FormatTest, ToIntSaturatedPreservesNormalValues)
{
    EXPECT_EQ(UI::Format::toIntSaturated(42L), 42);
    EXPECT_EQ(UI::Format::toIntSaturated(-42L), -42);
    EXPECT_EQ(UI::Format::toIntSaturated(0L), 0);
    EXPECT_EQ(UI::Format::toIntSaturated(100L), 100);
}

TEST(FormatTest, PercentToIntClampsNegativeToZero)
{
    EXPECT_EQ(UI::Format::percentToInt(-10.0), 0);
    EXPECT_EQ(UI::Format::percentToInt(0.0), 0);
    EXPECT_EQ(UI::Format::percentToInt(50.5), 51); // Rounds to nearest
    EXPECT_EQ(UI::Format::percentToInt(100.0), 100);
}

TEST(FormatTest, PercentHelpersTreatNaNAsNoReading)
{
    // NaN marks a sample with no reading; it used to print "-2,147,483,648%" and reach bar geometry (#1148).
    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double INF = std::numeric_limits<double>::infinity();
    EXPECT_EQ(UI::Format::percentToInt(NaN), 0);
    EXPECT_EQ(UI::Format::percentToInt(std::numeric_limits<float>::quiet_NaN()), 0);
    EXPECT_EQ(UI::Format::percentToInt(INF), std::numeric_limits<int>::max());
    EXPECT_EQ(UI::Format::percentToInt(-INF), 0);
    EXPECT_EQ(UI::Format::percentToInt(std::numeric_limits<double>::max()), std::numeric_limits<int>::max());
    EXPECT_EQ(UI::Format::percentToInt(std::numeric_limits<float>::max()), std::numeric_limits<int>::max());
    EXPECT_EQ(UI::Format::percentToInt(3.0e9), std::numeric_limits<int>::max()); // beyond a 32-bit long
    EXPECT_EQ(UI::Format::percentCompact(NaN), "N/A");
    EXPECT_EQ(UI::Format::percentCompact(std::numeric_limits<float>::quiet_NaN()), "N/A");
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(NaN), 0.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(INF), 100.0);
    EXPECT_DOUBLE_EQ(UI::Format::percent01(NaN), 0.0);
}

// =============================================================================
// clampPercent Tests (Heavily Used - 30+ call sites)
// =============================================================================

TEST(FormatTest, ClampPercentInRange)
{
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(50.0), 50.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(0.0), 0.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(100.0), 100.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(25.5), 25.5);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(99.9), 99.9);
}

TEST(FormatTest, ClampPercentAboveMax)
{
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(100.1), 100.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(150.0), 100.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(1000.0), 100.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(std::numeric_limits<double>::max()), 100.0);
}

TEST(FormatTest, ClampPercentBelowMin)
{
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(-0.1), 0.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(-50.0), 0.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(-1000.0), 0.0);
    EXPECT_DOUBLE_EQ(UI::Format::clampPercent(std::numeric_limits<double>::lowest()), 0.0);
}

TEST(FormatTest, ClampPercentHandlesSpecialValues)
{
    // NaN and infinity handling - clamp should produce finite results
    // Note: std::clamp behavior with NaN is implementation-defined
    // We just ensure no crash and a bounded result
    EXPECT_LE(UI::Format::clampPercent(std::numeric_limits<double>::infinity()), 100.0);
    EXPECT_GE(UI::Format::clampPercent(-std::numeric_limits<double>::infinity()), 0.0);
}

// =============================================================================
// percent01 Tests (Heavily Used - 20+ call sites)
// =============================================================================

TEST(FormatTest, Percent01InRange)
{
    EXPECT_DOUBLE_EQ(UI::Format::percent01(0.0), 0.0);
    EXPECT_DOUBLE_EQ(UI::Format::percent01(50.0), 0.5);
    EXPECT_DOUBLE_EQ(UI::Format::percent01(100.0), 1.0);
    EXPECT_DOUBLE_EQ(UI::Format::percent01(25.0), 0.25);
}

TEST(FormatTest, Percent01ClampsAndConverts)
{
    // Values outside [0, 100] should be clamped then converted
    EXPECT_DOUBLE_EQ(UI::Format::percent01(150.0), 1.0);
    EXPECT_DOUBLE_EQ(UI::Format::percent01(-50.0), 0.0);
    EXPECT_DOUBLE_EQ(UI::Format::percent01(200.0), 1.0);
}

TEST(FormatTest, Percent01Precision)
{
    // Test fractional percentages
    EXPECT_DOUBLE_EQ(UI::Format::percent01(33.33), 0.3333);
    EXPECT_NEAR(UI::Format::percent01(66.67), 0.6667, 1e-10);
}

// =============================================================================
// toFloatNarrow Tests (Used for ImGui/ImPlot interop)
// =============================================================================

TEST(FormatTest, ToFloatNarrowFromDouble)
{
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(0.0), 0.0F);
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(1.0), 1.0F);
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(-1.0), -1.0F);
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(3.14159), 3.14159F);
}

TEST(FormatTest, ToFloatNarrowFromIntegral)
{
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(0), 0.0F);
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(42), 42.0F);
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(-42), -42.0F);
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(1000000), 1000000.0F);
}

TEST(FormatTest, ToFloatNarrowFromUint64)
{
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(0ULL), 0.0F);
    EXPECT_FLOAT_EQ(UI::Format::toFloatNarrow(255ULL), 255.0F);
    // Large values lose precision but should not crash
    const auto largeValue = static_cast<std::uint64_t>(1) << 40;
    EXPECT_GT(UI::Format::toFloatNarrow(largeValue), 0.0F);
}

// =============================================================================
// checkedCount Tests (Safe narrowing for ImPlot series)
// =============================================================================

TEST(FormatTest, CheckedCountNormalValues)
{
    EXPECT_EQ(UI::Format::checkedCount(0), 0);
    EXPECT_EQ(UI::Format::checkedCount(100), 100);
    EXPECT_EQ(UI::Format::checkedCount(1000), 1000);
}

TEST(FormatTest, CheckedCountLargeValuesSaturate)
{
    // Values larger than INT_MAX should saturate to INT_MAX
    const auto hugeValue = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1000;
    EXPECT_EQ(UI::Format::checkedCount(hugeValue), std::numeric_limits<int>::max());
}

TEST(FormatTest, CheckedCountMaxSizeT)
{
    // SIZE_MAX should saturate to INT_MAX
    EXPECT_EQ(UI::Format::checkedCount(std::numeric_limits<std::size_t>::max()), std::numeric_limits<int>::max());
}

// =============================================================================
// Percentage Formatting Tests
// =============================================================================

TEST(FormatTest, PercentCompactFormatsCorrectly)
{
    // Just test that the function produces reasonable output
    const auto result = UI::Format::percentCompact(50.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains('%'));
}

// #1195: a process's share of the machine is usually a few percent or less, which percentCompact()
// rounded to "0%" or "1%" while the Processes table showed one decimal.
TEST(FormatTest, PercentOneDecimalKeepsTheFractionOfSmallPercents)
{
    EXPECT_EQ(UI::Format::percentOneDecimal(0.6), "0.6%");
    EXPECT_EQ(UI::Format::percentOneDecimal(6.25), "6.3%"); // Rounds half up, as the Processes table does
    EXPECT_EQ(UI::Format::percentOneDecimal(99.96), "100.0%");
    EXPECT_EQ(UI::Format::percentOneDecimal(0.0), "0.0%");
    EXPECT_EQ(UI::Format::percentOneDecimal(std::numeric_limits<double>::quiet_NaN()), "N/A");
}

TEST(FormatTest, PercentCompactHandlesZero)
{
    const auto result = UI::Format::percentCompact(0.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains('%'));
}

TEST(FormatTest, PercentCompactHandles100)
{
    const auto result = UI::Format::percentCompact(100.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains('%'));
}

// =============================================================================
// ID and Integer Formatting Tests
// =============================================================================

TEST(FormatTest, FormatIdFormatsCorrectly)
{
    // Just test that formatId produces reasonable output for various IDs
    const auto result = UI::Format::formatId(12345);
    EXPECT_FALSE(result.empty());
}

TEST(FormatTest, FormatIdHandlesZero)
{
    const auto result = UI::Format::formatId(0);
    EXPECT_FALSE(result.empty());
}

TEST(FormatTest, FormatIntLocalizedFormatsCorrectly)
{
    const auto result = UI::Format::formatIntLocalized(12345);
    EXPECT_FALSE(result.empty());
}

TEST(FormatTest, FormatUIntLocalizedFormatsCorrectly)
{
    const auto result = UI::Format::formatUIntLocalized(12345U);
    EXPECT_FALSE(result.empty());
}

TEST(FormatTest, FormatDoubleLocalizedFormatsCorrectly)
{
    const auto result = UI::Format::formatDoubleLocalized(123.456, 2);
    EXPECT_FALSE(result.empty());
}

TEST(FormatTest, FormatDoubleLocalizedHandlesZeroDecimals)
{
    const auto result = UI::Format::formatDoubleLocalized(123.456, 0);
    EXPECT_FALSE(result.empty());
}

// =============================================================================
// Count and Label Formatting Tests
// =============================================================================

TEST(FormatTest, FormatCountWithLabelFormatsCorrectly)
{
    const auto result = UI::Format::formatCountWithLabel(5, "processes");
    EXPECT_TRUE(result.contains("processes"));
}

TEST(FormatTest, FormatCountWithLabelZero)
{
    const auto result = UI::Format::formatCountWithLabel(0, "items");
    EXPECT_TRUE(result.contains("items"));
}

TEST(FormatTest, FormatCountWithLabelLargeNumber)
{
    const auto result = UI::Format::formatCountWithLabel(1000, "items");
    EXPECT_TRUE(result.contains("items"));
}

// =============================================================================
// Format Or Dash Tests
// =============================================================================

TEST(FormatTest, FormatOrDashReturnsFormattedValue)
{
    // formatOrDash requires a formatter function
    const auto result = UI::Format::formatOrDash(100, [](int v) { return std::format("{}", v); });
    EXPECT_EQ(result, "100");
}

TEST(FormatTest, FormatOrDashReturnsDashForZeroOrNegative)
{
    const auto resultZero = UI::Format::formatOrDash(0, [](int v) { return std::format("{}", v); });
    EXPECT_EQ(resultZero, "-");

    const auto resultNeg = UI::Format::formatOrDash(-5, [](int v) { return std::format("{}", v); });
    EXPECT_EQ(resultNeg, "-");
}

// =============================================================================
// Uptime Formatting Tests
// =============================================================================

TEST(FormatTest, FormatUptimeShortFormatsCorrectly)
{
    // 2 days, 5 hours, 30 minutes in seconds
    const std::uint64_t seconds = (2ULL * 24ULL * 60ULL * 60ULL) + (5ULL * 60ULL * 60ULL) + (30ULL * 60ULL);
    const auto result = UI::Format::formatUptimeShort(seconds);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("Up:"));
}

TEST(FormatTest, FormatUptimeShortHandlesSmallValues)
{
    const auto result = UI::Format::formatUptimeShort(300); // 5 minutes
    EXPECT_FALSE(result.empty());
}

TEST(FormatTest, FormatUptimeShortHandlesZero)
{
    const auto result = UI::Format::formatUptimeShort(0);
    EXPECT_TRUE(result.empty());
}

// =============================================================================
// Byte Unit Selection Tests
// =============================================================================

TEST(FormatTest, ChooseByteUnitSelectsBytes)
{
    const auto unit = UI::Format::chooseByteUnit(500.0);
    EXPECT_EQ(std::string(unit.suffix), "B");
}

TEST(FormatTest, ChooseByteUnitSelectsKilobytes)
{
    const auto unit = UI::Format::chooseByteUnit(2048.0);
    EXPECT_EQ(std::string(unit.suffix), "KiB");
}

TEST(FormatTest, ChooseByteUnitSelectsMegabytes)
{
    const auto unit = UI::Format::chooseByteUnit(2.0 * 1024.0 * 1024.0);
    EXPECT_EQ(std::string(unit.suffix), "MiB");
}

TEST(FormatTest, ChooseByteUnitSelectsGigabytes)
{
    const auto unit = UI::Format::chooseByteUnit(2.0 * 1024.0 * 1024.0 * 1024.0);
    EXPECT_EQ(std::string(unit.suffix), "GiB");
}

TEST(FormatTest, UnitForTotalBytesWorks)
{
    const auto unit = UI::Format::unitForTotalBytes(1024ULL * 1024ULL);
    EXPECT_EQ(std::string(unit.suffix), "MiB");
}

TEST(FormatTest, UnitForBytesPerSecondWorks)
{
    const auto unit = UI::Format::unitForBytesPerSecond(1024.0 * 1024.0);
    EXPECT_EQ(std::string(unit.suffix), "MiB");
}

// =============================================================================
// Byte Formatting Tests
// =============================================================================

TEST(FormatTest, FormatBytesFormatsCorrectly)
{
    const auto result = UI::Format::formatBytes(1536.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("KiB"));
}

TEST(FormatTest, FormatBytesWithUnitFormatsCorrectly)
{
    const auto unit = UI::Format::chooseByteUnit(1024.0 * 1024.0);
    const auto result = UI::Format::formatBytesWithUnit(1024.0 * 1024.0, unit);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("MiB"));
}

TEST(FormatTest, FormatBytesPerSecFormatsCorrectly)
{
    const auto result = UI::Format::formatBytesPerSec(1024.0 * 1024.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("MiB"));
    EXPECT_TRUE(result.contains("/s"));
}

TEST(FormatTest, FormatBytesPerSecOrNAShowsNAForAMissingSample)
{
    EXPECT_EQ(UI::Format::formatBytesPerSecOrNA(std::numeric_limits<double>::quiet_NaN()), "N/A");
    EXPECT_EQ(UI::Format::formatBytesPerSecOrNA(std::numeric_limits<double>::infinity()), "N/A");
    EXPECT_EQ(UI::Format::formatBytesPerSecOrNA(2048.0), UI::Format::formatBytesPerSec(2048.0));
}

TEST(FormatTest, FormatBytesPerSecWithUnitFormatsCorrectly)
{
    const auto unit = UI::Format::chooseByteUnit(1024.0);
    const auto result = UI::Format::formatBytesPerSecWithUnit(1024.0, unit);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("KiB"));
    EXPECT_TRUE(result.contains("/s"));
}

// =============================================================================
// Aligned Numeric Parts Tests
// =============================================================================

TEST(FormatTest, SplitBytesForAlignmentProducesParts)
{
    const auto unit = UI::Format::chooseByteUnit(1024.0 * 1024.0);
    const auto parts = UI::Format::splitBytesForAlignment(1024.0 * 1024.0, unit);

    EXPECT_FALSE(parts.wholePart.empty());
    EXPECT_FALSE(parts.unitPart.empty());
}

// ========== Comprehensive splitBytesForAlignment Tests ==========

TEST(FormatTest, SplitBytesForAlignmentZeroBytes)
{
    const auto unit = UI::Format::ByteUnit{.suffix = "B", .scale = 1.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(0.0, unit);

    EXPECT_EQ(parts.wholePart, "0.");
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_EQ(parts.unitPart, " B");
}

TEST(FormatTest, SplitBytesForAlignmentSmallBytes)
{
    const auto unit = UI::Format::ByteUnit{.suffix = "B", .scale = 1.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(512.0, unit);

    EXPECT_EQ(parts.wholePart, "512.");
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_EQ(parts.unitPart, " B");
}

TEST(FormatTest, SplitBytesForAlignmentKilobytes)
{
    const double bytes = 1536.0; // 1.5 KB
    const auto unit = UI::Format::ByteUnit{.suffix = "KiB", .scale = 1024.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(bytes, unit);

    EXPECT_EQ(parts.wholePart, "1.");
    EXPECT_EQ(parts.decimalPart, "5");
    EXPECT_EQ(parts.unitPart, " KiB");
}

TEST(FormatTest, SplitBytesForAlignmentMegabytes)
{
    const double bytes = 1024.0 * 1024.0 * 2.3; // 2.3 MB
    const auto unit = UI::Format::ByteUnit{.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(bytes, unit);

    EXPECT_EQ(parts.wholePart, "2.");
    EXPECT_EQ(parts.decimalPart, "3");
    EXPECT_EQ(parts.unitPart, " MiB");
}

TEST(FormatTest, SplitBytesForAlignmentGigabytes)
{
    const double bytes = 1024.0 * 1024.0 * 1024.0 * 8.7; // 8.7 GB
    const auto unit = UI::Format::ByteUnit{.suffix = "GiB", .scale = 1024.0 * 1024.0 * 1024.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(bytes, unit);

    EXPECT_EQ(parts.wholePart, "8.");
    EXPECT_EQ(parts.decimalPart, "7");
    EXPECT_EQ(parts.unitPart, " GiB");
}

TEST(FormatTest, SplitBytesForAlignmentRoundingOverflow)
{
    // 1023.95 bytes should round to 1024.0, which displays as "1,024."
    const auto unit = UI::Format::ByteUnit{.suffix = "B", .scale = 1.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(1023.95, unit);

    // When fractional rounds to 10, we carry to whole part
    EXPECT_EQ(parts.decimalPart, "0");
    // Whole part should be 1024 (with locale-dependent thousand separator)
    EXPECT_TRUE(parts.wholePart.contains("1"));
    EXPECT_TRUE(parts.wholePart.contains("024"));
    EXPECT_TRUE(parts.wholePart.ends_with("."));
}

TEST(FormatTest, SplitBytesForAlignmentThousandRange)
{
    // Test 1000-1023 range which requires thousand separator
    const auto unit = UI::Format::ByteUnit{.suffix = "B", .scale = 1.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(1000.0, unit);

    // Should contain "1" and "000" with a separator in between
    EXPECT_TRUE(parts.wholePart.contains("1"));
    EXPECT_TRUE(parts.wholePart.contains("000"));
    EXPECT_TRUE(parts.wholePart.ends_with("."));
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_EQ(parts.unitPart, " B");
}

TEST(FormatTest, SplitBytesForAlignmentThousandRangeKB)
{
    // 1023 KB = 1,047,552 bytes
    const double bytes = 1023.0 * 1024.0;
    const auto unit = UI::Format::ByteUnit{.suffix = "KiB", .scale = 1024.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(bytes, unit);

    // Should be "1,023." or similar with locale separator
    EXPECT_TRUE(parts.wholePart.contains("1"));
    EXPECT_TRUE(parts.wholePart.contains("023"));
    EXPECT_TRUE(parts.wholePart.ends_with("."));
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_EQ(parts.unitPart, " KiB");
}

TEST(FormatTest, SplitBytesForAlignmentSingleDigit)
{
    const auto unit = UI::Format::ByteUnit{.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(1024.0 * 1024.0 * 5.0, unit);

    EXPECT_EQ(parts.wholePart, "5.");
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_EQ(parts.unitPart, " MiB");
}

TEST(FormatTest, SplitBytesForAlignmentDoubleDigit)
{
    const auto unit = UI::Format::ByteUnit{.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(1024.0 * 1024.0 * 42.0, unit);

    EXPECT_EQ(parts.wholePart, "42.");
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_EQ(parts.unitPart, " MiB");
}

TEST(FormatTest, SplitBytesForAlignmentTripleDigit)
{
    const auto unit = UI::Format::ByteUnit{.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 1};
    const auto parts = UI::Format::splitBytesForAlignment(1024.0 * 1024.0 * 512.0, unit);

    EXPECT_EQ(parts.wholePart, "512.");
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_EQ(parts.unitPart, " MiB");
}

TEST(FormatTest, SplitBytesForAlignmentFractionalRounding)
{
    // Test various fractional values and their rounding
    const auto unit = UI::Format::ByteUnit{.suffix = "KiB", .scale = 1024.0, .decimals = 1};

    // 1.14 KB -> rounds to 1.1
    auto parts = UI::Format::splitBytesForAlignment(1024.0 * 1.14, unit);
    EXPECT_EQ(parts.wholePart, "1.");
    EXPECT_EQ(parts.decimalPart, "1");

    // 1.16 KB -> rounds to 1.2
    parts = UI::Format::splitBytesForAlignment(1024.0 * 1.16, unit);
    EXPECT_EQ(parts.wholePart, "1.");
    EXPECT_EQ(parts.decimalPart, "2");

    // 1.99 KB -> rounds to 2.0
    parts = UI::Format::splitBytesForAlignment(1024.0 * 1.99, unit);
    EXPECT_EQ(parts.wholePart, "2.");
    EXPECT_EQ(parts.decimalPart, "0");
}

// =============================================================================
// splitBytesForAlignmentFast vs splitBytesForAlignment comparison tests
// =============================================================================

/// Test fixture that sets up the user's locale (like the app does)
/// This ensures std::format("{:L}", ...) uses thousand separators
class FormatLocaleTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Save original locale
        m_OriginalLocale = std::locale();

        // Set up locale like main.cpp does
        try
        {
            const std::locale userLocale("");
            std::locale::global(userLocale);
        }
        catch (const std::exception&)
        {
            // If user's locale fails, try common locales with grouping
            const char* fallbackLocales[] = {"en_US.UTF-8", "en_GB.UTF-8", "C.UTF-8", nullptr};
            bool localeSet = false;
            for (const char** loc = fallbackLocales; *loc != nullptr; ++loc)
            {
                try
                {
                    std::locale::global(std::locale(*loc));
                    localeSet = true;
                    break;
                }
                catch (...)
                {
                    // Try next
                }
            }
            if (!localeSet)
            {
                GTEST_SKIP() << "No locale with thousand separators available";
            }
        }

        // Also set C locale for consistency
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        setlocale(LC_ALL, "");
    }

    void TearDown() override
    {
        // Restore original locale
        std::locale::global(m_OriginalLocale);
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        setlocale(LC_ALL, "C");
    }

  private:
    std::locale m_OriginalLocale;
};

namespace
{

/// Helper to compare AlignedNumericParts (slow) with AlignedBytesParts (fast)
/// Returns AssertionResult with detailed mismatch info on failure
auto compareBytesAlignment(double bytes, const UI::Format::ByteUnit& unit) -> ::testing::AssertionResult
{
    const auto slow = UI::Format::splitBytesForAlignment(bytes, unit);
    const auto fast = UI::Format::splitBytesForAlignmentFast(bytes, unit);

    // Compare whole part (string vs string_view)
    const bool wholeMatch = (slow.wholePart == fast.wholePart());

    // Compare decimal part (string "X" vs char 'X')
    const bool decimalMatch = (slow.decimalPart.size() == 1 && slow.decimalPart[0] == fast.decimalDigit);

    // Compare unit part - fast version uses static strings with leading space
    // slow version uses " X" format, fast uses static " B", " KiB", etc.
    const bool unitMatch = (slow.unitPart == fast.unitPart);

    if (!wholeMatch || !decimalMatch || !unitMatch)
    {
        return ::testing::AssertionFailure() << "Mismatch for bytes=" << bytes << " unit=" << unit.suffix << "\n"
                                             << "  slow.wholePart='" << slow.wholePart << "' vs fast.wholePart()='" << fast.wholePart()
                                             << "'\n"
                                             << "  slow.decimalPart='" << slow.decimalPart << "' vs fast.decimalDigit='"
                                             << fast.decimalDigit << "'\n"
                                             << "  slow.unitPart='" << slow.unitPart << "' vs fast.unitPart='" << fast.unitPart << "'";
    }
    return ::testing::AssertionSuccess();
}

} // namespace

TEST_F(FormatLocaleTest, SplitBytesForAlignmentFastMatchesSlowEdgeCases)
{
    // Test explicit edge cases
    const std::vector<double> edgeCases = {
        // Zero and small values
        0.0,
        0.1,
        0.5,
        0.9,
        0.95,
        0.99,
        1.0,

        // Single digit range
        5.0,
        9.0,
        9.5,
        9.9,
        9.95,

        // Double digit range
        10.0,
        42.0,
        99.0,
        99.5,
        99.9,
        99.95,

        // Triple digit range
        100.0,
        512.0,
        999.0,
        999.5,
        999.9,
        999.95,

        // Thousand range (requires separator)
        1000.0,
        1000.5,
        1023.0,
        1023.5,
        1023.9,
        1023.95,
        1024.0,

        // Larger values (rare after unit scaling but possible)
        2000.0,
        5000.0,
        9999.0,
        10000.0,
    };

    // Test with each unit type
    const std::vector<UI::Format::ByteUnit> units = {
        {.suffix = "B", .scale = 1.0, .decimals = 1},
        {.suffix = "KiB", .scale = 1024.0, .decimals = 1},
        {.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 1},
        {.suffix = "GiB", .scale = 1024.0 * 1024.0 * 1024.0, .decimals = 1},
    };

    for (const auto& unit : units)
    {
        for (double rawValue : edgeCases)
        {
            // Test the raw value as if it were already in the target unit
            const double bytes = rawValue * unit.scale;
            EXPECT_TRUE(compareBytesAlignment(bytes, unit)) << "Failed for rawValue=" << rawValue << " unit=" << unit.suffix;
        }
    }

    // Tested all edge case and unit combinations
}

TEST_F(FormatLocaleTest, SplitBytesForAlignmentFastMatchesSlowRandomValues)
{
    // Use a fixed seed for reproducibility
    std::mt19937 rng(42);

    // Different distributions for different ranges
    std::uniform_real_distribution<double> smallDist(0.0, 10.0);
    std::uniform_real_distribution<double> mediumDist(10.0, 1000.0);
    std::uniform_real_distribution<double> largeDist(1000.0, 10000.0);
    std::uniform_real_distribution<double> veryLargeDist(10000.0, 1000000.0);
    std::uniform_real_distribution<double> fractionalDist(0.0, 1.0);

    const std::vector<UI::Format::ByteUnit> units = {
        {.suffix = "B", .scale = 1.0, .decimals = 1},
        {.suffix = "KiB", .scale = 1024.0, .decimals = 1},
        {.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 1},
        {.suffix = "GiB", .scale = 1024.0 * 1024.0 * 1024.0, .decimals = 1},
    };

    constexpr int samplesPerRange = 100;

    for (const auto& unit : units)
    {
        // Small values (0-10)
        for (int i = 0; i < samplesPerRange; ++i)
        {
            const double rawValue = smallDist(rng);
            const double bytes = rawValue * unit.scale;
            EXPECT_TRUE(compareBytesAlignment(bytes, unit)) << "Small range failed for " << rawValue;
        }

        // Medium values (10-1000)
        for (int i = 0; i < samplesPerRange; ++i)
        {
            const double rawValue = mediumDist(rng);
            const double bytes = rawValue * unit.scale;
            EXPECT_TRUE(compareBytesAlignment(bytes, unit)) << "Medium range failed for " << rawValue;
        }

        // Large values (1000-10000) - thousand separator range
        for (int i = 0; i < samplesPerRange; ++i)
        {
            const double rawValue = largeDist(rng);
            const double bytes = rawValue * unit.scale;
            EXPECT_TRUE(compareBytesAlignment(bytes, unit)) << "Large range failed for " << rawValue;
        }

        // Very large values (10000-1000000) - unusual but possible
        for (int i = 0; i < samplesPerRange / 10; ++i)
        {
            const double rawValue = veryLargeDist(rng);
            const double bytes = rawValue * unit.scale;
            EXPECT_TRUE(compareBytesAlignment(bytes, unit)) << "Very large range failed for " << rawValue;
        }

        // Values near rounding boundaries (X.X5)
        for (int i = 0; i < samplesPerRange; ++i)
        {
            const int whole = static_cast<int>(mediumDist(rng));
            const double frac = static_cast<int>(fractionalDist(rng) * 10) / 10.0 + 0.05; // X.X5
            const double rawValue = whole + frac;
            const double bytes = rawValue * unit.scale;
            EXPECT_TRUE(compareBytesAlignment(bytes, unit)) << "Boundary failed for " << rawValue;
        }
    }

    // Tested random value combinations
}

TEST_F(FormatLocaleTest, SplitBytesForAlignmentFastMatchesSlowRealisticWorkload)
{
    // Simulate realistic process memory values
    std::mt19937 rng(123);

    // Process memory typically ranges from a few KB to several GB
    std::uniform_int_distribution<std::uint64_t> memDist(1024, 16ULL * 1024 * 1024 * 1024);

    constexpr int numProcesses = 500;

    for (int i = 0; i < numProcesses; ++i)
    {
        const auto memoryBytes = static_cast<double>(memDist(rng));

        // Use the real unit selection function
        const auto unit = UI::Format::chooseByteUnit(memoryBytes);

        EXPECT_TRUE(compareBytesAlignment(memoryBytes, unit)) << "Realistic workload failed for " << memoryBytes;
    }

    // Tested realistic memory values with random sampling
}

TEST(FormatTest, SplitBytesPerSecForAlignmentProducesParts)
{
    const auto unit = UI::Format::chooseByteUnit(1024.0);
    const auto parts = UI::Format::splitBytesPerSecForAlignment(1024.0, unit);

    EXPECT_FALSE(parts.wholePart.empty());
    EXPECT_FALSE(parts.unitPart.empty());
    EXPECT_TRUE(parts.unitPart.contains("/s"));
}

TEST(FormatTest, SplitPercentForAlignmentProducesParts)
{
    const auto parts = UI::Format::splitPercentForAlignment(75.5);

    EXPECT_FALSE(parts.wholePart.empty());
    EXPECT_EQ(parts.wholePart, "75.");
    EXPECT_EQ(parts.decimalDigit, '5');
    EXPECT_EQ(parts.unitPart, "%");
}

TEST(FormatTest, SplitPercentForAlignmentHandlesRounding)
{
    // Test rounding: 75.95 should round to 76.0
    const auto parts = UI::Format::splitPercentForAlignment(75.95);

    EXPECT_EQ(parts.wholePart, "76.");
    EXPECT_EQ(parts.decimalDigit, '0');
    EXPECT_EQ(parts.unitPart, "%");
}

TEST(FormatTest, SplitPercentForAlignmentHandlesBoundaries)
{
    // Test 0%
    const auto zero = UI::Format::splitPercentForAlignment(0.0);
    EXPECT_EQ(zero.wholePart, "0.");
    EXPECT_EQ(zero.decimalDigit, '0');

    // Test 100%
    const auto hundred = UI::Format::splitPercentForAlignment(100.0);
    EXPECT_EQ(hundred.wholePart, "100.");
    EXPECT_EQ(hundred.decimalDigit, '0');

    // Test clamping above 100
    const auto over = UI::Format::splitPercentForAlignment(150.0);
    EXPECT_EQ(over.wholePart, "100.");
    EXPECT_EQ(over.decimalDigit, '0');
}

// =============================================================================
// Power Formatting Tests
// =============================================================================

TEST(FormatTest, SplitPowerForAlignmentHandlesZero)
{
    const auto parts = UI::Format::splitPowerForAlignment(0.0);

    EXPECT_EQ(parts.wholePart, "0.");
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_TRUE(parts.unitPart.contains('W'));
}

TEST(FormatTest, SplitPowerForAlignmentHandlesWatts)
{
    const auto parts = UI::Format::splitPowerForAlignment(5.5);

    EXPECT_FALSE(parts.wholePart.empty());
    EXPECT_FALSE(parts.decimalPart.empty());
    EXPECT_TRUE(parts.unitPart.contains('W'));
}

TEST(FormatTest, SplitPowerForAlignmentHandlesMilliwatts)
{
    const auto parts = UI::Format::splitPowerForAlignment(0.005);

    EXPECT_FALSE(parts.wholePart.empty());
    EXPECT_TRUE(parts.unitPart.contains("mW"));
}

TEST(FormatTest, SplitPowerForAlignmentHandlesMicrowatts)
{
    const auto parts = UI::Format::splitPowerForAlignment(0.0005);

    EXPECT_FALSE(parts.wholePart.empty());
    // Note: µW uses UTF-8 encoding
    EXPECT_TRUE(parts.unitPart.contains('W'));
}

TEST(FormatTest, FormatPowerCompactHandlesZero)
{
    const auto result = UI::Format::formatPowerCompact(0.0);
    EXPECT_EQ(result, "-");
}

TEST(FormatTest, FormatPowerCompactHandlesNegative)
{
    const auto result = UI::Format::formatPowerCompact(-5.0);
    EXPECT_EQ(result, "-");
}

TEST(FormatTest, FormatPowerCompactHandlesWatts)
{
    const auto result = UI::Format::formatPowerCompact(15.5);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains('W'));
}

TEST(FormatTest, FormatPowerCompactHandlesMilliwatts)
{
    const auto result = UI::Format::formatPowerCompact(0.015);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("mW"));
}

TEST(FormatTest, FormatPowerCompactHandlesMicrowatts)
{
    // < 0.001 W → µW path
    const auto result = UI::Format::formatPowerCompact(0.0005);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains('W'));
    // µW uses the Unicode micro sign so just verify it is not mW
    EXPECT_FALSE(result.contains("mW"));
}

TEST(FormatTest, FormatPowerCompactBoundaryMilliWatt)
{
    // Exactly at the mW/W boundary (0.001 W)
    const auto mw = UI::Format::formatPowerCompact(0.001);
    EXPECT_TRUE(mw.contains("mW"));

    // Just below the mW boundary (0.0009 W) → µW path
    const auto uw = UI::Format::formatPowerCompact(0.0009);
    EXPECT_FALSE(uw.contains("mW"));
}

// =============================================================================
// splitPowerForAlignment additional branch tests
// =============================================================================

TEST(FormatTest, SplitPowerForAlignmentHandlesNegativeWatts)
{
    // Negative power should trigger the <= 0.0 guard and return the zero display
    const auto parts = UI::Format::splitPowerForAlignment(-5.0);

    EXPECT_EQ(parts.wholePart, "0.");
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_EQ(parts.unitPart, " W");
}

TEST(FormatTest, SplitPowerForAlignmentRoundingOverflow)
{
    // A value whose fractional part rounds up to 10 should carry to the whole part
    // Use an exactly representable binary fraction (4 + 31/32) to avoid FP ambiguity
    // while still exercising the carry-to-whole rounding path to 5.0 W.
    const auto parts = UI::Format::splitPowerForAlignment(4.96875);

    EXPECT_EQ(parts.wholePart, "5.");
    EXPECT_EQ(parts.decimalPart, "0");
    EXPECT_EQ(parts.unitPart, " W");
}

// =============================================================================
// formatPowerOrZero Tests
// =============================================================================

TEST(FormatTest, FormatPowerOrZeroHandlesZero)
{
    const auto result = UI::Format::formatPowerOrZero(0.0);
    EXPECT_EQ(result, "0.0 W");
}

TEST(FormatTest, FormatPowerOrZeroHandlesNegative)
{
    const auto result = UI::Format::formatPowerOrZero(-5.0);
    EXPECT_EQ(result, "0.0 W");
}

TEST(FormatTest, FormatPowerOrZeroHandlesPositiveWatts)
{
    const auto result = UI::Format::formatPowerOrZero(15.5);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains('W'));
    EXPECT_NE(result, "0.0 W");
}

TEST(FormatTest, FormatPowerOrZeroHandlesMilliwatts)
{
    const auto result = UI::Format::formatPowerOrZero(0.015);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("mW"));
}

TEST(FormatTest, FormatPowerOrZeroHandlesMicrowatts)
{
    const auto result = UI::Format::formatPowerOrZero(0.0005);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains('W'));
    EXPECT_NE(result, "0.0 W");
}

// =============================================================================
// formatUptimeShort — hours-only path (hours > 0, days == 0)
// =============================================================================

TEST(FormatTest, FormatUptimeShortHandlesHoursOnly)
{
    // 2 hours, 30 minutes, no days
    const std::uint64_t seconds = (2ULL * 60ULL * 60ULL) + (30ULL * 60ULL);
    const auto result = UI::Format::formatUptimeShort(seconds);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("Up:"));
    EXPECT_TRUE(result.contains("h"));
    EXPECT_TRUE(result.contains("m"));
}

TEST(FormatTest, FormatUptimeShortHandlesMinutesOnly)
{
    // Less than one hour: just minutes path
    const auto result = UI::Format::formatUptimeShort(45 * 60); // 45 minutes
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("Up:"));
    EXPECT_TRUE(result.contains("m"));
    EXPECT_FALSE(result.contains("h"));
    EXPECT_FALSE(result.contains("d"));
}

// =============================================================================
// splitBytesPerSecForAlignmentFast — unit suffix correctness
// =============================================================================

TEST_F(FormatLocaleTest, SplitBytesPerSecForAlignmentFastUnits)
{
    const std::vector<std::pair<UI::Format::ByteUnit, std::string>> unitExpected = {
        {{.suffix = "B", .scale = 1.0, .decimals = 1}, " B/s"},
        {{.suffix = "KiB", .scale = 1024.0, .decimals = 1}, " KiB/s"},
        {{.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 1}, " MiB/s"},
        {{.suffix = "GiB", .scale = 1024.0 * 1024.0 * 1024.0, .decimals = 1}, " GiB/s"},
    };

    for (const auto& [unit, expectedSuffix] : unitExpected)
    {
        const auto parts = UI::Format::splitBytesPerSecForAlignmentFast(512.0 * unit.scale, unit);
        EXPECT_EQ(parts.unitPart, expectedSuffix) << "Failed for unit=" << unit.suffix;
    }
}

TEST_F(FormatLocaleTest, SplitBytesPerSecForAlignmentFastMatchesSlowPath)
{
    const std::vector<UI::Format::ByteUnit> units = {
        {.suffix = "B", .scale = 1.0, .decimals = 1},
        {.suffix = "KiB", .scale = 1024.0, .decimals = 1},
        {.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 1},
        {.suffix = "GiB", .scale = 1024.0 * 1024.0 * 1024.0, .decimals = 1},
    };

    for (const auto& unit : units)
    {
        const double bytes = 512.0 * unit.scale; // 512 in each unit
        const auto slow = UI::Format::splitBytesPerSecForAlignment(bytes, unit);
        const auto fast = UI::Format::splitBytesPerSecForAlignmentFast(bytes, unit);

        EXPECT_EQ(slow.wholePart, fast.wholePart()) << "wholePart mismatch for " << unit.suffix;
        EXPECT_EQ(std::string(1, fast.decimalDigit), slow.decimalPart) << "decimalPart mismatch for " << unit.suffix;
        EXPECT_EQ(slow.unitPart, fast.unitPart) << "unitPart mismatch for " << unit.suffix;
    }
}

// =============================================================================
// splitPercentForAlignment — single-digit and rounding paths
// =============================================================================

TEST(FormatTest, SplitPercentForAlignmentSingleDigit)
{
    // Single-digit whole part (0-9)
    const auto parts = UI::Format::splitPercentForAlignment(7.3);
    EXPECT_EQ(parts.wholePart, "7.");
    EXPECT_EQ(parts.decimalDigit, '3');
}

TEST(FormatTest, SplitPercentForAlignmentDoubleDigit)
{
    // Two-digit whole part (10-99)
    const auto parts = UI::Format::splitPercentForAlignment(42.8);
    EXPECT_EQ(parts.wholePart, "42.");
    EXPECT_EQ(parts.decimalDigit, '8');
}

TEST(FormatTest, SplitPercentForAlignmentRoundingAt99_96875)
{
    // Use an exactly representable binary fraction (99 + 31/32) to avoid FP ambiguity
    // while still exercising the carry-to-whole rounding path to 100.0.
    const auto parts = UI::Format::splitPercentForAlignment(99.96875);
    EXPECT_EQ(parts.wholePart, "100.");
    EXPECT_EQ(parts.decimalDigit, '0');
}

// =============================================================================
// chooseByteUnit — boundary and negative value coverage
// =============================================================================

TEST(FormatTest, ChooseByteUnitHandlesNegativeBytes)
{
    // Negative bytes (unusual but possible from deltas): chooseByteUnit uses abs()
    const auto unit = UI::Format::chooseByteUnit(-2048.0);
    EXPECT_EQ(std::string(unit.suffix), "KiB");
}

TEST(FormatTest, ChooseByteUnitBoundaryExactlyGB)
{
    // Exactly 1 GiB → GB unit
    const auto unit = UI::Format::chooseByteUnit(1024.0 * 1024.0 * 1024.0);
    EXPECT_EQ(std::string(unit.suffix), "GiB");
}

TEST(FormatTest, ChooseByteUnitBoundaryExactlyMB)
{
    // Exactly 1 MiB → MB unit
    const auto unit = UI::Format::chooseByteUnit(1024.0 * 1024.0);
    EXPECT_EQ(std::string(unit.suffix), "MiB");
}

TEST(FormatTest, ChooseByteUnitBoundaryExactlyKB)
{
    // Exactly 1 KiB → KB unit
    const auto unit = UI::Format::chooseByteUnit(1024.0);
    EXPECT_EQ(std::string(unit.suffix), "KiB");
}

TEST(FormatTest, ChooseByteUnitZeroIsBytes)
{
    const auto unit = UI::Format::chooseByteUnit(0.0);
    EXPECT_EQ(std::string(unit.suffix), "B");
}

// =============================================================================
// FormatCountPerSecond — rate suffix coverage
// =============================================================================

TEST(FormatTest, FormatCountPerSecondSmallValue)
{
    const auto result = UI::Format::formatCountPerSecond(500.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("/s"));
}

TEST(FormatTest, FormatCountPerSecondThousands)
{
    const auto result = UI::Format::formatCountPerSecond(5000.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("K/s"));
}

TEST(FormatTest, FormatCountPerSecondMillions)
{
    const auto result = UI::Format::formatCountPerSecond(5000000.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("M/s"));
}

// =============================================================================
// Bytes Used/Total/Percent Compact Tests
// =============================================================================

TEST(FormatTest, BytesUsedTotalPercentCompactFormatsCorrectly)
{
    const auto result = UI::Format::bytesUsedTotalPercentCompact(512ULL * 1024ULL * 1024ULL, 1024ULL * 1024ULL * 1024ULL, 50.0);

    EXPECT_FALSE(result.empty());
    // Should contain "/" separator and percentage
    EXPECT_TRUE(result.contains('/'));
    EXPECT_TRUE(result.contains('%'));
}

// =============================================================================
// formatDuration: CPU Time, uptime and the time axis share one grammar (#1202)
// =============================================================================

TEST(FormatTest, FormatDurationUsesTheTwoLargestUnits)
{
    EXPECT_EQ(UI::Format::formatDuration(0.0), "0s");
    EXPECT_EQ(UI::Format::formatDuration(45.0), "45s");
    EXPECT_EQ(UI::Format::formatDuration(125.0), "2m 05s");
    EXPECT_EQ(UI::Format::formatDuration(3725.0), "1h 02m");
    EXPECT_EQ(UI::Format::formatDuration((3.0 * 86400.0) + (4.0 * 3600.0) + 59.0), "3d 04h");
}

TEST(FormatTest, FormatDurationRoundsToTheNearestSecondFirst)
{
    // 59.6 s is a minute, not "60s"; 0.4 s is "0s".
    EXPECT_EQ(UI::Format::formatDuration(59.6), "1m 00s");
    EXPECT_EQ(UI::Format::formatDuration(0.4), "0s");
    EXPECT_EQ(UI::Format::formatDuration(3599.5), "1h 00m");
}

TEST(FormatTest, FormatDurationFixedKeepsAZeroMinorPartCompactDropsIt)
{
    EXPECT_EQ(UI::Format::formatDuration(300.0), "5m 00s");
    EXPECT_EQ(UI::Format::formatDuration(300.0, UI::Format::DurationStyle::Compact), "5m");
    EXPECT_EQ(UI::Format::formatDuration(90.0, UI::Format::DurationStyle::Compact), "1m 30s");
    EXPECT_EQ(UI::Format::formatDuration(7200.0, UI::Format::DurationStyle::Compact), "2h");
    EXPECT_EQ(UI::Format::formatDuration(30.0, UI::Format::DurationStyle::Compact), "30s");
}

TEST(FormatTest, FormatDurationShowsTheSizeOfANegativeDurationAndNAForNaN)
{
    EXPECT_EQ(UI::Format::formatDuration(-300.0, UI::Format::DurationStyle::Compact), "5m");
    EXPECT_EQ(UI::Format::formatDuration(std::numeric_limits<double>::quiet_NaN()), "N/A");
    // Infinity is capped rather than reaching std::llround
    EXPECT_FALSE(UI::Format::formatDuration(std::numeric_limits<double>::infinity()).empty());
}

TEST(FormatTest, FormatUptimeShortUsesTheDurationGrammar)
{
    EXPECT_EQ(UI::Format::formatUptimeShort((2ULL * 86400ULL) + (5ULL * 3600ULL) + (30ULL * 60ULL)), "Up: 2d 05h");
    EXPECT_EQ(UI::Format::formatUptimeShort(45ULL * 60ULL), "Up: 45m 00s");
}

// =============================================================================
// Temperature, link speed and terabytes (#1202)
// =============================================================================

TEST(FormatTest, FormatCelsiusRoundsHalfAwayFromZero)
{
    EXPECT_EQ(UI::Format::formatCelsius(65.4), "65°C");
    EXPECT_EQ(UI::Format::formatCelsius(65.5), "66°C"); // The value strip used to truncate this to 65
    EXPECT_EQ(UI::Format::formatCelsius(64.5), "65°C"); // and std::format rounded this half to even, 64
    EXPECT_EQ(UI::Format::formatCelsius(-0.2), "0°C");
    EXPECT_EQ(UI::Format::formatCelsius(std::numeric_limits<double>::quiet_NaN()), "N/A");
}

TEST(FormatTest, FormatMegahertzIsWholeMegahertz)
{
    EXPECT_EQ(UI::Format::formatMegahertz(1850.0), "1850 MHz");
    EXPECT_EQ(UI::Format::formatMegahertz(1849.5), "1850 MHz");
    EXPECT_EQ(UI::Format::formatMegahertz(-0.2), "0 MHz");
    EXPECT_EQ(UI::Format::formatMegahertz(std::numeric_limits<double>::quiet_NaN()), "N/A");
}

TEST(FormatTest, FormatLinkSpeedIsADecimalBitRate)
{
    // Link speeds are stored in Mbit/s (Linux sysfs speed; Windows TransmitLinkSpeed / 10^6) and
    // shown in bits, as network hardware is rated (#1373).
    EXPECT_EQ(UI::Format::formatLinkSpeed(10), "10 Mbit/s");
    EXPECT_EQ(UI::Format::formatLinkSpeed(100), "100 Mbit/s");
    EXPECT_EQ(UI::Format::formatLinkSpeed(866), "866 Mbit/s"); // A Wi-Fi rate
    EXPECT_EQ(UI::Format::formatLinkSpeed(1000), "1 Gbit/s");
    EXPECT_EQ(UI::Format::formatLinkSpeed(2500), "2.5 Gbit/s");
    EXPECT_EQ(UI::Format::formatLinkSpeed(10'000), "10 Gbit/s");
    EXPECT_EQ(UI::Format::formatLinkSpeed(100'000), "100 Gbit/s");
    EXPECT_EQ(UI::Format::formatLinkSpeed(400'000), "400 Gbit/s");
}

TEST(FormatTest, FormatLinkSpeedRoundsToATenthOfAGigabit)
{
    EXPECT_EQ(UI::Format::formatLinkSpeed(999), "999 Mbit/s");
    EXPECT_EQ(UI::Format::formatLinkSpeed(1201), "1.2 Gbit/s");
    EXPECT_EQ(UI::Format::formatLinkSpeed(1250), "1.3 Gbit/s"); // Half rounds up
    EXPECT_EQ(UI::Format::formatLinkSpeed(2402), "2.4 Gbit/s");
    EXPECT_EQ(UI::Format::formatLinkSpeed(1999), "2 Gbit/s"); // Not "2.0 Gbit/s"
    EXPECT_EQ(UI::Format::formatLinkSpeed(10'049), "10 Gbit/s");
}

TEST(FormatTest, FormatLinkSpeedUnknownIsADash)
{
    // 0 is the probes' "unknown"; the Interface Status table shows "-" for it.
    EXPECT_EQ(UI::Format::formatLinkSpeed(0), "-");
}

TEST(FormatTest, FormatLinkSpeedAsByteRateIsInTheRatesUnits)
{
    // 1 Gbit/s = 125,000,000 bytes/s = 119.2 MiB/s; 10 Gbit/s = 1.2 GiB/s
    EXPECT_EQ(UI::Format::formatLinkSpeedAsByteRate(1000), "119.2 MiB/s");
    EXPECT_EQ(UI::Format::formatLinkSpeedAsByteRate(100), "11.9 MiB/s");
    EXPECT_EQ(UI::Format::formatLinkSpeedAsByteRate(10'000), "1.2 GiB/s");
    EXPECT_EQ(UI::Format::formatLinkSpeedAsByteRate(1000), UI::Format::formatBytesPerSec(125'000'000.0));
}

TEST(FormatTest, BytesAboveATebibyteUseTiB)
{
    constexpr double TIB = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    EXPECT_EQ(std::string(UI::Format::chooseByteUnit(TIB).suffix), "TiB");
    EXPECT_EQ(std::string(UI::Format::chooseByteUnit(TIB - 1.0).suffix), "GiB");
    EXPECT_EQ(UI::Format::formatBytes(2.0 * TIB), "2.0 TiB");
    EXPECT_EQ(UI::Format::formatBytesPerSec(1.5 * TIB), "1.5 TiB/s");
    // The table's aligned cells carry the same unit
    const auto parts = UI::Format::splitBytesPerSecForAlignmentFast(2.0 * TIB, UI::Format::BYTE_UNIT_TB);
    EXPECT_EQ(parts.unitPart, " TiB/s");
}

TEST(FormatTest, CellUnitSuffixCoversEveryByteUnit)
{
    for (const UI::Format::ByteUnit* unit : UI::Format::BYTE_UNITS)
    {
        EXPECT_EQ(UI::Format::cellUnitSuffix(*unit, false), " " + std::string(unit->suffix));
        EXPECT_EQ(UI::Format::cellUnitSuffix(*unit, true), " " + std::string(unit->suffix) + "/s");
    }
    // A unit that isn't one of them reads as bytes
    EXPECT_EQ(UI::Format::cellUnitSuffix(UI::Format::ByteUnit{.suffix = "XB", .scale = 1.0, .decimals = 1}, false), " B");
}

TEST(FormatTest, BytesUsedTotalPercentUsesFormatPercent)
{
    // Under 10 % the percent keeps a decimal, as everywhere else (formatPercent())
    EXPECT_EQ(UI::Format::bytesUsedTotalPercentCompact(1024ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL * 1024ULL, 6.25),
              "1.0 GiB / 16.0 GiB (6.3%)");
}

// =============================================================================
// splitBytesForAlignment — no-decimal (decimals=0) path
// =============================================================================

TEST(FormatTest, SplitBytesForAlignmentNoDecimalUnit)
{
    // ByteUnit with decimals=0 exercises the else branch (no fractional part)
    const UI::Format::ByteUnit unit{.suffix = "KiB", .scale = 1024.0, .decimals = 0};
    const auto parts = UI::Format::splitBytesForAlignment(2048.0, unit);
    EXPECT_EQ(parts.wholePart, "2");
    EXPECT_TRUE(parts.decimalPart.empty());
    EXPECT_EQ(parts.unitPart, " KiB");
}

TEST(FormatTest, SplitBytesForAlignmentNoDecimalZero)
{
    const UI::Format::ByteUnit unit{.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 0};
    const auto parts = UI::Format::splitBytesForAlignment(0.0, unit);
    EXPECT_EQ(parts.wholePart, "0");
    EXPECT_TRUE(parts.decimalPart.empty());
    EXPECT_EQ(parts.unitPart, " MiB");
}

// =============================================================================
// formatCpuAffinityMask — trailing range with prior bits (hasAny=true)
// =============================================================================

TEST(FormatTest, FormatCpuAffinityMaskTrailingAdjacentPairWithPrefix)
{
    // Bits {0,1} emit in-loop ("0,1"), then bits {62,63} are post-loop trailing
    // adjacent pair (rangeStart+1 == rangeEnd) with hasAny=true → comma prefix
    // 0x3 = bits 0,1 | 0xC000000000000000 = bits 62,63
    const uint64_t mask = 0x3ULL | 0xC000000000000000ULL;
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(mask), "0,1,62,63");
}

TEST(FormatTest, FormatCpuAffinityMaskTrailingLongRangeWithPrefix)
{
    // Bits {0,1} emit in-loop ("0,1"), then bits {61,62,63} are post-loop trailing
    // range (rangeEnd - rangeStart > 1) with hasAny=true → comma + "61-63"
    // 0x3 = bits 0,1 | 0xE000000000000000 = bits 61,62,63
    const uint64_t mask = 0x3ULL | 0xE000000000000000ULL;
    EXPECT_EQ(UI::Format::formatCpuAffinityMask(mask), "0,1,61-63");
}

// =============================================================================
// Logical processor summary (#1203)
// =============================================================================

TEST(FormatTest, LogicalProcessorSummaryCountsLogicalProcessorsNotCores)
{
    EXPECT_EQ(UI::Format::formatLogicalProcessorSummary(16, 3700.0), " (16 logical processors @ 3.70 GHz)");
    EXPECT_EQ(UI::Format::formatLogicalProcessorSummary(16, 0.0), " (16 logical processors)");
    EXPECT_EQ(UI::Format::formatLogicalProcessorSummary(1, 0.0), " (1 logical processor)");
    EXPECT_FALSE(UI::Format::formatLogicalProcessorSummary(8, 2400.0).contains("cores"));
}

// =============================================================================
// One grammar for values and axes (#1202)
// =============================================================================

TEST(FormatTest, FormatPowerCompactUsesOneDecimal)
{
    EXPECT_EQ(UI::Format::formatPowerCompact(15.5), "15.5 W");
    EXPECT_EQ(UI::Format::formatPowerCompact(45.0), "45.0 W");
    EXPECT_EQ(UI::Format::formatPowerCompact(0.015), "15.0 mW");
}

TEST(FormatTest, FormatWattsScalesUnitsAndKeepsSign)
{
    EXPECT_EQ(UI::Format::formatWatts(0.0), "0.0 W");
    EXPECT_EQ(UI::Format::formatWatts(-0.0), "0.0 W");
    EXPECT_EQ(UI::Format::formatWatts(45.0), "45.0 W");
    EXPECT_EQ(UI::Format::formatWatts(0.5), "500.0 mW");
    EXPECT_EQ(UI::Format::formatWatts(-12.25), "-12.3 W"); // An exact half rounds away from zero, like the table
}

// The Processes table's Power cell (splitPowerForAlignment) and the value/axis formatter agree.
TEST(FormatTest, FormatWattsMatchesThePowerColumn)
{
    // 12.25 and 0.0125 are exact halves: std::format alone would round them to even.
    for (const double watts : {0.0005, 0.0125, 0.5, 1.0, 4.96875, 12.25, 45.0, 123.45})
    {
        const auto parts = UI::Format::splitPowerForAlignment(watts);
        EXPECT_EQ(UI::Format::formatWatts(watts), parts.wholePart + parts.decimalPart + parts.unitPart) << watts;
    }
}

// The table's byte cells (splitBytesForAlignment) and formatBytes/the byte axes agree.
TEST(FormatTest, FormatBytesMatchesTheByteColumns)
{
    // 3.25 MB is an exact half: std::format alone printed "3.2 MiB" beside the table's "3.3 MiB".
    for (const double bytes : {512.0, 1536.0, 3.25 * 1024.0 * 1024.0, 1.5 * 1024.0 * 1024.0 * 1024.0})
    {
        const auto unit = UI::Format::chooseByteUnit(bytes);
        const auto parts = UI::Format::splitBytesForAlignment(bytes, unit);
        EXPECT_EQ(UI::Format::formatBytes(bytes), parts.wholePart + parts.decimalPart + parts.unitPart) << bytes;
        const auto rate = UI::Format::splitBytesPerSecForAlignment(bytes, unit);
        EXPECT_EQ(UI::Format::formatBytesPerSec(bytes), rate.wholePart + rate.decimalPart + rate.unitPart) << bytes;
    }
}

TEST(FormatTest, ByteUnitForReturnsTheNamedUnits)
{
    EXPECT_EQ(&UI::Format::byteUnitFor(10.0), &UI::Format::BYTE_UNIT_B);
    EXPECT_EQ(&UI::Format::byteUnitFor(2048.0), &UI::Format::BYTE_UNIT_KB);
    EXPECT_EQ(&UI::Format::byteUnitFor(3.0 * 1024.0 * 1024.0), &UI::Format::BYTE_UNIT_MB);
    EXPECT_EQ(&UI::Format::byteUnitFor(5.0 * 1024.0 * 1024.0 * 1024.0), &UI::Format::BYTE_UNIT_GB);
    EXPECT_EQ(UI::Format::chooseByteUnit(2048.0).suffix, "KiB");
}

TEST(FormatTest, FormatPercentIsWholeFromTenAndOneDecimalBelow)
{
    EXPECT_EQ(UI::Format::formatPercent(0.0), "0%");
    EXPECT_EQ(UI::Format::formatPercent(0.2), "0.2%");
    EXPECT_EQ(UI::Format::formatPercent(4.25), "4.3%"); // Exact half: away from zero, like percentOneDecimal()
    EXPECT_EQ(UI::Format::formatPercent(9.96), "10%");
    EXPECT_EQ(UI::Format::formatPercent(42.3), "42%");
    EXPECT_EQ(UI::Format::formatPercent(100.0), "100%");
    EXPECT_EQ(UI::Format::formatPercent(std::numeric_limits<double>::quiet_NaN()), "N/A");
}

// #1202: anything under 0.05 % rounds to zero and reads "0%", as on a chart axis -- never "0.0%" or
// "-0.0%".
TEST(FormatTest, FormatPercentPrintsWhatRoundsToZeroAsZero)
{
    for (const double percent : {0.04, 0.0499, 1e-9, -0.0, -1e-9, -0.01, -0.04})
    {
        EXPECT_EQ(UI::Format::formatPercent(percent), "0%") << percent;
    }
    EXPECT_EQ(UI::Format::formatPercent(0.05), "0.1%");
    EXPECT_EQ(UI::Format::formatPercent(-0.05), "-0.1%");
}

// Whole percents read the same as percentCompact(), the existing whole-percent value formatter.
TEST(FormatTest, FormatPercentMatchesPercentCompactFromTen)
{
    for (const double percent : {10.0, 12.0, 42.0, 99.0, 100.0})
    {
        EXPECT_EQ(UI::Format::formatPercent(percent), UI::Format::percentCompact(percent)) << percent;
    }
}

// =============================================================================
// formatFixedLocalizedTo (#1334): the allocation-free formatter behind every chart axis tick must
// print exactly what std::format("{:.{}Lf}") prints, in every locale.
// =============================================================================

namespace
{

/// Numeric punctuation with a chosen decimal point, separator and grouping, without depending on an
/// OS locale name.
class TestNumpunct : public std::numpunct<char>
{
  public:
    TestNumpunct(char decimalPoint, char thousandsSep, std::string grouping)
        : m_DecimalPoint(decimalPoint), m_ThousandsSep(thousandsSep), m_Grouping(std::move(grouping))
    {}

  protected:
    [[nodiscard]] char do_decimal_point() const override
    {
        return m_DecimalPoint;
    }
    [[nodiscard]] char do_thousands_sep() const override
    {
        return m_ThousandsSep;
    }
    [[nodiscard]] std::string do_grouping() const override
    {
        return m_Grouping;
    }

  private:
    char m_DecimalPoint;
    char m_ThousandsSep;
    std::string m_Grouping;
};

/// Makes a locale with TestNumpunct global for one scope and restores the previous one after it.
class ScopedTestNumpunct
{
  public:
    // std::locale takes ownership of the facet and deletes it with its last copy, which the analyzer
    // does not see.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks,cppcoreguidelines-owning-memory)
    ScopedTestNumpunct(char decimalPoint, char thousandsSep, std::string grouping)
        : m_Previous(
              std::locale::global(std::locale(std::locale::classic(), new TestNumpunct(decimalPoint, thousandsSep, std::move(grouping)))))
    {}
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks,cppcoreguidelines-owning-memory)
    ~ScopedTestNumpunct()
    {
        std::locale::global(m_Previous);
    }
    ScopedTestNumpunct(const ScopedTestNumpunct&) = delete;
    ScopedTestNumpunct& operator=(const ScopedTestNumpunct&) = delete;
    ScopedTestNumpunct(ScopedTestNumpunct&&) = delete;
    ScopedTestNumpunct& operator=(ScopedTestNumpunct&&) = delete;

  private:
    std::locale m_Previous;
};

[[nodiscard]] std::string fastFixed(double value, int decimals)
{
    std::array<char, 512> buffer{}; // Room for 1e300 with its separators
    const std::size_t length = UI::Format::formatFixedLocalizedTo(buffer.data(), buffer.size(), value, decimals);
    return {buffer.data(), length};
}

/// Values covering the sign, -0.0, exact binary halves (which std::format rounds to even), group
/// boundaries and very large magnitudes, plus random ones across the axis ranges.
[[nodiscard]] std::vector<double> fixedFormatSamples()
{
    std::vector<double> values{0.0,
                               -0.0,
                               0.04,
                               0.05,
                               0.25,
                               1.25,
                               2.5,
                               -2.5,
                               3.25,
                               -0.04,
                               9.95,
                               99.95,
                               999.95,
                               1000.0,
                               1023.95,
                               12345.678,
                               -12345.678,
                               123456.0,
                               1234567.891,
                               -987654321.5,
                               1.0e15,
                               -1.0e15,
                               1.0e20,
                               123456789012345678.0,
                               1.0e300,
                               5e-324,
                               0.1,
                               0.15,
                               0.35,
                               1e9 / 3.0,
                               4398046511104.0};
    std::mt19937 rng(1334); // NOLINT(bugprone-random-generator-seed) -- reproducible samples
    std::uniform_real_distribution<double> dist(-5.0e9, 5.0e9);
    for (int i = 0; i < 500; ++i)
    {
        values.push_back(dist(rng));
        values.push_back(dist(rng) / 1024.0 / 1024.0);
    }
    return values;
}

void expectMatchesStdFormat(const char* localeName)
{
    for (const double value : fixedFormatSamples())
    {
        for (int decimals = 0; decimals <= 3; ++decimals)
        {
            EXPECT_EQ(fastFixed(value, decimals), std::format("{:.{}Lf}", value, decimals))
                << localeName << ": value " << value << ", decimals " << decimals;
        }
    }
}

} // namespace

TEST(FormatFixedLocalizedTest, MatchesStdFormatInTheClassicLocale)
{
    const ScopedTestNumpunct classic('.', ',', "");
    expectMatchesStdFormat("classic");
}

TEST(FormatFixedLocalizedTest, MatchesStdFormatWithThousandsGrouping)
{
    const ScopedTestNumpunct enUs('.', ',', "\3");
    expectMatchesStdFormat("en_US-like");
}

TEST(FormatFixedLocalizedTest, MatchesStdFormatWithACommaDecimalAndDotGroups)
{
    const ScopedTestNumpunct deDe(',', '.', "\3");
    expectMatchesStdFormat("de_DE-like");
}

TEST(FormatFixedLocalizedTest, MatchesStdFormatWithIndianGrouping)
{
    // First group of 3, then groups of 2: 12,34,56,789.
    const ScopedTestNumpunct enIn('.', ',', "\3\2");
    expectMatchesStdFormat("en_IN-like");
    EXPECT_EQ(fastFixed(123456789.0, 1), "12,34,56,789.0");
}

TEST(FormatFixedLocalizedTest, GroupingEndsAtCharMax)
{
    // CHAR_MAX ends grouping: one separator, then every remaining digit in one group.
    const ScopedTestNumpunct limited('.', ' ', std::string{'\3', std::numeric_limits<char>::max()});
    EXPECT_EQ(fastFixed(123456789.0, 0), "123456 789");
    EXPECT_EQ(fastFixed(123456789.0, 0), std::format("{:.0Lf}", 123456789.0));
}

TEST(FormatFixedLocalizedTest, FollowsAGlobalLocaleChange)
{
    // The punctuation is cached; replacing the global locale must be noticed on the next call.
    {
        const ScopedTestNumpunct comma(',', '.', "\3");
        EXPECT_EQ(fastFixed(1234.5, 1), "1.234,5");
    }
    {
        const ScopedTestNumpunct dot('.', ',', "\3");
        EXPECT_EQ(fastFixed(1234.5, 1), "1,234.5");
    }
    const ScopedTestNumpunct classic('.', ',', "");
    EXPECT_EQ(fastFixed(1234.5, 1), "1234.5");
}

TEST(FormatFixedLocalizedTest, ReturnsZeroWhenItCannotFormatSoCallersFallBack)
{
    const ScopedTestNumpunct classic('.', ',', "");
    std::array<char, 8> small{};
    EXPECT_EQ(UI::Format::formatFixedLocalizedTo(small.data(), small.size(), 123456789.0, 1), 0U) << "does not fit";
    EXPECT_EQ(UI::Format::formatFixedLocalizedTo(small.data(), small.size(), 1234567.0, 0), 7U) << "fits exactly";
    EXPECT_EQ(UI::Format::formatFixedLocalizedTo(small.data(), small.size(), std::numeric_limits<double>::quiet_NaN(), 1), 0U);
    EXPECT_EQ(UI::Format::formatFixedLocalizedTo(small.data(), small.size(), std::numeric_limits<double>::infinity(), 1), 0U);
    EXPECT_EQ(UI::Format::formatFixedLocalizedTo(small.data(), small.size(), 1.0, -1), 0U);
    EXPECT_EQ(UI::Format::formatFixedLocalizedTo(small.data(), small.size(), 1.0, 10), 0U);
}

TEST(FormatFixedLocalizedTest, ValueFormattersStillPrintNonFiniteValuesLikeStdFormat)
{
    // Non-finite values take the std::format fallback, so their text is unchanged.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    // The unit text comes from the ByteUnit itself (IEC names since #1341), not a literal here.
    using UI::Format::BYTE_UNIT_GB;
    using UI::Format::BYTE_UNIT_KB;
    using UI::Format::BYTE_UNIT_MB;
    EXPECT_EQ(UI::Format::formatBytesWithUnit(inf, BYTE_UNIT_MB), std::format("{:.1Lf} {}", inf, BYTE_UNIT_MB.suffix));
    EXPECT_EQ(UI::Format::formatBytesPerSecWithUnit(-inf, BYTE_UNIT_KB), std::format("{:.1Lf} {}/s", -inf, BYTE_UNIT_KB.suffix));
    EXPECT_EQ(UI::Format::formatWatts(inf), std::format("{:.1Lf} W", inf));
    EXPECT_EQ(UI::Format::formatBytesWithUnit(nan, BYTE_UNIT_GB), std::format("{:.1Lf} {}", nan, BYTE_UNIT_GB.suffix));
}

TEST(FormatFixedLocalizedTest, ByteFormattersMatchTheirStdFormatDefinition)
{
    const ScopedTestNumpunct enUs('.', ',', "\3");
    for (const double bytes : fixedFormatSamples())
    {
        for (const auto* unit : {&UI::Format::BYTE_UNIT_B, &UI::Format::BYTE_UNIT_KB, &UI::Format::BYTE_UNIT_MB, &UI::Format::BYTE_UNIT_GB})
        {
            const double rounded = UI::Format::roundHalfAwayFromZero(bytes / unit->scale, unit->decimals);
            const std::string expected = std::format("{:.{}Lf} {}", rounded, unit->decimals, unit->suffix);
            EXPECT_EQ(UI::Format::formatBytesWithUnit(bytes, *unit), expected) << bytes;
            EXPECT_EQ(UI::Format::formatBytesPerSecWithUnit(bytes, *unit), expected + "/s") << bytes;
        }
    }
}

// #1366: splitBytesForAlignmentFast() took its thousands separator from a per-thread cache that was
// filled on first use and never refreshed, so a thread that formatted under the "C" locale kept
// printing "8658." after a grouping locale was made global, while the slow path printed "8,658.".
TEST(FormatFixedLocalizedTest, FastByteAlignmentFollowsAGlobalLocaleChange)
{
    const UI::Format::ByteUnit bytesUnit{.suffix = "B", .scale = 1.0, .decimals = 1};
    constexpr double BYTES = 8658.36;
    {
        // Format under "C" first on this thread, as an earlier test or early startup code would.
        const ScopedTestNumpunct classic('.', ',', "");
        EXPECT_EQ(UI::Format::splitBytesForAlignmentFast(BYTES, bytesUnit).wholePart(), "8658.");
        EXPECT_TRUE(compareBytesAlignment(BYTES, bytesUnit));
    }
    {
        const ScopedTestNumpunct enUs('.', ',', "\3");
        EXPECT_EQ(UI::Format::splitBytesForAlignmentFast(BYTES, bytesUnit).wholePart(), "8,658.");
        EXPECT_TRUE(compareBytesAlignment(BYTES, bytesUnit));
        EXPECT_TRUE(compareBytesAlignment(1234567.0, bytesUnit));
    }
    {
        const ScopedTestNumpunct deDe(',', '.', "\3");
        EXPECT_EQ(UI::Format::splitBytesForAlignmentFast(BYTES, bytesUnit).wholePart(), "8.658,");
        EXPECT_TRUE(compareBytesAlignment(BYTES, bytesUnit));
    }
    // And back to no grouping: the separator goes away again.
    const ScopedTestNumpunct classic('.', ',', "");
    EXPECT_EQ(UI::Format::splitBytesForAlignmentFast(BYTES, bytesUnit).wholePart(), "8658.");
    EXPECT_TRUE(compareBytesAlignment(BYTES, bytesUnit));
}
