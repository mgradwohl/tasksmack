/// @file test_SettingsLayerDetail.cpp
/// @brief Unit tests for SettingsLayer helper functions.
/// @see Issue #310

#include "App/SettingsLayerDetail.h"

#include <gtest/gtest.h>

#include <ranges>
#include <cstring>

namespace App::detail
{
namespace
{

// ========================================
// Font Size Index Tests
// ========================================

TEST(SettingsLayerDetailTest, FindFontSizeIndexReturnsCorrectIndices)
{
    EXPECT_EQ(findFontSizeIndex(UI::FontSize::Small), 0U);
    EXPECT_EQ(findFontSizeIndex(UI::FontSize::Medium), 1U);
    EXPECT_EQ(findFontSizeIndex(UI::FontSize::Large), 2U);
    EXPECT_EQ(findFontSizeIndex(UI::FontSize::ExtraLarge), 3U);
    EXPECT_EQ(findFontSizeIndex(UI::FontSize::Huge), 4U);
    EXPECT_EQ(findFontSizeIndex(UI::FontSize::EvenHuger), 5U);
}

TEST(SettingsLayerDetailTest, FindFontSizeIndexReturnsDefaultForInvalid)
{
    // Cast an invalid value to FontSize
    const auto invalidSize = static_cast<UI::FontSize>(999);
    // Default should be 1 (Medium)
    EXPECT_EQ(findFontSizeIndex(invalidSize), 1U);
}

// ========================================
// Refresh Rate Index Tests
// ========================================

TEST(SettingsLayerDetailTest, FindRefreshRateIndexReturnsCorrectIndices)
{
    EXPECT_EQ(findRefreshRateIndex(100), 0U);
    EXPECT_EQ(findRefreshRateIndex(250), 1U);
    EXPECT_EQ(findRefreshRateIndex(500), 2U);
    EXPECT_EQ(findRefreshRateIndex(1000), 3U);
    EXPECT_EQ(findRefreshRateIndex(2000), 4U);
    EXPECT_EQ(findRefreshRateIndex(5000), 5U);
}

TEST(SettingsLayerDetailTest, FindRefreshRateIndexReturnsDefaultForInvalid)
{
    // Invalid values should return default index 3 (1 second)
    EXPECT_EQ(findRefreshRateIndex(0), 3U);
    EXPECT_EQ(findRefreshRateIndex(999), 3U);
    EXPECT_EQ(findRefreshRateIndex(-1), 3U);
    EXPECT_EQ(findRefreshRateIndex(10000), 3U);
}

// ========================================
// History Duration Index Tests
// ========================================

TEST(SettingsLayerDetailTest, FindHistoryIndexReturnsCorrectIndices)
{
    EXPECT_EQ(findHistoryIndex(60), 0U);  // 1 minute
    EXPECT_EQ(findHistoryIndex(120), 1U); // 2 minutes
    EXPECT_EQ(findHistoryIndex(300), 2U); // 5 minutes
    EXPECT_EQ(findHistoryIndex(600), 3U); // 10 minutes
}

TEST(SettingsLayerDetailTest, FindHistoryIndexReturnsDefaultForInvalid)
{
    // Invalid values should return default index 2 (5 minutes)
    EXPECT_EQ(findHistoryIndex(0), 2U);
    EXPECT_EQ(findHistoryIndex(30), 2U);
    EXPECT_EQ(findHistoryIndex(90), 2U);
    EXPECT_EQ(findHistoryIndex(1000), 2U);
    EXPECT_EQ(findHistoryIndex(-1), 2U);
}

// ========================================
// Option Array Consistency Tests
// ========================================

TEST(SettingsLayerDetailTest, FontSizeOptionsHaveExpectedCount)
{
    EXPECT_EQ(FONT_SIZE_OPTIONS.size(), 6U);
}

TEST(SettingsLayerDetailTest, RefreshRateOptionsHaveExpectedCount)
{
    EXPECT_EQ(REFRESH_RATE_OPTIONS.size(), 6U);
}

TEST(SettingsLayerDetailTest, HistoryOptionsHaveExpectedCount)
{
    EXPECT_EQ(HISTORY_OPTIONS.size(), 4U);
}

TEST(SettingsLayerDetailTest, AllFontSizeOptionsHaveLabels)
{
    for (const auto& opt : FONT_SIZE_OPTIONS)
    {
        EXPECT_NE(opt.label, nullptr) << "Font size option has null label";
        if (opt.label != nullptr)
        {
            EXPECT_GT(std::strlen(opt.label), 0U) << "Font size option has empty label";
        }
    }
}

TEST(SettingsLayerDetailTest, AllRefreshRateOptionsHaveLabels)
{
    for (const auto& opt : REFRESH_RATE_OPTIONS)
    {
        EXPECT_NE(opt.label, nullptr) << "Refresh rate option has null label (valueMs=" << opt.valueMs << ")";
        if (opt.label != nullptr)
        {
            EXPECT_GT(std::strlen(opt.label), 0U) << "Refresh rate option has empty label (valueMs=" << opt.valueMs << ")";
        }
    }
}

TEST(SettingsLayerDetailTest, AllHistoryOptionsHaveLabels)
{
    for (const auto& opt : HISTORY_OPTIONS)
    {
        EXPECT_NE(opt.label, nullptr) << "History option has null label (valueSeconds=" << opt.valueSeconds << ")";
        if (opt.label != nullptr)
        {
            EXPECT_GT(std::strlen(opt.label), 0U) << "History option has empty label (valueSeconds=" << opt.valueSeconds << ")";
        }
    }
}

TEST(SettingsLayerDetailTest, RefreshRateValuesArePositive)
{
    for (const auto& opt : REFRESH_RATE_OPTIONS)
    {
        EXPECT_GT(opt.valueMs, 0) << "Refresh rate option has non-positive valueMs: " << opt.valueMs
                                  << " (label=" << (opt.label != nullptr ? opt.label : "<null>") << ")";
    }
}

TEST(SettingsLayerDetailTest, HistoryValuesArePositive)
{
    for (const auto& opt : HISTORY_OPTIONS)
    {
        EXPECT_GT(opt.valueSeconds, 0) << "History option has non-positive valueSeconds: " << opt.valueSeconds
                                       << " (label=" << (opt.label != nullptr ? opt.label : "<null>") << ")";
    }
}

} // namespace
} // namespace App::detail
