/// @file test_SettingsLayerDetail.cpp
/// @brief Unit tests for SettingsLayer helper functions.
/// @see Issue #310

#include "App/SettingsLayerDetail.h"

#include <gtest/gtest.h>

#include <optional>

namespace App::Detail
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
        EXPECT_FALSE(opt.label.empty()) << "Font size option has empty label";
    }
}

TEST(SettingsLayerDetailTest, AllRefreshRateOptionsHaveLabels)
{
    for (const auto& opt : REFRESH_RATE_OPTIONS)
    {
        EXPECT_FALSE(opt.label.empty()) << "Refresh rate option has empty label (valueMs=" << opt.valueMs << ")";
    }
}

TEST(SettingsLayerDetailTest, AllHistoryOptionsHaveLabels)
{
    for (const auto& opt : HISTORY_OPTIONS)
    {
        EXPECT_FALSE(opt.label.empty()) << "History option has empty label (valueSeconds=" << opt.valueSeconds << ")";
    }
}

TEST(SettingsLayerDetailTest, RefreshRateValuesArePositive)
{
    for (const auto& opt : REFRESH_RATE_OPTIONS)
    {
        EXPECT_GT(opt.valueMs, 0) << "Refresh rate option has non-positive valueMs: " << opt.valueMs << " (label=" << opt.label << ")";
    }
}

TEST(SettingsLayerDetailTest, HistoryValuesArePositive)
{
    for (const auto& opt : HISTORY_OPTIONS)
    {
        EXPECT_GT(opt.valueSeconds, 0) << "History option has non-positive valueSeconds: " << opt.valueSeconds << " (label=" << opt.label
                                       << ")";
    }
}

// ========================================
// Apply writes only picked combos (#1120, #1151)
// ========================================

TEST(SettingsLayerDetailTest, UntouchedComboWritesNothing)
{
    // A stored value that isn't an option (interval_ms = 750) has no index; untouched, Apply must
    // leave it alone instead of writing the old fallback option.
    const ComboState offList{.index = std::nullopt, .touched = false};
    EXPECT_FALSE(pickedOption(offList, REFRESH_RATE_OPTIONS).has_value());

    // Same for a matching value the user didn't change: nothing to write.
    const ComboState onList{.index = 3, .touched = false};
    EXPECT_FALSE(pickedOption(onList, REFRESH_RATE_OPTIONS).has_value());
}

TEST(SettingsLayerDetailTest, PickedComboWritesThePickedOption)
{
    const ComboState picked{.index = 1, .touched = true};
    const auto option = pickedOption(picked, HISTORY_OPTIONS);
    ASSERT_TRUE(option.has_value());
    EXPECT_EQ(option->valueSeconds, 120);
}

TEST(SettingsLayerDetailTest, FontSizeChangedWhileOpenIsNotRevertedByApply)
{
    // The dialog opened on Medium, then Ctrl+= made it Large; the combo was never touched, so
    // Apply has nothing to write and the shortcut's change stands (#1151).
    const ComboState fontChoice{.index = optionIndexOf(FONT_SIZE_OPTIONS, UI::FontSize::Medium, &FontSizeOption::value)};
    EXPECT_FALSE(pickedOption(fontChoice, FONT_SIZE_OPTIONS).has_value());
}

TEST(SettingsLayerDetailTest, OptionIndexOfFindsOnlyExactValues)
{
    EXPECT_EQ(optionIndexOf(REFRESH_RATE_OPTIONS, 250, &RefreshRateOption::valueMs), 1U);
    EXPECT_FALSE(optionIndexOf(REFRESH_RATE_OPTIONS, 750, &RefreshRateOption::valueMs).has_value());
    EXPECT_FALSE(optionIndexOf(HISTORY_OPTIONS, 1800, &HistoryOption::valueSeconds).has_value());
}

TEST(SettingsLayerDetailTest, CustomLabelsDescribeOffListValues)
{
    EXPECT_EQ(customRefreshLabel(750), "Custom (750 ms)");
    EXPECT_EQ(customHistoryLabel(1800), "Custom (30 minutes)");
    EXPECT_EQ(customHistoryLabel(60), "Custom (1 minute)");
    EXPECT_EQ(customHistoryLabel(45), "Custom (45 seconds)");
}

} // namespace
} // namespace App::Detail
