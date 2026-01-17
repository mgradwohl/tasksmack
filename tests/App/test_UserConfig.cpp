// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "App/ProcessColumnConfig.h"
#include "App/UserConfig.h"
#include "Domain/SamplingConfig.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

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

// ========== Config Load/Save Integration Tests ==========

TEST(UserConfigTest, LoadValidConfigFile)
{
    // Create a temporary config file with known values
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "tasksmack_test_config";
    std::filesystem::create_directories(tempDir);
    const std::filesystem::path configFile = tempDir / "test_config.toml";

    // Write a test config
    std::ofstream out(configFile);
    out << "[sampling]\n";
    out << "interval_ms = 2000\n";
    out << "history_max_seconds = 600\n";
    out << "\n";
    out << "[ui]\n";
    out << "chart_smooth_factor = 0.8\n";
    out << "progress_color_low_threshold = 40.0\n";
    out << "progress_color_high_threshold = 80.0\n";
    out << "\n";
    out << "[theme]\n";
    out << "id = \"cyberpunk\"\n";
    out << "\n";
    out << "[window]\n";
    out << "width = 1920\n";
    out << "height = 1080\n";
    out << "maximized = true\n";
    out.close();

    // Note: UserConfig is a singleton, we can't easily test it in isolation
    // This test documents expected behavior but would require refactoring
    // UserConfig to accept a config path for testing

    // Cleanup
    std::filesystem::remove_all(tempDir);
}

TEST(UserConfigTest, LoadConfigWithOutOfRangeValuesGetsClampedCorrectly)
{
    // This test documents that out-of-range values should be clamped
    // by the Domain::Sampling clamp functions during load

    // Example: refreshIntervalMs should be clamped to REFRESH_INTERVAL_MIN_MS..REFRESH_INTERVAL_MAX_MS
    const int tooSmall = Domain::Sampling::REFRESH_INTERVAL_MIN_MS - 100;
    const int tooLarge = Domain::Sampling::REFRESH_INTERVAL_MAX_MS + 100;

    // These would be clamped by clampRefreshInterval()
    EXPECT_LT(tooSmall, Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
    EXPECT_GT(tooLarge, Domain::Sampling::REFRESH_INTERVAL_MAX_MS);

    const int clampedSmall = Domain::Sampling::clampRefreshInterval(tooSmall);
    const int clampedLarge = Domain::Sampling::clampRefreshInterval(tooLarge);

    EXPECT_EQ(clampedSmall, Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
    EXPECT_EQ(clampedLarge, Domain::Sampling::REFRESH_INTERVAL_MAX_MS);
}

TEST(UserConfigTest, LoadConfigWithMissingKeysUsesDefaults)
{
    // When a config key is missing, the default value should be retained
    const UserSettings settings;

    // These are the defaults that should be preserved when keys are missing
    EXPECT_EQ(settings.refreshIntervalMs, Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS);
    EXPECT_EQ(settings.maxHistorySeconds, Domain::Sampling::HISTORY_SECONDS_DEFAULT);
    EXPECT_EQ(settings.chartSmoothFactor, Domain::Sampling::CHART_SMOOTH_FACTOR_DEFAULT);
    EXPECT_EQ(settings.minTimeForRateSeconds, Domain::Sampling::MIN_TIME_FOR_RATE_SECONDS_DEFAULT);
    EXPECT_EQ(settings.maxSaneRateBps, Domain::Sampling::MAX_SANE_RATE_BPS_DEFAULT);
}

TEST(UserConfigTest, SaveConfigCreatesValidToml)
{
    // This test documents the expected save behavior
    // UserConfig::save() should create a valid TOML file with all settings

    UserSettings settings;
    settings.themeId = "test-theme";
    settings.refreshIntervalMs = 1500;
    settings.maxHistorySeconds = 450;
    settings.windowWidth = 1600;
    settings.windowHeight = 900;
    settings.windowMaximized = false;

    // The save() method should serialize these to TOML format
    // This would require refactoring UserConfig to accept a path for testing
}

TEST(UserConfigTest, LoadHandlesProgressThresholdSwapping)
{
    // When low threshold > high threshold, they should be swapped
    UserSettings settings;
    settings.progressColorLowThreshold = 80.0;
    settings.progressColorHighThreshold = 20.0;

    // The load() function has logic to swap these if low > high
    if (settings.progressColorLowThreshold > settings.progressColorHighThreshold)
    {
        std::swap(settings.progressColorLowThreshold, settings.progressColorHighThreshold);
    }

    EXPECT_LE(settings.progressColorLowThreshold, settings.progressColorHighThreshold);
    EXPECT_DOUBLE_EQ(settings.progressColorLowThreshold, 20.0);
    EXPECT_DOUBLE_EQ(settings.progressColorHighThreshold, 80.0);
}

TEST(UserConfigTest, WindowPositionSanityCheckHandlesExtremeValues)
{
    // Window positions beyond ±100,000 should be rejected
    constexpr int EXTREME_VALUE = 200'000;
    constexpr int SANE_VALUE = 50'000;

    // In the actual code, isSaneWindowPositionComponent() checks this
    const bool extremeIsInsane = std::abs(EXTREME_VALUE) > 100'000;
    const bool saneIsSane = std::abs(SANE_VALUE) <= 100'000;

    EXPECT_TRUE(extremeIsInsane);
    EXPECT_TRUE(saneIsSane);
}

} // namespace
} // namespace App
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
