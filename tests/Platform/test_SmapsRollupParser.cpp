/// @file test_SmapsRollupParser.cpp
/// @brief Platform::SmapsRollup (#1885): /proc/[pid]/smaps_rollup as a process's PSS, USS and swap PSS.
/// Pure text, so built and run on every platform.

#include "Platform/IProcessProbe.h"
#include "Platform/Linux/SmapsRollupParser.h"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace Platform::SmapsRollup
{
namespace
{

// As Linux 6.x writes it.
constexpr std::string_view ROLLUP = "5581c7a3e000-7ffd8c1f5000 ---p 00000000 00:00 0                          [rollup]\n"
                                    "Rss:               12288 kB\n"
                                    "Pss:                6789 kB\n"
                                    "Pss_Dirty:          4000 kB\n"
                                    "Pss_Anon:           3500 kB\n"
                                    "Pss_File:           3189 kB\n"
                                    "Pss_Shmem:           100 kB\n"
                                    "Shared_Clean:       6000 kB\n"
                                    "Shared_Dirty:          0 kB\n"
                                    "Private_Clean:       100 kB\n"
                                    "Private_Dirty:      4200 kB\n"
                                    "Referenced:        12288 kB\n"
                                    "Anonymous:          4200 kB\n"
                                    "Swap:                 64 kB\n"
                                    "SwapPss:              50 kB\n"
                                    "Locked:                0 kB\n";

TEST(SmapsRollupParserTest, PssUssAndSwapPssAreReadInBytes)
{
    const std::optional<MemoryRollup> rollup = parse(ROLLUP);
    ASSERT_TRUE(rollup.has_value());
    const MemoryRollup value = rollup.value_or(MemoryRollup{});
    EXPECT_EQ(value.pssBytes, 6789ULL * 1024);
    EXPECT_EQ(value.ussBytes, (100ULL + 4200ULL) * 1024); // Private_Clean + Private_Dirty
    EXPECT_EQ(value.swapPssBytes, 50ULL * 1024);
}

TEST(SmapsRollupParserTest, AKernelWithoutSwapPssReadsZeroSwapPss)
{
    // SwapPss arrived in Linux 4.3; smaps_rollup itself in 4.14, so this is only a defensive read.
    const std::optional<MemoryRollup> rollup = parse("Pss: 10 kB\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB\n");
    ASSERT_TRUE(rollup.has_value());
    EXPECT_EQ(rollup.value_or(MemoryRollup{}).swapPssBytes, 0U);
    EXPECT_EQ(rollup.value_or(MemoryRollup{}).ussBytes, 3U * 1024);
}

TEST(SmapsRollupParserTest, AMissingOrUnreadableFieldIsNoRollup)
{
    EXPECT_FALSE(parse("").has_value());                                                           // an empty read (the process exited)
    EXPECT_FALSE(parse("Private_Clean: 1 kB\nPrivate_Dirty: 2 kB\n").has_value());                 // no Pss
    EXPECT_FALSE(parse("Pss: 10 kB\nPrivate_Dirty: 2 kB\n").has_value());                          // no Private_Clean
    EXPECT_FALSE(parse("Pss: 10 kB\nPrivate_Clean: 1 kB\n").has_value());                          // no Private_Dirty
    EXPECT_FALSE(parse("Pss: ten kB\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB\n").has_value());    // not a number
    EXPECT_FALSE(parse("Pss: 10\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB\n").has_value());        // no unit
    EXPECT_FALSE(parse("Pss: 10 MB\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB\n").has_value());     // another unit
    EXPECT_FALSE(parse("Pss: 10 kBytes\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB\n").has_value()); // more after kB
    EXPECT_FALSE(parse("Pss: 10 kB x\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB\n").has_value());
    // A SwapPss line that is there but unreadable makes the rollup unreadable, not a zero.
    EXPECT_FALSE(parse("Pss: 10 kB\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB\nSwapPss: lots kB\n").has_value());
    EXPECT_FALSE(parse("Pss: 10 kB\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB\nSwapPss: 18446744073709551615 kB\n").has_value());
}

TEST(SmapsRollupParserTest, AValueThatWouldOverflowIsNoRollup)
{
    EXPECT_FALSE(parse("Pss: 18446744073709551615 kB\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB\n").has_value());
    EXPECT_FALSE(parse("Pss: 1 kB\nPrivate_Clean: 18014398509481983 kB\nPrivate_Dirty: 18014398509481983 kB\n").has_value());
}

TEST(SmapsRollupParserTest, ATruncatedLastLineStillReadsWhatCameBefore)
{
    // The last line without its newline (a read that stopped there) still counts.
    const std::optional<MemoryRollup> rollup = parse("Pss: 10 kB\nPrivate_Clean: 1 kB\nPrivate_Dirty: 2 kB");
    ASSERT_TRUE(rollup.has_value());
    EXPECT_EQ(rollup.value_or(MemoryRollup{}).pssBytes, 10U * 1024);
}

} // namespace
} // namespace Platform::SmapsRollup
