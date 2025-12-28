/// @file test_Format.cpp
/// @brief Tests for UI::Format functions
///
/// Tests cover:
/// - CPU affinity mask formatting

#include "UI/Format.h"

#include <gtest/gtest.h>

#include <string>

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
