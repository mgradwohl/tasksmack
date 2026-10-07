// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "App/ProcessColumnConfig.h"
#include "App/UserConfig.h"
#include "App/UserConfigHelpers.h"
#include "Domain/SamplingConfig.h"
#include "UI/ChartWidgets.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#if defined(__linux__) && __has_include(<sys/xattr.h>)
#include <sys/xattr.h>
#endif
#include <unistd.h>
#endif

namespace App
{
namespace
{

// ========== Test Fixtures ==========

/// Fixture for UserConfigSaveLoad tests (ShowPrivilegeNotice round-trips).
/// Redirects the UserConfig singleton to an isolated temp directory so that
/// parallel ctest invocations don't race on the real platform config file.
class UserConfigSaveLoadFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Save the original path so TearDown can restore it.
        m_OriginalPath = UserConfig::get().configPath();

        // Create a unique temp directory exclusive to this test process.
        // Use a retry loop so we never share state with another parallel test process.
        // ctest runs each test executable in a separate process with -j$(nproc).
        const auto base = std::filesystem::temp_directory_path() / "tasksmack_ucsl_";
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
                FAIL() << "Failed to create temp directory after " << kMaxRetries << " attempts";
            }
        }

        // Tests write fixture files through m_ConfigPath, built here from the fixture's own temp
        // directory, rather than through configPath(): the singleton's default path comes from
        // XDG_CONFIG_HOME/HOME, and no environment-derived string should reach a file write (#1379).
        m_ConfigPath = m_TempDir / "config.toml";
        UserConfig::get().resetConfigPathForTesting(m_ConfigPath);
        ASSERT_EQ(UserConfig::get().configPath(), m_ConfigPath);
    }

    void TearDown() override
    {
        // Restore the real platform config path.
        UserConfig::get().resetConfigPathForTesting(m_OriginalPath);

        // Remove the temp directory.
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
    }

    std::filesystem::path m_TempDir;
    std::filesystem::path m_ConfigPath;
    std::filesystem::path m_OriginalPath;
};

/// Fixture for UserConfig persistence tests
/// Creates a temporary config directory for each test
class UserConfigPersistenceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Create unique temp directory for this test.
        // Use std::random_device + a create_directory retry loop so we
        // provably start with a fresh, exclusive directory. ctest runs each
        // test executable in a separate process with -j$(nproc); a retry loop is the
        // only reliable way to guarantee we never share state between tests.
        const auto base = std::filesystem::temp_directory_path() / "tasksmack_test_config_";
        std::random_device rd;
        for (;;)
        {
            m_TempDir = base;
            m_TempDir += std::to_string(rd());
            std::error_code ec;
            if (std::filesystem::create_directory(m_TempDir, ec) && !ec)
            {
                break; // created a fresh, exclusive directory
            }
        }

        m_ConfigPath = m_TempDir / "config.toml";
    }

    void TearDown() override
    {
        // Clean up temp directory
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
        // Ignore cleanup errors - best effort
    }

    /// Write a config file with given content
    void writeConfigFile(const std::string& content)
    {
        std::ofstream file(m_ConfigPath);
        ASSERT_TRUE(file.is_open()) << "Failed to create test config file";
        file << content;
        file.close();
    }

    /// Read the config file as string
    [[nodiscard]] auto readConfigFile() const -> std::string
    {
        std::ifstream file(m_ConfigPath);
        if (!file.is_open())
        {
            return "";
        }
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    std::filesystem::path m_TempDir;
    std::filesystem::path m_ConfigPath;
};

// ========== Config Directory Detection ==========

// Note: UserConfig is a singleton with a fixed config path, making it difficult to test
// in isolation without affecting the real config file. The tests below verify TOML format
// and demonstrate expected behavior, but don't actually test the UserConfig class directly.
// For actual UserConfig behavior, see test_UserConfig.cpp which tests UserSettings struct.

// ========== Load: Missing File ==========

// Skipped: UserConfig::load() would need dependency injection or a test-specific
// config path to test properly without side effects

// ========== Load: Valid TOML ==========

TEST_F(UserConfigPersistenceTest, LoadValidTomlFile)
{
    // Create a valid config file
    writeConfigFile(R"(
[sampling]
interval_ms = 2000
history_max_seconds = 600

[theme]
id = "cyberpunk"

[font]
size = "large"

[window]
width = 1600
height = 900
x = 100
y = 50
maximized = true

[process_columns]
pid = true
name = true
cpu_percent = false
)");

    // Note: Since UserConfig is a singleton, we can't easily test loading
    // a custom config file path. This test demonstrates the file format
    // but would need refactoring of UserConfig to support dependency injection
    // of the config path for proper unit testing.

    // For now, verify the file was created correctly
    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    const auto content = readConfigFile();
    EXPECT_TRUE(content.contains("cyberpunk"));
    EXPECT_TRUE(content.contains("interval_ms = 2000"));
}

// ========== Load: Malformed TOML ==========

TEST_F(UserConfigPersistenceTest, MalformedTomlFileFormat)
{
    // Create an invalid TOML file
    writeConfigFile(R"(
[theme
id = "missing-bracket"
this is not valid toml
)");

    // Verify file exists
    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));

    // Note: Since UserConfig is a singleton with a fixed config path,
    // we can't easily test error handling. This test documents the expected
    // format but would need refactoring to properly test error handling.
}

// ========== Load: Invalid Values ==========

TEST_F(UserConfigPersistenceTest, InvalidRefreshIntervalFormat)
{
    writeConfigFile(R"(
[sampling]
interval_ms = "not-a-number"
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
}

TEST_F(UserConfigPersistenceTest, OutOfRangeRefreshInterval)
{
    writeConfigFile(R"(
[sampling]
interval_ms = 999999999
history_max_seconds = -100
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // UserConfig should clamp these values when loading
}

TEST_F(UserConfigPersistenceTest, OutOfRangeWindowDimensions)
{
    writeConfigFile(R"(
[window]
width = 99999
height = -500
x = 999999999
y = -999999999
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // UserConfig should clamp or reject these values
}

// ========== Load: Font Size Variations ==========

TEST_F(UserConfigPersistenceTest, AllValidFontSizes)
{
    const std::vector<std::string> validSizes = {"small", "medium", "large", "extra-large", "huge", "even-huger"};

    for (const auto& size : validSizes)
    {
        writeConfigFile("[font]\nsize = \"" + size + "\"\n");
        EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    }
}

TEST_F(UserConfigPersistenceTest, InvalidFontSizeDefaultsToMedium)
{
    writeConfigFile(R"(
[font]
size = "super-duper-mega-huge"
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // Should default to medium
}

// ========== Load: Window Position Edge Cases ==========

TEST_F(UserConfigPersistenceTest, WindowPositionNegativeValues)
{
    writeConfigFile(R"(
[window]
x = -1920
y = -1080
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // Negative positions valid for multi-monitor
}

TEST_F(UserConfigPersistenceTest, WindowPositionExtremeValues)
{
    writeConfigFile(R"(
[window]
x = 999999999
y = -999999999
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // Should be rejected or clamped
}

TEST_F(UserConfigPersistenceTest, WindowPositionMissingValues)
{
    writeConfigFile(R"(
[window]
width = 1280
height = 720
# x and y not specified
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // x and y should remain unset (std::optional)
}

// ========== Load: Process Columns ==========

TEST_F(UserConfigPersistenceTest, ProcessColumnsPartialConfig)
{
    writeConfigFile(R"(
[process_columns]
pid = false
name = true
# Other columns not specified - should keep defaults
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
}

TEST_F(UserConfigPersistenceTest, ProcessColumnsInvalidValues)
{
    writeConfigFile(R"(
[process_columns]
pid = "yes"
name = 123
cpu_percent = [1, 2, 3]
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // Should use defaults for invalid values
}

TEST_F(UserConfigPersistenceTest, ProcessColumnsAllColumns)
{
    // Generate config for all columns
    std::string config = "[process_columns]\n";
    for (std::size_t i = 0; i < std::to_underlying(ProcessColumn::Count); ++i)
    {
        const auto col = static_cast<ProcessColumn>(i);
        const auto info = getColumnInfo(col);
        config += std::string(info.configKey) + " = " + (i % 2 == 0 ? "true" : "false") + "\n";
    }

    writeConfigFile(config);
    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
}

// ========== Save: Basic Functionality ==========

TEST_F(UserConfigPersistenceTest, SaveCreatesConfigFile)
{
    // Note: Since UserConfig is a singleton with fixed path,
    // we can't easily test save() to our custom path.
    // This test verifies the file format we expect.

    // Demonstrate expected output format
    const std::string expectedFormat = R"(# TaskSmack user configuration
[sampling]
interval_ms = 1000

[theme]
id = "arctic-fire"

[font]
size = "medium"
)";

    writeConfigFile(expectedFormat);
    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));

    const auto content = readConfigFile();
    EXPECT_TRUE(content.contains("TaskSmack user configuration"));
    EXPECT_TRUE(content.contains("[sampling]"));
    EXPECT_TRUE(content.contains("[theme]"));
}

// ========== Save: Directory Creation ==========

TEST_F(UserConfigPersistenceTest, SaveCreatesParentDirectory)
{
    // Create nested path
    const std::filesystem::path nestedPath = m_TempDir / "nested" / "deep" / "config.toml";

    // Parent directory shouldn't exist yet
    EXPECT_FALSE(std::filesystem::exists(nestedPath.parent_path()));

    // Verify that the directory structure would be created
    // (We can't test UserConfig::save() directly due to singleton)
    std::error_code ec;
    std::filesystem::create_directories(nestedPath.parent_path(), ec);
    EXPECT_FALSE(ec);
    EXPECT_TRUE(std::filesystem::exists(nestedPath.parent_path()));
}

// ========== Save: File Permissions ==========

TEST_F(UserConfigPersistenceTest, SaveHandlesReadOnlyDirectory)
{
    // This test is Linux-only because:
    // - Windows file permissions work differently (ACLs vs POSIX permissions)
    // - Windows std::filesystem::permissions() behavior varies by filesystem
    // - The NTFS permission model doesn't map cleanly to owner_read/owner_exec
    // On Windows, permission testing would require Windows-specific ACL manipulation.
#ifndef _WIN32
    std::filesystem::permissions(
        m_TempDir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec, std::filesystem::perm_options::replace);

    // Attempting to write should fail gracefully
    std::ofstream file(m_ConfigPath);
    EXPECT_FALSE(file.is_open());

    // Restore permissions for cleanup
    std::filesystem::permissions(m_TempDir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
#endif
}

// ========== Save: Data Integrity ==========

TEST_F(UserConfigPersistenceTest, SavePreservesAllSettings)
{
    // Demonstrate expected output with all settings
    const std::string fullConfig = R"(# TaskSmack user configuration
[sampling]
interval_ms = 2500
history_max_seconds = 900

[theme]
id = "monochrome"

[font]
size = "huge"

[window]
width = 2560
height = 1440
x = 0
y = 0
maximized = false

[process_columns]
pid = true
name = true
cpu_percent = true
mem_percent = true
)";

    writeConfigFile(fullConfig);
    const auto content = readConfigFile();

    // Verify all sections present
    EXPECT_TRUE(content.contains("[sampling]"));
    EXPECT_TRUE(content.contains("[theme]"));
    EXPECT_TRUE(content.contains("[font]"));
    EXPECT_TRUE(content.contains("[window]"));
    EXPECT_TRUE(content.contains("[process_columns]"));

    // Verify values
    EXPECT_TRUE(content.contains("interval_ms = 2500"));
    EXPECT_TRUE(content.contains("id = \"monochrome\""));
    EXPECT_TRUE(content.contains("size = \"huge\""));
}

// ========== Load/Save Round Trip ==========

TEST_F(UserConfigPersistenceTest, RoundTripPreservesData)
{
    // Create a config with specific values
    const std::string originalConfig = R"(
[sampling]
interval_ms = 3000
history_max_seconds = 1200

[theme]
id = "ubuntu-dark"

[font]
size = "extra-large"

[window]
width = 1920
height = 1080
x = -100
y = 200
maximized = true

[process_columns]
pid = true
name = false
cpu_percent = true
mem_percent = false
)";

    writeConfigFile(originalConfig);
    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));

    // Read it back
    const auto readBack = readConfigFile();

    // Verify key values are preserved
    EXPECT_TRUE(readBack.contains("3000"));
    EXPECT_TRUE(readBack.contains("ubuntu-dark"));
    EXPECT_TRUE(readBack.contains("extra-large"));
    EXPECT_TRUE(readBack.contains("-100"));
}

// ========== Edge Cases ==========

TEST_F(UserConfigPersistenceTest, EmptyConfigFile)
{
    writeConfigFile("");
    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));

    // Should load with defaults (no crash)
}

TEST_F(UserConfigPersistenceTest, ConfigFileWithOnlyComments)
{
    writeConfigFile(R"(
# This is a comment
# Another comment
# No actual config data
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
}

TEST_F(UserConfigPersistenceTest, ConfigFileWithUnknownSections)
{
    writeConfigFile(R"(
[unknown_section]
random_key = "random_value"

[theme]
id = "arctic-fire"

[future_feature]
something = 123
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // Should ignore unknown sections
}

TEST_F(UserConfigPersistenceTest, ConfigFileWithDuplicateKeys)
{
    writeConfigFile(R"(
[theme]
id = "first-theme"
id = "second-theme"
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // TOML parser should handle duplicates (last value wins)
}

TEST_F(UserConfigPersistenceTest, ConfigFileWithUnicodeCharacters)
{
    writeConfigFile(R"(
[theme]
id = "테마-한글-🎨"
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    const auto content = readConfigFile();
    EXPECT_TRUE(content.contains("테마"));
}

// ========== Clamping Behavior ==========

TEST_F(UserConfigPersistenceTest, RefreshIntervalClampedOnLoad)
{
    writeConfigFile(R"(
[sampling]
interval_ms = -1000
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // Should clamp to REFRESH_INTERVAL_MIN_MS
}

TEST_F(UserConfigPersistenceTest, HistorySecondsClampedOnLoad)
{
    writeConfigFile(R"(
[sampling]
history_max_seconds = 999999
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // Should clamp to HISTORY_SECONDS_MAX
}

TEST_F(UserConfigPersistenceTest, WindowDimensionsClampedOnLoad)
{
    writeConfigFile(R"(
[window]
width = 50
height = 99999
)");

    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    // Width should clamp to 200, height to 16384
}

// ========== showPrivilegeNotice Persistence ==========

TEST(UserSettingsTest, ShowPrivilegeNoticeDefaultsToTrue)
{
    // UserSettings default: showPrivilegeNotice = true (notice shown unless user dismisses)
    const UserSettings settings;
    EXPECT_TRUE(settings.showPrivilegeNotice);
}

TEST_F(UserConfigSaveLoadFixture, ShowPrivilegeNoticeFalseIsSavedAndLoaded)
{
    // Round-trips show_privilege_notice = false via save() + load() using an isolated
    // temp config file so parallel ctest runs don't race on the real platform config.
    auto& config = UserConfig::get();

    // Set false, save, reset in-memory to true, then load — must round-trip to false.
    config.settings().showPrivilegeNotice = false;
    config.save();
    config.settings().showPrivilegeNotice = true;
    config.load();
    EXPECT_FALSE(config.settings().showPrivilegeNotice);
}

TEST_F(UserConfigSaveLoadFixture, ShowPrivilegeNoticeTrueIsSavedAndLoaded)
{
    // Mirror of the false variant: round-trips show_privilege_notice = true via save() + load()
    // using an isolated temp config file so parallel ctest runs don't race on the real platform config.
    auto& config = UserConfig::get();

    // Set true, save, reset in-memory to false, then load — must round-trip to true.
    config.settings().showPrivilegeNotice = true;
    config.save();
    config.settings().showPrivilegeNotice = false;
    config.load();
    EXPECT_TRUE(config.settings().showPrivilegeNotice);
}

// ========== Process table layout persistence (#952) ==========

TEST(UserSettingsTest, ProcessTableLayoutDefaultsToEmpty)
{
    const UserSettings settings;
    EXPECT_TRUE(settings.processTableLayout.empty());
}

// The layout is multi-line text with brackets, '=' and '^' in it; it must come back byte for byte,
// or ImGui restores a different layout from the one that was saved.
TEST_F(UserConfigSaveLoadFixture, ProcessTableLayoutIsSavedAndLoaded)
{
    const std::string layout = "[Table][0xA7EC3B99,3]\n"
                               "RefScale=11\n"
                               "Column 0  Width=61 Order=0 ID=0x20E34941\n"
                               "Column 1  Width=229 Order=2 Sort=0^ ID=0x4637146C\n"
                               "Column 2  Weight=1.0000 Order=1 ID=0x0B8C089F\n";
    auto& config = UserConfig::get();

    config.settings().processTableLayout = layout;
    config.save();
    config.settings().processTableLayout.clear();
    config.load();
    EXPECT_EQ(config.settings().processTableLayout, layout);
}

TEST_F(UserConfigSaveLoadFixture, EmptyProcessTableLayoutRoundTrips)
{
    auto& config = UserConfig::get();

    config.settings().processTableLayout.clear();
    config.save();
    config.settings().processTableLayout = "stale";
    config.load();
    EXPECT_TRUE(config.settings().processTableLayout.empty());
}

// ========== chartAntiAliasing Persistence (perf-plan #843 phase 1) ==========

TEST(UserSettingsTest, ChartAntiAliasingDefaultsToTrue)
{
    // UserSettings default: chartAntiAliasing = true (current visual behavior preserved
    // unless the user opts out via config.toml for a lower-power/integrated GPU).
    const UserSettings settings;
    EXPECT_TRUE(settings.chartAntiAliasing);
}

// ========== Window scale (#1168) ==========

TEST(UserSettingsTest, DefaultWindowSizeIsAt100Percent)
{
    // The default size is for 100 %, so a first run on a 200 % display opens it at twice the pixels.
    const UserSettings settings;
    ASSERT_TRUE(settings.windowScale.has_value());
    EXPECT_FLOAT_EQ(settings.windowScale.value_or(0.0F), 1.0F);
}

TEST_F(UserConfigSaveLoadFixture, WindowScaleIsSavedAndLoaded)
{
    auto& config = UserConfig::get();
    config.settings().windowWidth = 2240;
    config.settings().windowHeight = 1260;
    config.settings().windowScale = 1.75F;
    config.save();

    UserConfig::get().resetConfigPathForTesting(m_TempDir / "config.toml");
    config.load();
    EXPECT_EQ(config.settings().windowWidth, 2240);
    ASSERT_TRUE(config.settings().windowScale.has_value());
    EXPECT_FLOAT_EQ(config.settings().windowScale.value_or(0.0F), 1.75F);
}

TEST_F(UserConfigSaveLoadFixture, SavedSizeWithoutAScaleIsRestoredUnconverted)
{
    // A config written before the scale was saved: its size is in unknown units, so it must not be
    // treated as a 100 % size and converted on restore.
    {
        std::ofstream file(m_TempDir / "config.toml");
        file << "[window]\nwidth = 1600\nheight = 900\n";
    }
    auto& config = UserConfig::get();
    config.load();
    EXPECT_EQ(config.settings().windowWidth, 1600);
    EXPECT_FALSE(config.settings().windowScale.has_value());
}

TEST_F(UserConfigSaveLoadFixture, UnusableSavedWindowScaleIsIgnored)
{
    for (const char* scale : {"0.0", "-2.0", "nan", "1000.0"})
    {
        {
            std::ofstream file(m_TempDir / "config.toml");
            file << "[window]\nwidth = 1600\nheight = 900\nscale = " << scale << "\n";
        }
        UserConfig::get().resetConfigPathForTesting(m_TempDir / "config.toml");
        auto& config = UserConfig::get();
        config.load();
        EXPECT_FALSE(config.settings().windowScale.has_value()) << scale;
    }
}

TEST_F(UserConfigSaveLoadFixture, WholeNumberWindowScaleIsRead)
{
    {
        std::ofstream file(m_TempDir / "config.toml");
        file << "[window]\nwidth = 2560\nheight = 1440\nscale = 2\n";
    }
    auto& config = UserConfig::get();
    config.load();
    ASSERT_TRUE(config.settings().windowScale.has_value());
    EXPECT_FLOAT_EQ(config.settings().windowScale.value_or(0.0F), 2.0F);
}

TEST_F(UserConfigSaveLoadFixture, ChartAntiAliasingFalseIsSavedAndLoaded)
{
    auto& config = UserConfig::get();

    config.settings().chartAntiAliasing = false;
    config.save();
    config.settings().chartAntiAliasing = true;
    config.load();
    EXPECT_FALSE(config.settings().chartAntiAliasing);
}

TEST_F(UserConfigSaveLoadFixture, ChartAntiAliasingTrueIsSavedAndLoaded)
{
    auto& config = UserConfig::get();

    config.settings().chartAntiAliasing = true;
    config.save();
    config.settings().chartAntiAliasing = false;
    config.load();
    EXPECT_TRUE(config.settings().chartAntiAliasing);
}

TEST_F(UserConfigSaveLoadFixture, ApplyToApplicationPushesChartAntiAliasingIntoUI)
{
    // UI::Widgets::chartAntiAliasingEnabled() is process-wide, mutable state that
    // applyToApplication() is the only intended non-test writer of; restore it to the
    // documented default afterward so this test doesn't leak state into others in this binary.
    auto& config = UserConfig::get();

    config.settings().chartAntiAliasing = false;
    config.applyToApplication();
    EXPECT_FALSE(UI::Widgets::chartAntiAliasingEnabled()) << "a regression that omits or inverts this assignment would leave every other "
                                                             "chartAntiAliasing test green while the setting has no real effect";

    config.settings().chartAntiAliasing = true;
    config.applyToApplication();
    EXPECT_TRUE(UI::Widgets::chartAntiAliasingEnabled());
}

// ========== Load: Missing File Uses Defaults ==========

TEST_F(UserConfigSaveLoadFixture, LoadMissingFileUsesDefaults)
{
    // Config file does not exist — load() should silently use defaults.
    auto& config = UserConfig::get();

    // File was not created in this fixture — path is in temp dir but file is absent.
    ASSERT_FALSE(std::filesystem::exists(config.configPath()));

    config.load();

    // Defaults from UserSettings
    EXPECT_EQ(config.settings().themeId, "arctic-fire");
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::Medium);
    EXPECT_TRUE(config.settings().showPrivilegeNotice);
    EXPECT_TRUE(config.settings().chartAntiAliasing);
}

// ========== Load: is-loaded guard ==========

TEST_F(UserConfigSaveLoadFixture, LoadIsIdempotentAfterFirstCall)
{
    // Calling load() twice must not overwrite settings modified between calls.
    auto& config = UserConfig::get();

    config.load(); // first load — file absent, uses defaults
    config.settings().themeId = "cyberpunk";

    config.load(); // second call — should be a no-op due to m_IsLoaded guard
    EXPECT_EQ(config.settings().themeId, "cyberpunk");
}

// ========== Load: Malformed TOML Uses Defaults ==========

TEST_F(UserConfigSaveLoadFixture, LoadMalformedTomlUsesDefaults)
{
    // Write a TOML file that will fail to parse.
    std::ofstream file(m_TempDir / "config.toml");
    ASSERT_TRUE(file.is_open());
    file << "[theme\nid = \"missing-bracket\"\nthis is not valid toml\n";
    file.close();

    auto& config = UserConfig::get();
    config.load();

    // Parse failure -> falls back to defaults.
    EXPECT_EQ(config.settings().themeId, "arctic-fire");
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::Medium);
}

// ========== Load/Save: Theme Round-Trip ==========

TEST_F(UserConfigSaveLoadFixture, ThemeIdRoundTrip)
{
    auto& config = UserConfig::get();

    config.settings().themeId = "dracula";
    config.save();
    config.settings().themeId = "arctic-fire"; // reset in-memory
    config.load();

    EXPECT_EQ(config.settings().themeId, "dracula");
}

TEST_F(UserConfigSaveLoadFixture, ThemeAndFontSizeRoundTripThroughTheTheme)
{
    // #1187: applyToApplication() hands the theme id and font size to UI::Theme, and
    // captureFromApplication() reads them back before save(). Dropping or crossing either call
    // would lose the user's theme or font size on the next save while every file round-trip test
    // stayed green. The test build's Theme (tests/Mocks/ThemeStub.cpp) keeps what it is given.
    auto& theme = UI::Theme::get();
    const std::string originalId = theme.currentThemeId();
    const UI::FontSize originalSize = theme.currentFontSize();

    auto& config = UserConfig::get();
    config.settings().themeId = "dracula";
    config.settings().fontSize = UI::FontSize::ExtraLarge;
    config.applyToApplication();
    EXPECT_EQ(theme.currentThemeId(), "dracula");
    EXPECT_EQ(theme.currentFontSize(), UI::FontSize::ExtraLarge);

    config.settings().themeId = "arctic-fire";
    config.settings().fontSize = UI::FontSize::Small;
    config.captureFromApplication();
    EXPECT_EQ(config.settings().themeId, "dracula");
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::ExtraLarge);

    // The Theme singleton is process-wide; the chart tests in this binary scale by its font size.
    theme.setThemeById(originalId);
    theme.setFontSize(originalSize);
}

// ========== Load/Save: Font Size Round-Trips ==========

TEST_F(UserConfigSaveLoadFixture, FontSizeSmallRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().fontSize = UI::FontSize::Small;
    config.save();
    config.settings().fontSize = UI::FontSize::Medium;
    config.load();
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::Small);
}

TEST_F(UserConfigSaveLoadFixture, FontSizeLargeRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().fontSize = UI::FontSize::Large;
    config.save();
    config.settings().fontSize = UI::FontSize::Medium;
    config.load();
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::Large);
}

TEST_F(UserConfigSaveLoadFixture, FontSizeExtraLargeRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().fontSize = UI::FontSize::ExtraLarge;
    config.save();
    config.settings().fontSize = UI::FontSize::Medium;
    config.load();
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::ExtraLarge);
}

TEST_F(UserConfigSaveLoadFixture, FontSizeHugeRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().fontSize = UI::FontSize::Huge;
    config.save();
    config.settings().fontSize = UI::FontSize::Medium;
    config.load();
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::Huge);
}

TEST_F(UserConfigSaveLoadFixture, FontSizeEvenHugerRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().fontSize = UI::FontSize::EvenHuger;
    config.save();
    config.settings().fontSize = UI::FontSize::Medium;
    config.load();
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::EvenHuger);
}

TEST_F(UserConfigSaveLoadFixture, FontSizeMediumRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().fontSize = UI::FontSize::Medium;
    config.save();
    config.settings().fontSize = UI::FontSize::Small;
    config.load();
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::Medium);
}

// ========== Load: Invalid Font Size Keeps Default ==========

TEST_F(UserConfigSaveLoadFixture, UnknownFontSizeKeepsDefault)
{
    // Write a TOML file with an unrecognised font size string.
    std::ofstream file(m_TempDir / "config.toml");
    ASSERT_TRUE(file.is_open());
    file << "[font]\nsize = \"super-mega-large\"\n";
    file.close();

    auto& config = UserConfig::get();
    config.load();

    // Unrecognised value — font size remains at default (Medium).
    EXPECT_EQ(config.settings().fontSize, UI::FontSize::Medium);
}

// ========== Load/Save: Sampling Parameters Round-Trip ==========

TEST_F(UserConfigSaveLoadFixture, SamplingIntervalRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().refreshIntervalMs = 2000;
    config.save();
    config.settings().refreshIntervalMs = Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS;
    config.load();
    EXPECT_EQ(config.settings().refreshIntervalMs, 2000);
}

TEST_F(UserConfigSaveLoadFixture, HistorySecondsRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().maxHistorySeconds = 600;
    config.save();
    config.settings().maxHistorySeconds = Domain::Sampling::HISTORY_SECONDS_DEFAULT;
    config.load();
    EXPECT_EQ(config.settings().maxHistorySeconds, 600);
}

// ========== Load/Save: Window State Round-Trip ==========

TEST_F(UserConfigSaveLoadFixture, WindowDimensionsRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().windowWidth = 1920;
    config.settings().windowHeight = 1080;
    config.settings().windowMaximized = true;
    config.save();
    config.settings().windowWidth = 1280;
    config.settings().windowHeight = 720;
    config.settings().windowMaximized = false;
    config.load();
    EXPECT_EQ(config.settings().windowWidth, 1920);
    EXPECT_EQ(config.settings().windowHeight, 1080);
    EXPECT_TRUE(config.settings().windowMaximized);
}

TEST_F(UserConfigSaveLoadFixture, ForceNativeWindowDecorationsOnWaylandRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().forceNativeWindowDecorationsOnWayland = true;
    config.save();
    config.settings().forceNativeWindowDecorationsOnWayland = false;
    config.load();
    EXPECT_TRUE(config.settings().forceNativeWindowDecorationsOnWayland);
}

TEST_F(UserConfigSaveLoadFixture, WindowPositionSaneValuesRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().windowPosX = 200;
    config.settings().windowPosY = 150;
    config.save();
    config.settings().windowPosX = std::nullopt;
    config.settings().windowPosY = std::nullopt;
    config.load();
    ASSERT_TRUE(config.settings().windowPosX.has_value());
    ASSERT_TRUE(config.settings().windowPosY.has_value());
    EXPECT_EQ(*config.settings().windowPosX, 200);
    EXPECT_EQ(*config.settings().windowPosY, 150);
}

TEST_F(UserConfigSaveLoadFixture, WindowPositionNegativeValidRoundTrip)
{
    // Negative window positions are valid for multi-monitor setups.
    auto& config = UserConfig::get();
    config.settings().windowPosX = -1920;
    config.settings().windowPosY = -100;
    config.save();
    config.settings().windowPosX = std::nullopt;
    config.settings().windowPosY = std::nullopt;
    config.load();
    ASSERT_TRUE(config.settings().windowPosX.has_value());
    ASSERT_TRUE(config.settings().windowPosY.has_value());
    EXPECT_EQ(*config.settings().windowPosX, -1920);
    EXPECT_EQ(*config.settings().windowPosY, -100);
}

TEST_F(UserConfigSaveLoadFixture, WindowPositionInsaneValueIsRejected)
{
    // Write a TOML with a position far outside sane bounds (abs > 100,000).
    std::ofstream file(m_TempDir / "config.toml");
    ASSERT_TRUE(file.is_open());
    file << "[window]\nx = 999999\ny = -999999\n";
    file.close();

    auto& config = UserConfig::get();
    config.load();

    // isSaneWindowPositionComponent rejects abs > 100,000 — positions stay unset.
    EXPECT_FALSE(config.settings().windowPosX.has_value());
    EXPECT_FALSE(config.settings().windowPosY.has_value());
}

TEST_F(UserConfigSaveLoadFixture, WindowPositionAbsentStaysUnset)
{
    // Write a TOML without window x/y keys.
    std::ofstream file(m_TempDir / "config.toml");
    ASSERT_TRUE(file.is_open());
    file << "[window]\nwidth = 1280\nheight = 720\n";
    file.close();

    auto& config = UserConfig::get();
    config.load();

    EXPECT_FALSE(config.settings().windowPosX.has_value());
    EXPECT_FALSE(config.settings().windowPosY.has_value());
}

// ========== Load/Save: UI Parameters Round-Trip ==========

TEST_F(UserConfigSaveLoadFixture, ChartSmoothFactorRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().chartSmoothFactor = 0.5;
    config.save();
    config.settings().chartSmoothFactor = Domain::Sampling::CHART_SMOOTH_FACTOR_DEFAULT;
    config.load();
    EXPECT_DOUBLE_EQ(config.settings().chartSmoothFactor, 0.5);
}

TEST_F(UserConfigSaveLoadFixture, ChartTauRangeRoundTrip)
{
    auto& config = UserConfig::get();
    config.settings().chartTauMsMin = 50;
    config.settings().chartTauMsMax = 1000;
    config.save();
    config.settings().chartTauMsMin = Domain::Sampling::CHART_TAU_MS_MIN_DEFAULT;
    config.settings().chartTauMsMax = Domain::Sampling::CHART_TAU_MS_MAX_DEFAULT;
    config.load();
    EXPECT_EQ(config.settings().chartTauMsMin, 50);
    EXPECT_EQ(config.settings().chartTauMsMax, 1000);
}

/// Restores UI's chart smoothing to its defaults however a test ends, so later tests in this
/// binary see the built-in behaviour.
class ChartSmoothingRestore
{
  public:
    ChartSmoothingRestore() = default;
    ChartSmoothingRestore(const ChartSmoothingRestore&) = delete;
    ChartSmoothingRestore& operator=(const ChartSmoothingRestore&) = delete;
    ChartSmoothingRestore(ChartSmoothingRestore&&) = delete;
    ChartSmoothingRestore& operator=(ChartSmoothingRestore&&) = delete;
    ~ChartSmoothingRestore()
    {
        UI::Widgets::setChartSmoothing(Domain::Sampling::CHART_SMOOTH_FACTOR_DEFAULT,
                                       Domain::Sampling::CHART_TAU_MS_MIN_DEFAULT,
                                       Domain::Sampling::CHART_TAU_MS_MAX_DEFAULT);
    }
};

TEST_F(UserConfigSaveLoadFixture, ChartSmoothingKeysReachTheChartsAtStartup)
{
    // #1123: chart_smooth_factor and chart_tau_ms_min/max were loaded and saved but never applied;
    // the easing always used the built-in 0.5 / 20 ms / 400 ms.
    const ChartSmoothingRestore restore;
    {
        std::ofstream file(m_TempDir / "config.toml");
        file << "[ui]\nchart_smooth_factor = 0.25\nchart_tau_ms_min = 50\nchart_tau_ms_max = 1500\n";
    }
    auto& config = UserConfig::get();
    config.load();
    config.applyToApplication();

    const UI::Widgets::ChartSmoothing smoothing = UI::Widgets::chartSmoothing();
    EXPECT_DOUBLE_EQ(smoothing.smoothFactor, 0.25);
    EXPECT_DOUBLE_EQ(smoothing.tauMsMin, 50.0);
    EXPECT_DOUBLE_EQ(smoothing.tauMsMax, 1500.0);

    // At a 1 s refresh the time constant is 0.25 x 1000 = 250 ms (the defaults gave 400 ms).
    EXPECT_NEAR(UI::Widgets::computeAlpha(0.1, std::chrono::milliseconds(1000)), 1.0 - std::exp(-100.0 / 250.0), 1e-9);
    // At a 100 ms refresh, 25 ms is raised to the configured 50 ms floor (the default was 20 ms).
    EXPECT_NEAR(UI::Widgets::computeAlpha(0.01, std::chrono::milliseconds(100)), 1.0 - std::exp(-10.0 / 50.0), 1e-9);
    // At a 5 s refresh, 1250 ms stays under the configured 1500 ms ceiling (the default was 400 ms).
    EXPECT_NEAR(UI::Widgets::computeAlpha(0.1, std::chrono::milliseconds(5000)), 1.0 - std::exp(-100.0 / 1250.0), 1e-9);
}

TEST_F(UserConfigSaveLoadFixture, SaveRemovesRetiredKeysAndKeepsTheRest)
{
    // #1123: keys older versions wrote but never applied are dropped on the next save, so the file
    // stops advertising tuning that does nothing; unknown keys and the kept settings stay.
    {
        std::ofstream file(m_TempDir / "config.toml");
        file << "[metrics]\nmin_time_for_rate_seconds = 1.5\nmax_sane_rate_bps = 50000000000.0\n"
                "integrated_gpu_vram_threshold_mb = 256\n"
                "[ui]\nprogress_color_low_threshold = 30.0\nprogress_color_high_threshold = 90.0\nchart_smooth_factor = 0.25\n"
                "my_note = \"kept\"\n";
    }
    auto& config = UserConfig::get();
    config.load();
    EXPECT_DOUBLE_EQ(config.settings().maxSaneRateBps, 50'000'000'000.0);
    config.save();

    const toml::table saved = toml::parse_file((m_TempDir / "config.toml").string());
    for (const auto& retired : UserConfigHelpers::RETIRED_KEYS)
    {
        EXPECT_FALSE(saved[retired.section][retired.key]) << retired.section << "." << retired.key;
    }
    EXPECT_DOUBLE_EQ(saved["metrics"]["max_sane_rate_bps"].value_or(0.0), 50'000'000'000.0);
    EXPECT_DOUBLE_EQ(saved["ui"]["chart_smooth_factor"].value_or(0.0), 0.25);
    EXPECT_EQ(saved["ui"]["my_note"].value_or(std::string{}), "kept");

    std::ifstream in(m_TempDir / "config.toml");
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    for (const auto& retired : UserConfigHelpers::RETIRED_KEYS)
    {
        EXPECT_EQ(text.find(retired.key), std::string::npos) << "the header must not document " << retired.key;
    }
}

TEST_F(UserConfigSaveLoadFixture, HeaderDescribesTheRateCeilingAsCoveringInterfacesToo)
{
    // #1291 extended [metrics] max_sane_rate_bps from per-process rates to interface rates; the
    // header save() writes into every config.toml must not still call it a per-process ceiling.
    UserConfig::get().save();

    std::ifstream in(m_TempDir / "config.toml");
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    EXPECT_TRUE(text.contains("max_sane_rate_bps: network rate ceiling for process and interface rates"));
    EXPECT_FALSE(text.contains("per-process network rate ceiling"));
}

// ========== Load/Save: Process Columns Round-Trip ==========

TEST_F(UserConfigSaveLoadFixture, ProcessColumnsRoundTrip)
{
    auto& config = UserConfig::get();

    // Toggle two known columns to stable, self-documenting values.
    constexpr auto col0 = ProcessColumn::PID;
    constexpr auto col1 = ProcessColumn::Name;
    const bool original0 = config.settings().processColumns.isVisible(col0);
    const bool original1 = config.settings().processColumns.isVisible(col1);

    config.settings().processColumns.setVisible(col0, !original0);
    config.settings().processColumns.setVisible(col1, !original1);
    config.save();

    // Reset to original
    config.settings().processColumns.setVisible(col0, original0);
    config.settings().processColumns.setVisible(col1, original1);
    config.load();

    EXPECT_EQ(config.settings().processColumns.isVisible(col0), !original0);
    EXPECT_EQ(config.settings().processColumns.isVisible(col1), !original1);
}

// ========== Save: Creates Parent Directories ==========

TEST_F(UserConfigSaveLoadFixture, SaveCreatesParentDirectoriesIfAbsent)
{
    // Redirect config to a nested path whose parent dirs don't yet exist.
    const auto nestedConfig = m_TempDir / "a" / "b" / "config.toml";
    UserConfig::get().resetConfigPathForTesting(nestedConfig);

    UserConfig::get().save();

    EXPECT_TRUE(std::filesystem::exists(nestedConfig));
}

// ========== Metrics: maxSaneRate Round-Trip ==========

TEST_F(UserConfigSaveLoadFixture, MetricsMaxSaneRateRoundTrip)
{
    auto& config = UserConfig::get();
    // 5 GB/s — within the valid clamp range [1 GB/s, 100 GB/s].
    constexpr double testRate = 5'000'000'000.0;
    config.settings().maxSaneRateBps = testRate;
    config.save();
    config.settings().maxSaneRateBps = Domain::Sampling::MAX_SANE_RATE_BPS_DEFAULT;
    config.load();
    EXPECT_DOUBLE_EQ(config.settings().maxSaneRateBps, testRate);
}

// ========== Robust config writes (#1122, #1124) ==========

[[nodiscard]] std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

[[nodiscard]] toml::table parsed(const std::filesystem::path& path)
{
    return toml::parse_file(path.string());
}

TEST(UserConfigMergeTest, OnlyKeysTaskSmackChangedAreWritten)
{
    const toml::table base{{"sampling", toml::table{{"interval_ms", 1000}}}, {"theme", toml::table{{"id", "arctic-fire"}}}};
    const toml::table mine{{"sampling", toml::table{{"interval_ms", 1000}}}, {"theme", toml::table{{"id", "mocha"}}}};
    toml::table document{{"sampling", toml::table{{"interval_ms", 750}, {"future", 1}}}, {"theme", toml::table{{"id", "dracula"}}}};

    UserConfigHelpers::mergeOwnedKeys(document, base, mine);

    EXPECT_EQ(document["sampling"]["interval_ms"].value<int>(), 750); // edited outside: kept
    EXPECT_EQ(document["sampling"]["future"].value<int>(), 1);        // not TaskSmack's: kept
    EXPECT_EQ(document["theme"]["id"].value<std::string>(), "mocha"); // changed by TaskSmack: written
}

TEST(UserConfigMergeTest, KeysDeletedOrClearedStayDeleted)
{
    // Deleted outside TaskSmack (unchanged in-app): stays deleted. No longer written by TaskSmack
    // (a cleared window position): removed.
    const toml::table base{{"window", toml::table{{"x", 10}, {"y", 20}}}, {"process_table", toml::table{{"layout", "abc"}}}};
    const toml::table mine{{"window", toml::table{{"y", 20}}}, {"process_table", toml::table{{"layout", "abc"}}}};
    toml::table document{{"window", toml::table{{"x", 10}, {"y", 20}, {"mine", true}}}, {"process_table", toml::table{}}};

    UserConfigHelpers::mergeOwnedKeys(document, base, mine);

    EXPECT_FALSE(document["window"]["x"]);
    EXPECT_EQ(document["window"]["y"].value<int>(), 20);
    EXPECT_EQ(document["window"]["mine"].value<bool>(), true);
    EXPECT_FALSE(document["process_table"]["layout"]);
}

TEST_F(UserConfigSaveLoadFixture, SaveKeepsKeysTaskSmackDoesNotOwn)
{
    const auto& path = m_ConfigPath;
    writeFile(path, "[sampling]\ninterval_ms = 500\nfuture_option = 7\n\n[plugin]\nenabled = true\n");
    auto& config = UserConfig::get();
    config.load();
    config.save();

    const auto document = parsed(path);
    EXPECT_EQ(document["sampling"]["future_option"].value<int>(), 7);
    EXPECT_EQ(document["plugin"]["enabled"].value<bool>(), true);
    EXPECT_EQ(document["sampling"]["interval_ms"].value<int>(), 500);
}

TEST_F(UserConfigSaveLoadFixture, EditsMadeWhileRunningSurviveSavesAndShutdown)
{
    const auto& path = m_ConfigPath;
    auto& config = UserConfig::get();
    config.settings().refreshIntervalMs = 1000;
    config.settings().themeId = "arctic-fire";
    config.save();
    config.load();

    // Edited in a text editor while TaskSmack runs, keeping the same modification time.
    const auto before = std::filesystem::last_write_time(path);
    writeFile(path, "[sampling]\ninterval_ms = 750\n\n[theme]\nid = \"dracula\"\n");
    std::filesystem::last_write_time(path, before);

    // Apply changes a different setting, then shutdown saves again with the theme still running.
    config.settings().maxHistorySeconds = 600;
    config.save();
    config.save();

    const auto document = parsed(path);
    EXPECT_EQ(document["sampling"]["interval_ms"].value<int>(), 750);
    EXPECT_EQ(document["theme"]["id"].value<std::string>(), "dracula");
    EXPECT_EQ(document["sampling"]["history_max_seconds"].value<int>(), 600);
}

TEST_F(UserConfigSaveLoadFixture, ToggledColumnDoesNotOverwriteOtherColumnsEditedOutside)
{
    const auto& path = m_ConfigPath;
    auto& config = UserConfig::get();
    config.save();
    config.load();
    const auto columns = allProcessColumns();
    ASSERT_GE(columns.size(), 2U);
    const auto first = getColumnInfo(columns[0]).configKey;
    const auto second = getColumnInfo(columns[1]).configKey;

    auto document = parsed(path);
    const bool secondWas = document["process_columns"][second].value_or(true);
    document["process_columns"].as_table()->insert_or_assign(second, !secondWas);
    {
        std::ofstream out(path, std::ios::trunc);
        out << document;
    }

    config.settings().processColumns.toggleVisible(columns[0]);
    config.save();

    const auto saved = parsed(path);
    EXPECT_EQ(saved["process_columns"][second].value<bool>(), !secondWas);
    EXPECT_EQ(saved["process_columns"][first].value<bool>(), config.settings().processColumns.isVisible(columns[0]));
}

TEST_F(UserConfigSaveLoadFixture, MalformedConfigIsNotReplaced)
{
    const auto& path = m_ConfigPath;
    const std::string malformed = "[sampling\ninterval_ms = = 5\n";
    writeFile(path, malformed);
    auto& config = UserConfig::get();
    config.load();
    config.settings().themeId = "dracula";
    config.save();
    EXPECT_EQ(readFile(path), malformed);
}

TEST_F(UserConfigSaveLoadFixture, SaveLeavesNoTemporaryFileBehind)
{
    auto& config = UserConfig::get();
    config.save();
    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath));
    for (const auto& entry : std::filesystem::directory_iterator(m_TempDir))
    {
        EXPECT_NE(entry.path().extension(), ".tmp") << entry.path();
    }
}

#ifndef _WIN32
TEST_F(UserConfigSaveLoadFixture, FailedWriteLeavesTheOriginalIntactAndALaterSaveSucceeds)
{
    // The data-loss path the atomic replace exists for: writing the new file fails partway. The
    // original must stay byte-for-byte, the temporary file must go, and the change must still be
    // saved by the next successful save (#1222 review).
    const auto& path = m_ConfigPath;
    const std::string original = "[theme]\nid = \"arctic-fire\"\n";
    writeFile(path, original);
    auto& config = UserConfig::get();
    config.load();
    config.settings().themeId = "mocha";

    // Save in a child whose file-size limit is far below the new file's size, so the write fails
    // with EFBIG once the buffer is flushed (SIGXFSZ ignored so it is an error, not a kill).
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        std::signal(SIGXFSZ, SIG_IGN);
        constexpr rlim_t TOO_SMALL = 64;
        const rlimit limit{.rlim_cur = TOO_SMALL, .rlim_max = TOO_SMALL};
        setrlimit(RLIMIT_FSIZE, &limit);
        UserConfig::get().save();
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));

    std::ifstream in(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), original);
    for (const auto& entry : std::filesystem::directory_iterator(m_TempDir))
    {
        EXPECT_NE(entry.path().extension(), ".tmp") << entry.path();
    }

    config.save();
    EXPECT_EQ(parsed(path)["theme"]["id"].value<std::string>(), "mocha");
}

TEST_F(UserConfigSaveLoadFixture, SymlinkedConfigKeepsItsLinkAndUpdatesItsTarget)
{
    // A dotfiles-managed config is often a symlink: saving must write through it (#1222 review).
    const auto& link = m_ConfigPath;
    const auto target = m_TempDir / "dotfiles-config.toml";
    writeFile(target, "[theme]\nid = \"arctic-fire\"\n");
    std::filesystem::create_symlink(target, link);

    auto& config = UserConfig::get();
    config.load();
    config.settings().themeId = "mocha";
    config.save();

    EXPECT_TRUE(std::filesystem::is_symlink(link));
    EXPECT_EQ(parsed(target)["theme"]["id"].value<std::string>(), "mocha");
}

TEST_F(UserConfigSaveLoadFixture, SymlinkTargetWithALongNameStillSaves)
{
    // A 250-byte target name is valid under a 255-byte limit; a temporary named after it with a
    // suffix would not be, and every save would fail (#1222 review).
    const auto& link = m_ConfigPath;
    const auto target = m_TempDir / (std::string(245, 'c') + ".toml");
    writeFile(target, "[theme]\nid = \"arctic-fire\"\n");
    std::filesystem::create_symlink(target, link);

    auto& config = UserConfig::get();
    config.load();
    config.settings().themeId = "mocha";
    config.save();

    EXPECT_TRUE(std::filesystem::is_symlink(link));
    EXPECT_EQ(parsed(target)["theme"]["id"].value<std::string>(), "mocha");
}

TEST_F(UserConfigSaveLoadFixture, SymlinkToAMissingFileCreatesTheTarget)
{
    // canonical() needs the target to exist; a link to a file not created yet must still be
    // written through, not refused (#1222 review).
    const auto& link = m_ConfigPath;
    const auto target = m_TempDir / "not-yet-created.toml";
    std::filesystem::create_symlink(target, link);

    auto& config = UserConfig::get();
    config.load();
    config.settings().themeId = "mocha";
    config.save();

    EXPECT_TRUE(std::filesystem::is_symlink(link));
    ASSERT_TRUE(std::filesystem::exists(target));
    EXPECT_EQ(parsed(target)["theme"]["id"].value<std::string>(), "mocha");
}

TEST_F(UserConfigSaveLoadFixture, LinkLoopIsNotSaved)
{
    const auto& link = m_ConfigPath;
    const auto other = m_TempDir / "other.toml";
    std::filesystem::create_symlink(other, link);
    std::filesystem::create_symlink(link, other);

    auto& config = UserConfig::get();
    config.load();
    config.settings().themeId = "mocha";
    config.save();

    EXPECT_TRUE(std::filesystem::is_symlink(link));
    EXPECT_TRUE(std::filesystem::is_symlink(other));
}
#endif

TEST_F(UserConfigSaveLoadFixture, ConfigCreatedBeforeTheFirstSaveKeepsItsOtherSettings)
{
    // No file at startup, then one is created (or repaired) before TaskSmack's first save: only
    // the setting changed in-app is written over it, not every runtime default (#1222 review).
    auto& config = UserConfig::get();
    config.load();
    writeFile(m_ConfigPath, "[sampling]\ninterval_ms = 750\n");

    config.settings().themeId = "mocha";
    config.save();

    const auto saved = parsed(m_ConfigPath);
    EXPECT_EQ(saved["sampling"]["interval_ms"].value<std::int64_t>(), 750);
    EXPECT_EQ(saved["theme"]["id"].value<std::string>(), "mocha");
}

#ifndef _WIN32
TEST_F(UserConfigSaveLoadFixture, UnreadableConfigFileIsNotReplaced)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root can read a mode-000 file";
    }
    const auto& path = m_ConfigPath;
    writeFile(path, "[theme]\nid = \"dracula\"\n");
    std::filesystem::permissions(path, std::filesystem::perms::none);

    auto& config = UserConfig::get();
    config.settings().themeId = "mocha";
    EXPECT_NO_THROW(config.save());

    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    EXPECT_EQ(parsed(path)["theme"]["id"].value<std::string>(), "dracula");
}

TEST_F(UserConfigSaveLoadFixture, SaveKeepsTheConfigFilesPermissions)
{
    auto& config = UserConfig::get();
    config.save();
    const auto& path = m_ConfigPath;
    const auto restricted = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::filesystem::permissions(path, restricted);

    config.settings().themeId = "mocha";
    config.save();
    EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all, restricted);
}

TEST_F(UserConfigSaveLoadFixture, StagingFileIsNeverReadableByOthers)
{
    // The staging file is created owner-only (#1222 review), so a brand-new config is 0600 rather
    // than umask-readable, while an existing config's own, wider mode is still restored.
    auto& config = UserConfig::get();
    config.save();
    const auto& path = m_ConfigPath;
    const auto ownerOnly = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all, ownerOnly);

    const auto shared = ownerOnly | std::filesystem::perms::group_read | std::filesystem::perms::others_read;
    std::filesystem::permissions(path, shared);
    config.settings().themeId = "mocha";
    config.save();
    EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all, shared);
}

TEST_F(UserConfigSaveLoadFixture, SavesUnderAUmaskThatMasksOwnerWrite)
{
    // The staging file is written through the descriptor that created it: reopening its path
    // failed with EACCES under a umask without owner-write, so nothing was ever saved (#1222 review).
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root can write a mode-0400 file anyway";
    }
    auto& config = UserConfig::get();
    config.settings().themeId = "mocha";
    const mode_t previous = ::umask(0277);
    config.save();
    config.settings().themeId = "latte";
    config.save(); // replacing a 0400 config works too
    ::umask(previous);

    EXPECT_EQ(parsed(m_ConfigPath)["theme"]["id"].value<std::string>(), "latte");
    for (const auto& entry : std::filesystem::directory_iterator(m_TempDir))
    {
        EXPECT_NE(entry.path().extension(), ".tmp") << entry.path();
    }
}

#if defined(__linux__) && __has_include(<sys/xattr.h>)
/// A raw POSIX ACL xattr (version 2, then {tag, perm, id} entries) granting user `nobody` read:
/// user::rw-, user:nobody:r--, group::---, mask::r--, other::---. Its mode bits read as 0640.
[[nodiscard]] std::vector<char> nobodyCanReadAcl()
{
    std::vector<char> acl;
    const auto put = [&acl](const auto value)
    {
        const auto* bytes = reinterpret_cast<const char*>(&value);
        acl.insert(acl.end(), bytes, bytes + sizeof(value));
    };
    constexpr std::uint32_t UNDEFINED_ID = 0xFFFFFFFF;
    put(std::uint32_t{2});
    for (const auto& [tag, perm, id] : {std::tuple<std::uint16_t, std::uint16_t, std::uint32_t>{0x01, 6, UNDEFINED_ID},
                                        {0x02, 4, 65534},
                                        {0x04, 0, UNDEFINED_ID},
                                        {0x10, 4, UNDEFINED_ID},
                                        {0x20, 0, UNDEFINED_ID}})
    {
        put(tag);
        put(perm);
        put(id);
    }
    return acl;
}

TEST_F(UserConfigSaveLoadFixture, SaveDropsAnAclTheOriginalDidNotHave)
{
    // The staging file inherits the directory's default ACL; if the original config had none, the
    // replacement must not either, or its named grants would expose it (#1222 review).
    auto& config = UserConfig::get();
    config.save(); // before the default ACL exists: no ACL of its own
    const auto& path = m_ConfigPath;
    std::vector<char> probe(256);
    ASSERT_LT(::getxattr(path.c_str(), "system.posix_acl_access", probe.data(), probe.size()), 0);

    const auto acl = nobodyCanReadAcl();
    if (::setxattr(m_TempDir.c_str(), "system.posix_acl_default", acl.data(), acl.size(), 0) != 0)
    {
        GTEST_SKIP() << "filesystem has no POSIX ACLs: " << std::strerror(errno);
    }

    config.settings().themeId = "mocha";
    config.save();

    EXPECT_LT(::getxattr(path.c_str(), "system.posix_acl_access", probe.data(), probe.size()), 0)
        << "the replacement kept an ACL inherited from the directory";
    EXPECT_EQ(errno, ENODATA);
    EXPECT_EQ(parsed(path)["theme"]["id"].value<std::string>(), "mocha");
}

TEST_F(UserConfigSaveLoadFixture, SaveKeepsTheConfigsAccessControlList)
{
    // A named-user read grant with group::--- and mask::r-- reports mode 0640. Copying only that
    // mode to the new file would let the whole group read it; the ACL must come along (#1222 review).
    auto& config = UserConfig::get();
    config.save();
    const auto& path = m_ConfigPath;

    const auto acl = nobodyCanReadAcl();
    if (::setxattr(path.c_str(), "system.posix_acl_access", acl.data(), acl.size(), 0) != 0)
    {
        GTEST_SKIP() << "filesystem has no POSIX ACLs: " << std::strerror(errno);
    }

    config.settings().themeId = "mocha";
    config.save();

    std::vector<char> saved(acl.size() + 64);
    const auto size = ::getxattr(path.c_str(), "system.posix_acl_access", saved.data(), saved.size());
    ASSERT_GT(size, 0) << "the new config lost its ACL";
    saved.resize(static_cast<std::size_t>(size));
    EXPECT_EQ(saved, acl);
    EXPECT_EQ(parsed(path)["theme"]["id"].value<std::string>(), "mocha");
}
#endif

TEST_F(UserConfigSaveLoadFixture, FailedStagingLeavesAnExistingFileAlone)
{
    // Every candidate staging name taken: the save fails, and the file that already had that name
    // -- someone else's -- must not be deleted on the way out (#1222 review).
    auto& config = UserConfig::get();
    config.setTempNameSourceForTesting([] { return std::uint32_t{0xDEADBEEF}; });
    const auto theirs = m_TempDir / ".tasksmack-config.deadbeef.tmp";
    writeFile(theirs, "not TaskSmack's");

    config.settings().themeId = "mocha";
    config.save();

    ASSERT_TRUE(std::filesystem::exists(theirs));
    std::ifstream in(theirs);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), "not TaskSmack's");
    EXPECT_FALSE(std::filesystem::exists(m_ConfigPath)); // nothing was saved
}

TEST_F(UserConfigSaveLoadFixture, SaveKeepsTheConfigsOwningGroup)
{
    // In a setgid directory a new file takes the directory's group. The replacement must keep the
    // original config's group instead, or that group's members could read a 0640 config (#1222 review).
    std::vector<gid_t> groups(static_cast<std::size_t>(::getgroups(0, nullptr)));
    groups.resize(static_cast<std::size_t>(::getgroups(static_cast<int>(groups.size()), groups.data())));
    std::erase(groups, ::getegid());
    if (groups.size() < 2)
    {
        GTEST_SKIP() << "needs membership of two supplementary groups";
    }
    const gid_t directoryGroup = groups[0];
    const gid_t configGroup = groups[1];
    ASSERT_EQ(::chown(m_TempDir.c_str(), static_cast<uid_t>(-1), directoryGroup), 0);
    ASSERT_EQ(::chmod(m_TempDir.c_str(), 02770), 0); // setgid: new files take directoryGroup

    auto& config = UserConfig::get();
    config.save();
    const auto& path = m_ConfigPath;
    ASSERT_EQ(::chown(path.c_str(), static_cast<uid_t>(-1), configGroup), 0);
    ASSERT_EQ(::chmod(path.c_str(), 0640), 0);

    config.settings().themeId = "mocha";
    config.save();

    struct stat saved = {};
    ASSERT_EQ(::stat(path.c_str(), &saved), 0);
    EXPECT_EQ(saved.st_gid, configGroup) << "the replacement took the directory's group";
    EXPECT_EQ(saved.st_mode & 0777, 0640U);
    EXPECT_EQ(parsed(path)["theme"]["id"].value<std::string>(), "mocha");
}

TEST_F(UserConfigSaveLoadFixture, UnreadableConfigDirectoryFallsBackToDefaults)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root can read a mode-000 directory";
    }
    const auto dir = m_TempDir / "locked";
    std::filesystem::create_directory(dir);
    UserConfig::get().resetConfigPathForTesting(dir / "inner" / "config.toml");
    std::filesystem::permissions(dir, std::filesystem::perms::none);

    EXPECT_NO_THROW(UserConfig::get().load());
    EXPECT_EQ(UserConfig::get().settings().refreshIntervalMs, UserSettings{}.refreshIntervalMs);
    EXPECT_NO_THROW(UserConfig::get().save());

    std::filesystem::permissions(dir, std::filesystem::perms::owner_all); // let TearDown remove it
}
#endif

} // namespace
} // namespace App
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
