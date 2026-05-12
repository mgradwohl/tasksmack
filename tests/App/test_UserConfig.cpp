// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "App/ProcessColumnConfig.h"
#include "App/UserConfig.h"
#include "Domain/SamplingConfig.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

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

// ========== UserConfig load() / save() integration (uses resetConfigPathForTesting) ==========

class UserConfigLoadSaveTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Save the original path so TearDown can restore it.
        m_OriginalPath = UserConfig::get().configPath();

        // Create a unique temp directory exclusive to this test process.
        // Use std::random_device + a create_directory retry loop so we
        // provably start with a fresh, exclusive directory. ctest runs each
        // test executable in a separate process with -j$(nproc); a retry loop is the
        // only reliable way to guarantee we never share state between tests.
        const auto base = std::filesystem::temp_directory_path() / "tasksmack_ucls_";
        std::random_device rd;
        constexpr int kMaxRetries = 100;
        for (int attempt = 0; attempt < kMaxRetries; ++attempt)
        {
            m_TempDir = base;
            m_TempDir += std::to_string(rd());
            std::error_code ec;
            if (std::filesystem::create_directory(m_TempDir, ec) && !ec)
            {
                break;
            }
            if (attempt == kMaxRetries - 1)
            {
                FAIL() << "Failed to create unique temp directory after " << kMaxRetries << " attempts";
            }
        }

        m_ConfigPath = m_TempDir / "config.toml";
        // Reset singleton to a clean state pointing at our temp file
        UserConfig::get().resetConfigPathForTesting(m_ConfigPath);
    }

    void TearDown() override
    {
        // Restore the real platform config path before removing the temp directory,
        // so the singleton is never left pointing at a non-existent file.
        UserConfig::get().resetConfigPathForTesting(m_OriginalPath);
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
    }

    std::filesystem::path m_TempDir;
    std::filesystem::path m_ConfigPath;
    std::filesystem::path m_OriginalPath;
};

TEST_F(UserConfigLoadSaveTest, LoadDoesNothingWhenFileAbsent)
{
    UserConfig::get().load();
    // Should keep defaults
    EXPECT_EQ(UserConfig::get().settings().refreshIntervalMs, Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS);
}

TEST_F(UserConfigLoadSaveTest, LoadIsIdempotent)
{
    // First load seeds defaults; second call must be a no-op (isLoaded guard)
    UserConfig::get().load();
    UserConfig::get().settings().refreshIntervalMs = 9999;
    UserConfig::get().load(); // should not re-read or reset
    EXPECT_EQ(UserConfig::get().settings().refreshIntervalMs, 9999);
}

TEST_F(UserConfigLoadSaveTest, LoadHandlesTomlParseError)
{
    // Unclosed string literal — causes toml::parse_error (EOF in string)
    // without triggering parser debug assertions
    {
        std::ofstream f(m_ConfigPath);
        f << "interval_ms = \"unclosed string\n";
    }
    UserConfig::get().load(); // should not throw
    EXPECT_EQ(UserConfig::get().settings().refreshIntervalMs, Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS);
}

TEST_F(UserConfigLoadSaveTest, LoadSwapsProgressThresholdsWhenInverted)
{
    // Write a config where low > high — load() must swap them
    {
        std::ofstream f(m_ConfigPath);
        f << "[ui]\n";
        f << "progress_color_low_threshold = 80.0\n";
        f << "progress_color_high_threshold = 20.0\n";
    }
    UserConfig::get().load();
    const auto& s = UserConfig::get().settings();
    EXPECT_LE(s.progressColorLowThreshold, s.progressColorHighThreshold);
}

TEST_F(UserConfigLoadSaveTest, LoadParsesAllFontSizes)
{
    const std::vector<std::pair<std::string, UI::FontSize>> cases = {
        {"small", UI::FontSize::Small},
        {"medium", UI::FontSize::Medium},
        {"large", UI::FontSize::Large},
        {"extra-large", UI::FontSize::ExtraLarge},
        {"huge", UI::FontSize::Huge},
        {"even-huger", UI::FontSize::EvenHuger},
    };

    for (const auto& [str, expected] : cases)
    {
        UserConfig::get().resetConfigPathForTesting(m_ConfigPath);
        {
            std::ofstream f(m_ConfigPath);
            f << "[font]\nsize = \"" << str << "\"\n";
        }
        UserConfig::get().load();
        EXPECT_EQ(UserConfig::get().settings().fontSize, expected) << "Failed for font size: " << str;
    }
}

TEST_F(UserConfigLoadSaveTest, LoadClampsInsaneWindowPosition)
{
    // Values far outside ±100000 must be reset to nullopt
    {
        std::ofstream f(m_ConfigPath);
        f << "[window]\n";
        f << "x = 999999999\n";
        f << "y = -999999999\n";
    }
    UserConfig::get().load();
    EXPECT_FALSE(UserConfig::get().settings().windowPosX.has_value());
    EXPECT_FALSE(UserConfig::get().settings().windowPosY.has_value());
}

TEST_F(UserConfigLoadSaveTest, SaveCreatesFileAndRoundTrips)
{
    auto& cfg = UserConfig::get();
    cfg.settings().refreshIntervalMs = 2000;
    cfg.settings().themeId = "dracula";
    cfg.settings().windowPosX = 150;
    cfg.settings().windowPosY = 250;
    cfg.save();

    ASSERT_TRUE(std::filesystem::exists(m_ConfigPath));

    cfg.resetConfigPathForTesting(m_ConfigPath);
    cfg.load();
    EXPECT_EQ(cfg.settings().refreshIntervalMs, 2000);
    EXPECT_EQ(cfg.settings().themeId, "dracula");
    EXPECT_TRUE(cfg.settings().windowPosX.has_value());
    EXPECT_EQ(*cfg.settings().windowPosX, 150);
    EXPECT_TRUE(cfg.settings().windowPosY.has_value());
    EXPECT_EQ(*cfg.settings().windowPosY, 250);
}

TEST_F(UserConfigLoadSaveTest, SaveWithNoOptionalWindowPos)
{
    auto& cfg = UserConfig::get();
    cfg.settings().windowPosX.reset();
    cfg.settings().windowPosY.reset();
    cfg.save();

    ASSERT_TRUE(std::filesystem::exists(m_ConfigPath));

    cfg.resetConfigPathForTesting(m_ConfigPath);
    cfg.load();
    EXPECT_FALSE(cfg.settings().windowPosX.has_value());
    EXPECT_FALSE(cfg.settings().windowPosY.has_value());
}

} // namespace
} // namespace App
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
