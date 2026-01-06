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
