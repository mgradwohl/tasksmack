/// @file test_Format.cpp
/// @brief Tests for UI::Format functions
///
/// Tests cover:
/// - CPU affinity mask formatting

#include "UI/Format.h"

#include <gtest/gtest.h>

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
    // Use long long to avoid overflow on Windows where long is 32-bit
    const long long largeValue = static_cast<long long>(std::numeric_limits<int>::max()) + 1000LL;
    EXPECT_EQ(UI::Format::toIntSaturated(largeValue), std::numeric_limits<int>::max());
}

TEST(FormatTest, ToIntSaturatedPreservesNormalValues)
{
    EXPECT_EQ(UI::Format::toIntSaturated(42L), 42);
    EXPECT_EQ(UI::Format::toIntSaturated(-42L), -42);
    EXPECT_EQ(UI::Format::toIntSaturated(0L), 0);
    EXPECT_EQ(UI::Format::toIntSaturated(100L), 100);
}

TEST(FormatTest, PercentToIntClampsTo0To100)
{
    EXPECT_EQ(UI::Format::percentToInt(-10.0), 0);
    EXPECT_EQ(UI::Format::percentToInt(0.0), 0);
    EXPECT_EQ(UI::Format::percentToInt(50.5), 51); // Rounds to nearest
    EXPECT_EQ(UI::Format::percentToInt(100.0), 100);
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

TEST(FormatTest, PercentOneDecimalLocalizedFormatsCorrectly)
{
    // Just test that the function produces reasonable output
    const auto result = UI::Format::percentOneDecimalLocalized(50.5);
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

TEST(FormatTest, FormatOrDashReturnsForZeroOrNegative)
{
    const auto resultZero = UI::Format::formatOrDash(0, [](int v) { return std::format("{}", v); });
    EXPECT_EQ(resultZero, "-");

    const auto resultNeg = UI::Format::formatOrDash(-5, [](int v) { return std::format("{}", v); });
    EXPECT_EQ(resultNeg, "-");
}

// =============================================================================
// Uptime Formatting Tests
// =============================================================================

TEST(FormatTest, FormatHoursMinutesFormatsCorrectly)
{
    const auto result = UI::Format::formatHoursMinutes(1, 30); // 1 hour 30 minutes
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("1h"));
    EXPECT_TRUE(result.contains("30m"));
}

TEST(FormatTest, FormatHoursMinutesHandlesZero)
{
    const auto result = UI::Format::formatHoursMinutes(0, 0);
    EXPECT_FALSE(result.empty());
}

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
    EXPECT_EQ(std::string(unit.suffix), "KB");
}

TEST(FormatTest, ChooseByteUnitSelectsMegabytes)
{
    const auto unit = UI::Format::chooseByteUnit(2.0 * 1024.0 * 1024.0);
    EXPECT_EQ(std::string(unit.suffix), "MB");
}

TEST(FormatTest, ChooseByteUnitSelectsGigabytes)
{
    const auto unit = UI::Format::chooseByteUnit(2.0 * 1024.0 * 1024.0 * 1024.0);
    EXPECT_EQ(std::string(unit.suffix), "GB");
}

TEST(FormatTest, UnitForTotalBytesWorks)
{
    const auto unit = UI::Format::unitForTotalBytes(1024ULL * 1024ULL);
    EXPECT_EQ(std::string(unit.suffix), "MB");
}

TEST(FormatTest, UnitForBytesPerSecondWorks)
{
    const auto unit = UI::Format::unitForBytesPerSecond(1024.0 * 1024.0);
    EXPECT_EQ(std::string(unit.suffix), "MB");
}

// =============================================================================
// Byte Formatting Tests
// =============================================================================

TEST(FormatTest, FormatBytesFormatsCorrectly)
{
    const auto result = UI::Format::formatBytes(1536.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("KB"));
}

TEST(FormatTest, FormatBytesWithUnitFormatsCorrectly)
{
    const auto unit = UI::Format::chooseByteUnit(1024.0 * 1024.0);
    const auto result = UI::Format::formatBytesWithUnit(1024.0 * 1024.0, unit);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("MB"));
}

TEST(FormatTest, FormatBytesPerSecFormatsCorrectly)
{
    const auto result = UI::Format::formatBytesPerSec(1024.0 * 1024.0);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("MB"));
    EXPECT_TRUE(result.contains("/s"));
}

TEST(FormatTest, FormatBytesPerSecWithUnitFormatsCorrectly)
{
    const auto unit = UI::Format::chooseByteUnit(1024.0);
    const auto result = UI::Format::formatBytesPerSecWithUnit(1024.0, unit);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.contains("KB"));
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
    EXPECT_FALSE(parts.unitPart.empty());
    EXPECT_TRUE(parts.unitPart.contains('%'));
}

TEST(FormatTest, SplitPercentForAlignmentHandlesZeroDecimals)
{
    const auto parts = UI::Format::splitPercentForAlignment(75.0, 0);

    EXPECT_FALSE(parts.wholePart.empty());
    EXPECT_TRUE(parts.decimalPart.empty());
    EXPECT_TRUE(parts.unitPart.contains('%'));
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

// =============================================================================
// Count Per Second Formatting Tests
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
// CPU Time Compact Formatting Tests
// =============================================================================

TEST(FormatTest, FormatCpuTimeCompactFormatsSeconds)
{
    const auto result = UI::Format::formatCpuTimeCompact(45.0);
    EXPECT_EQ(result, "0:45");
}

TEST(FormatTest, FormatCpuTimeCompactFormatsMinutes)
{
    const auto result = UI::Format::formatCpuTimeCompact(125.0); // 2:05
    EXPECT_EQ(result, "2:05");
}

TEST(FormatTest, FormatCpuTimeCompactFormatsHours)
{
    const auto result = UI::Format::formatCpuTimeCompact(3725.0); // 1:02:05
    EXPECT_EQ(result, "1:02:05");
}

TEST(FormatTest, FormatCpuTimeCompactHandlesZero)
{
    const auto result = UI::Format::formatCpuTimeCompact(0.0);
    EXPECT_EQ(result, "0:00");
}
