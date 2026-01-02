// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "App/ProcessColumnConfig.h"
#include "App/UserConfig.h"
#include "Domain/SamplingConfig.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>

#include <string>

namespace App
{
namespace
{

// ========== UserSettings Default Values ==========

TEST(UserSettingsTest, DefaultThemeId)
{
    const UserSettings settings;
    EXPECT_EQ(settings.themeId, "arctic-fire");
}

TEST(UserSettingsTest, DefaultFontSize)
{
    const UserSettings settings;
    EXPECT_EQ(settings.fontSize, UI::FontSize::Medium);
}

TEST(UserSettingsTest, DefaultRefreshInterval)
{
    const UserSettings settings;
    EXPECT_EQ(settings.refreshIntervalMs, Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS);
}

TEST(UserSettingsTest, DefaultMaxHistorySeconds)
{
    const UserSettings settings;
    EXPECT_EQ(settings.maxHistorySeconds, Domain::Sampling::HISTORY_SECONDS_DEFAULT);
}

TEST(UserSettingsTest, DefaultWindowDimensions)
{
    const UserSettings settings;
    EXPECT_EQ(settings.windowWidth, 1280);
    EXPECT_EQ(settings.windowHeight, 720);
    EXPECT_FALSE(settings.windowPosX.has_value());
    EXPECT_FALSE(settings.windowPosY.has_value());
    EXPECT_FALSE(settings.windowMaximized);
}

TEST(UserSettingsTest, DefaultProcessColumnsAreVisible)
{
    const UserSettings settings;
    // Key columns should be visible by default
    EXPECT_TRUE(settings.processColumns.isVisible(ProcessColumn::PID));
    EXPECT_TRUE(settings.processColumns.isVisible(ProcessColumn::Name));
    EXPECT_TRUE(settings.processColumns.isVisible(ProcessColumn::CpuPercent));
    EXPECT_TRUE(settings.processColumns.isVisible(ProcessColumn::MemPercent));
}

// ========== Boundary Conditions ==========

TEST(UserSettingsTest, RefreshIntervalBoundaries)
{
    UserSettings settings;

    // Test min boundary
    settings.refreshIntervalMs = Domain::Sampling::REFRESH_INTERVAL_MIN_MS;
    EXPECT_EQ(settings.refreshIntervalMs, Domain::Sampling::REFRESH_INTERVAL_MIN_MS);

    // Test max boundary
    settings.refreshIntervalMs = Domain::Sampling::REFRESH_INTERVAL_MAX_MS;
    EXPECT_EQ(settings.refreshIntervalMs, Domain::Sampling::REFRESH_INTERVAL_MAX_MS);
}

TEST(UserSettingsTest, HistorySecondsBoundaries)
{
    UserSettings settings;

    // Test min boundary
    settings.maxHistorySeconds = Domain::Sampling::HISTORY_SECONDS_MIN;
    EXPECT_EQ(settings.maxHistorySeconds, Domain::Sampling::HISTORY_SECONDS_MIN);

    // Test max boundary
    settings.maxHistorySeconds = Domain::Sampling::HISTORY_SECONDS_MAX;
    EXPECT_EQ(settings.maxHistorySeconds, Domain::Sampling::HISTORY_SECONDS_MAX);
}

TEST(UserSettingsTest, WindowDimensionsBoundaries)
{
    UserSettings settings;

    // Reasonable window sizes
    settings.windowWidth = 200;
    settings.windowHeight = 200;
    EXPECT_EQ(settings.windowWidth, 200);
    EXPECT_EQ(settings.windowHeight, 200);

    // Large window sizes
    settings.windowWidth = 16384;
    settings.windowHeight = 16384;
    EXPECT_EQ(settings.windowWidth, 16384);
    EXPECT_EQ(settings.windowHeight, 16384);
}

// ========== Font Size Enum Values ==========

TEST(UserSettingsTest, AllFontSizesAreValid)
{
    UserSettings settings;

    settings.fontSize = UI::FontSize::Small;
    EXPECT_EQ(settings.fontSize, UI::FontSize::Small);

    settings.fontSize = UI::FontSize::Medium;
    EXPECT_EQ(settings.fontSize, UI::FontSize::Medium);

    settings.fontSize = UI::FontSize::Large;
    EXPECT_EQ(settings.fontSize, UI::FontSize::Large);

    settings.fontSize = UI::FontSize::ExtraLarge;
    EXPECT_EQ(settings.fontSize, UI::FontSize::ExtraLarge);

    settings.fontSize = UI::FontSize::Huge;
    EXPECT_EQ(settings.fontSize, UI::FontSize::Huge);

    settings.fontSize = UI::FontSize::EvenHuger;
    EXPECT_EQ(settings.fontSize, UI::FontSize::EvenHuger);
}

// ========== Window Position Optional Handling ==========

TEST(UserSettingsTest, WindowPositionCanBeSet)
{
    UserSettings settings;

    settings.windowPosX = 100;
    settings.windowPosY = 200;

    EXPECT_TRUE(settings.windowPosX.has_value());
    EXPECT_TRUE(settings.windowPosY.has_value());
    EXPECT_EQ(*settings.windowPosX, 100);
    EXPECT_EQ(*settings.windowPosY, 200);
}

TEST(UserSettingsTest, WindowPositionCanBeReset)
{
    UserSettings settings;

    settings.windowPosX = 100;
    settings.windowPosY = 200;
    settings.windowPosX.reset();
    settings.windowPosY.reset();

    EXPECT_FALSE(settings.windowPosX.has_value());
    EXPECT_FALSE(settings.windowPosY.has_value());
}

TEST(UserSettingsTest, WindowPositionHandlesNegativeValues)
{
    UserSettings settings;

    // Negative positions are valid (multi-monitor setups)
    settings.windowPosX = -500;
    settings.windowPosY = -300;

    EXPECT_EQ(*settings.windowPosX, -500);
    EXPECT_EQ(*settings.windowPosY, -300);
}

// ========== Process Column Settings Integration ==========

TEST(UserSettingsTest, ProcessColumnsCanBeModified)
{
    UserSettings settings;

    // Hide a column
    settings.processColumns.setVisible(ProcessColumn::PID, false);
    EXPECT_FALSE(settings.processColumns.isVisible(ProcessColumn::PID));

    // Show it again
    settings.processColumns.setVisible(ProcessColumn::PID, true);
    EXPECT_TRUE(settings.processColumns.isVisible(ProcessColumn::PID));
}

TEST(UserSettingsTest, ProcessColumnsToggleWorks)
{
    UserSettings settings;

    const bool initial = settings.processColumns.isVisible(ProcessColumn::Name);
    settings.processColumns.toggleVisible(ProcessColumn::Name);
    EXPECT_EQ(settings.processColumns.isVisible(ProcessColumn::Name), !initial);
}

// ========== Multiple Settings Interactions ==========

TEST(UserSettingsTest, CopySemantics)
{
    UserSettings original;
    original.themeId = "custom-theme";
    original.refreshIntervalMs = 2000;
    original.windowPosX = 500;

    // Copy
    const UserSettings copy = original;

    EXPECT_EQ(copy.themeId, "custom-theme");
    EXPECT_EQ(copy.refreshIntervalMs, 2000);
    EXPECT_TRUE(copy.windowPosX.has_value());
    EXPECT_EQ(*copy.windowPosX, 500);

    // Modifying original shouldn't affect copy (no aliasing)
    original.themeId = "modified";
    EXPECT_EQ(copy.themeId, "custom-theme");
}

TEST(UserSettingsTest, MoveSemantics)
{
    UserSettings original;
    original.themeId = "move-theme";
    original.windowPosX = 123;

    // Move
    UserSettings moved = std::move(original);

    // Verify moved object has expected values
    EXPECT_EQ(moved.themeId, "move-theme");
    EXPECT_TRUE(moved.windowPosX.has_value());
    EXPECT_EQ(*moved.windowPosX, 123);

    // Note: We don't verify the moved-from state of 'original' because the C++ standard
    // only guarantees that moved-from objects are in a valid but unspecified state.
}

TEST(UserSettingsTest, SettingsModificationIsIndependent)
{
    UserSettings settings1;
    UserSettings settings2;

    settings1.refreshIntervalMs = 1000;
    settings2.refreshIntervalMs = 5000;

    EXPECT_EQ(settings1.refreshIntervalMs, 1000);
    EXPECT_EQ(settings2.refreshIntervalMs, 5000);
}

// ========== Edge Cases ==========

TEST(UserSettingsTest, EmptyThemeIdIsAllowed)
{
    UserSettings settings;
    settings.themeId = "";
    EXPECT_TRUE(settings.themeId.empty());
}

TEST(UserSettingsTest, LongThemeIdIsAllowed)
{
    UserSettings settings;
    const std::string longTheme(1000, 'x');
    settings.themeId = longTheme;
    EXPECT_EQ(settings.themeId.size(), 1000);
}

TEST(UserSettingsTest, ZeroWindowDimensionsAreStorable)
{
    UserSettings settings;
    settings.windowWidth = 0;
    settings.windowHeight = 0;
    EXPECT_EQ(settings.windowWidth, 0);
    EXPECT_EQ(settings.windowHeight, 0);
}

} // namespace
} // namespace App
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
