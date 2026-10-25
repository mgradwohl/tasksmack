/// @file test_SettingsLayerDetail.cpp
/// @brief Unit tests for SettingsLayer helper functions.
/// @see Issue #310

#include "App/SettingsLayerDetail.h"
#include "Domain/SamplingConfig.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>

#include <optional>

namespace App::Detail
{
namespace
{

// ========================================
// Option index lookup (optionIndexOf, which replaced the find*Index helpers in #1221)
// ========================================

TEST(SettingsLayerDetailTest, OptionIndexOfMapsEveryFontSizeToItsOption)
{
    EXPECT_EQ(optionIndexOf(FONT_SIZE_OPTIONS, UI::FontSize::Small, &FontSizeOption::value), 0U);
    EXPECT_EQ(optionIndexOf(FONT_SIZE_OPTIONS, UI::FontSize::Medium, &FontSizeOption::value), 1U);
    EXPECT_EQ(optionIndexOf(FONT_SIZE_OPTIONS, UI::FontSize::Large, &FontSizeOption::value), 2U);
    EXPECT_EQ(optionIndexOf(FONT_SIZE_OPTIONS, UI::FontSize::ExtraLarge, &FontSizeOption::value), 3U);
    EXPECT_EQ(optionIndexOf(FONT_SIZE_OPTIONS, UI::FontSize::Huge, &FontSizeOption::value), 4U);
    EXPECT_EQ(optionIndexOf(FONT_SIZE_OPTIONS, UI::FontSize::EvenHuger, &FontSizeOption::value), 5U);
    EXPECT_FALSE(optionIndexOf(FONT_SIZE_OPTIONS, static_cast<UI::FontSize>(999), &FontSizeOption::value).has_value());
}

TEST(SettingsLayerDetailTest, OptionIndexOfMapsEveryRefreshRateToItsOption)
{
    EXPECT_EQ(optionIndexOf(REFRESH_RATE_OPTIONS, 100, &RefreshRateOption::valueMs), 0U);
    EXPECT_EQ(optionIndexOf(REFRESH_RATE_OPTIONS, 250, &RefreshRateOption::valueMs), 1U);
    EXPECT_EQ(optionIndexOf(REFRESH_RATE_OPTIONS, 500, &RefreshRateOption::valueMs), 2U);
    EXPECT_EQ(optionIndexOf(REFRESH_RATE_OPTIONS, 1000, &RefreshRateOption::valueMs), 3U);
    EXPECT_EQ(optionIndexOf(REFRESH_RATE_OPTIONS, 2000, &RefreshRateOption::valueMs), 4U);
    EXPECT_EQ(optionIndexOf(REFRESH_RATE_OPTIONS, 5000, &RefreshRateOption::valueMs), 5U);
    EXPECT_FALSE(optionIndexOf(REFRESH_RATE_OPTIONS, 0, &RefreshRateOption::valueMs).has_value());
    EXPECT_FALSE(optionIndexOf(REFRESH_RATE_OPTIONS, -1, &RefreshRateOption::valueMs).has_value());
}

TEST(SettingsLayerDetailTest, OptionIndexOfMapsEveryHistoryDurationToItsOption)
{
    EXPECT_EQ(optionIndexOf(HISTORY_OPTIONS, 60, &HistoryOption::valueSeconds), 0U);
    EXPECT_EQ(optionIndexOf(HISTORY_OPTIONS, 120, &HistoryOption::valueSeconds), 1U);
    EXPECT_EQ(optionIndexOf(HISTORY_OPTIONS, 300, &HistoryOption::valueSeconds), 2U);
    EXPECT_EQ(optionIndexOf(HISTORY_OPTIONS, 600, &HistoryOption::valueSeconds), 3U);
    EXPECT_FALSE(optionIndexOf(HISTORY_OPTIONS, 0, &HistoryOption::valueSeconds).has_value());
    EXPECT_FALSE(optionIndexOf(HISTORY_OPTIONS, -1, &HistoryOption::valueSeconds).has_value());
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

// ========================================
// Reset to defaults (#1273)
// ========================================

TEST(SettingsLayerDetailTest, DefaultChoicePicksEachShippedDefault)
{
    // Picked, so Save writes the default even when the stored value was already something else.
    const ComboState font = defaultChoice(FONT_SIZE_OPTIONS, UI::FontSize::Medium, &FontSizeOption::value);
    EXPECT_TRUE(font.touched);
    ASSERT_TRUE(pickedOption(font, FONT_SIZE_OPTIONS).has_value());
    EXPECT_EQ(pickedOption(font, FONT_SIZE_OPTIONS).value().value, UI::FontSize::Medium);

    const ComboState refresh =
        defaultChoice(REFRESH_RATE_OPTIONS, Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS, &RefreshRateOption::valueMs);
    ASSERT_TRUE(pickedOption(refresh, REFRESH_RATE_OPTIONS).has_value());
    EXPECT_EQ(pickedOption(refresh, REFRESH_RATE_OPTIONS).value().valueMs, Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS);

    const ComboState history = defaultChoice(HISTORY_OPTIONS, Domain::Sampling::HISTORY_SECONDS_DEFAULT, &HistoryOption::valueSeconds);
    ASSERT_TRUE(pickedOption(history, HISTORY_OPTIONS).has_value());
    EXPECT_EQ(pickedOption(history, HISTORY_OPTIONS).value().valueSeconds, Domain::Sampling::HISTORY_SECONDS_DEFAULT);
}

TEST(SettingsLayerDetailTest, DefaultChoiceForAnOffListValueWritesNothing)
{
    const ComboState choice = defaultChoice(REFRESH_RATE_OPTIONS, 750, &RefreshRateOption::valueMs);
    EXPECT_FALSE(choice.index.has_value());
    EXPECT_FALSE(choice.touched);
    EXPECT_FALSE(pickedOption(choice, REFRESH_RATE_OPTIONS).has_value());
}

TEST(SettingsLayerDetailTest, LargestFontPresetIsNamedLargest)
{
    EXPECT_EQ(FONT_SIZE_OPTIONS.back().label, "Largest");
    EXPECT_EQ(FONT_SIZE_OPTIONS.back().value, UI::FontSize::EvenHuger);
}

} // namespace
} // namespace App::Detail
