/// @file test_WindowOverride.cpp
/// @brief App::WindowOverride (#1453): the TASKSMACK_WINDOW parser, and the decision not to save the
/// window geometry while it is set -- down to the config file's [window] section staying byte for byte
/// what it was.

#include "App/UserConfig.h"
#include "App/WindowOverride.h"
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <system_error>

namespace
{

using App::WindowOverride::CapturedWindow;
using App::WindowOverride::Geometry;
using App::WindowOverride::parse;

// ========== Parser ==========

TEST(WindowOverrideParseTest, UnsetEmptyAndOffWordsGiveNoOverrideAndNoWarning)
{
    for (const char* value : {static_cast<const char*>(nullptr), "", "   ", "0", "off", "OFF", "false", "no"})
    {
        SCOPED_TRACE(value == nullptr ? "(nullptr)" : value);
        const auto result = parse(value);
        EXPECT_FALSE(result.geometry.has_value());
        EXPECT_TRUE(result.warning.empty()) << result.warning;
    }
}

TEST(WindowOverrideParseTest, ValidSizesParseWithoutWarning)
{
    struct Case
    {
        const char* value = nullptr;
        Geometry expected;
    };
    for (const Case& c : {
             Case{.value = "1600x900", .expected = {.width = 1600, .height = 900, .maximized = false}},
             Case{.value = "1600X900", .expected = {.width = 1600, .height = 900, .maximized = false}},
             Case{.value = " 1280 x 720 ", .expected = {.width = 1280, .height = 720, .maximized = false}},
             Case{.value = "200x16384", .expected = {.width = 200, .height = 16384, .maximized = false}},
             Case{.value = "0800x0600", .expected = {.width = 800, .height = 600, .maximized = false}},
         })
    {
        SCOPED_TRACE(c.value);
        const auto result = parse(c.value);
        EXPECT_EQ(result.geometry, std::optional<Geometry>{c.expected});
        EXPECT_TRUE(result.warning.empty()) << result.warning;
    }
}

TEST(WindowOverrideParseTest, MaximizedFlag)
{
    for (const char* value : {"1600x900,maximized", "1600x900, maximized", "1600x900,MAXIMIZED", "1600x900,maximised"})
    {
        SCOPED_TRACE(value);
        const auto result = parse(value);
        EXPECT_EQ(result.geometry, (std::optional<Geometry>{Geometry{.width = 1600, .height = 900, .maximized = true}}));
        EXPECT_TRUE(result.warning.empty()) << result.warning;
    }
}

TEST(WindowOverrideParseTest, InvalidValuesAreIgnoredWithAWarning)
{
    for (const char* value : {"1600",
                              "1600x",
                              "x900",
                              "1600*900",
                              "-1600x900",
                              "+1600x900",
                              "1600x900x2",
                              "1600.5x900",
                              "1600x900,",
                              "1600x900,max",
                              "1600x900,maximized,maximized",
                              "1600x900;maximized",
                              "maximized",
                              "on",
                              "abc"})
    {
        SCOPED_TRACE(value);
        const auto result = parse(value);
        EXPECT_FALSE(result.geometry.has_value());
        EXPECT_FALSE(result.warning.empty());
        EXPECT_NE(result.warning.find("ignored"), std::string::npos) << result.warning;
    }
}

TEST(WindowOverrideParseTest, SizesOutsideTheWindowLimitsAreClampedWithAWarning)
{
    struct Case
    {
        const char* value = nullptr;
        int width = 0;
        int height = 0;
    };
    for (const Case& c : {
             Case{.value = "100x900", .width = Core::WINDOW_MIN_DIMENSION, .height = 900},
             Case{.value = "0x0", .width = Core::WINDOW_MIN_DIMENSION, .height = Core::WINDOW_MIN_DIMENSION},
             Case{.value = "1600x20000", .width = 1600, .height = Core::WINDOW_MAX_DIMENSION},
             // Past int: saturates, then clamps like any other size that is too large.
             Case{.value = "99999999999999999999x900", .width = Core::WINDOW_MAX_DIMENSION, .height = 900},
         })
    {
        SCOPED_TRACE(c.value);
        const auto result = parse(c.value);
        EXPECT_EQ(result.geometry, (std::optional<Geometry>{Geometry{.width = c.width, .height = c.height, .maximized = false}}));
        EXPECT_NE(result.warning.find("clamped"), std::string::npos) << result.warning;
    }

    const auto maximized = parse("50x50,maximized");
    EXPECT_EQ(
        maximized.geometry,
        (std::optional<Geometry>{Geometry{.width = Core::WINDOW_MIN_DIMENSION, .height = Core::WINDOW_MIN_DIMENSION, .maximized = true}}));
}

TEST(WindowOverrideParseTest, Describe)
{
    EXPECT_EQ(App::WindowOverride::describe({.width = 1600, .height = 900, .maximized = false}), "1600x900, not maximized");
    EXPECT_EQ(App::WindowOverride::describe({.width = 1600, .height = 900, .maximized = true}), "1600x900, maximized");
}

// ========== The save decision ==========

/// A window that was moved, resized and maximized during the run.
[[nodiscard]] CapturedWindow movedAndMaximizedWindow()
{
    return CapturedWindow{
        .normal = Core::WindowGeometry::Rect{.x = 40, .y = 50, .width = 1600, .height = 900},
        .normalScale = 1.25F,
        .canPosition = true,
        .maximized = true,
    };
}

[[nodiscard]] App::UserSettings savedSettings()
{
    App::UserSettings settings;
    settings.windowWidth = 3000;
    settings.windowHeight = 1800;
    settings.windowScale = 1.0F;
    settings.windowPosX = 100;
    settings.windowPosY = 200;
    settings.windowMaximized = false;
    return settings;
}

void expectSameWindowSettings(const App::UserSettings& actual, const App::UserSettings& expected)
{
    EXPECT_EQ(actual.windowWidth, expected.windowWidth);
    EXPECT_EQ(actual.windowHeight, expected.windowHeight);
    EXPECT_EQ(actual.windowScale, expected.windowScale);
    EXPECT_EQ(actual.windowPosX, expected.windowPosX);
    EXPECT_EQ(actual.windowPosY, expected.windowPosY);
    EXPECT_EQ(actual.windowMaximized, expected.windowMaximized);
}

TEST(WindowOverrideSaveTest, GeometryIsSavedOnlyWithoutAnOverride)
{
    EXPECT_TRUE(App::WindowOverride::shouldSaveWindowGeometry(std::nullopt));
    EXPECT_FALSE(App::WindowOverride::shouldSaveWindowGeometry(Geometry{.width = 1600, .height = 900, .maximized = false}));
    EXPECT_FALSE(App::WindowOverride::shouldSaveWindowGeometry(Geometry{.width = 1600, .height = 900, .maximized = true}));
}

TEST(WindowOverrideSaveTest, WithoutAnOverrideTheLiveGeometryIsCaptured)
{
    App::UserSettings settings = savedSettings();
    App::WindowOverride::captureWindowGeometry(settings, movedAndMaximizedWindow(), std::nullopt);

    EXPECT_EQ(settings.windowWidth, 1600);
    EXPECT_EQ(settings.windowHeight, 900);
    EXPECT_EQ(settings.windowScale, std::optional<float>{1.25F});
    EXPECT_EQ(settings.windowPosX, std::optional<int>{40});
    EXPECT_EQ(settings.windowPosY, std::optional<int>{50});
    EXPECT_TRUE(settings.windowMaximized);
}

TEST(WindowOverrideSaveTest, WithoutAnOverrideAnUnknownNormalRectKeepsTheSavedOne)
{
    App::UserSettings settings = savedSettings();
    CapturedWindow captured = movedAndMaximizedWindow();
    captured.normal.reset();
    App::WindowOverride::captureWindowGeometry(settings, captured, std::nullopt);

    App::UserSettings expected = savedSettings();
    expected.windowMaximized = true; // the maximized state is still captured
    expectSameWindowSettings(settings, expected);
}

TEST(WindowOverrideSaveTest, WithoutPositioningOrScaleOnlyTheSizeAndStateAreCaptured)
{
    App::UserSettings settings = savedSettings();
    CapturedWindow captured = movedAndMaximizedWindow();
    captured.canPosition = false;
    captured.normalScale = 0.0F;
    App::WindowOverride::captureWindowGeometry(settings, captured, std::nullopt);

    EXPECT_EQ(settings.windowWidth, 1600);
    EXPECT_EQ(settings.windowHeight, 900);
    EXPECT_FALSE(settings.windowScale.has_value());
    EXPECT_EQ(settings.windowPosX, std::optional<int>{100});
    EXPECT_EQ(settings.windowPosY, std::optional<int>{200});
    EXPECT_TRUE(settings.windowMaximized);
}

TEST(WindowOverrideSaveTest, WithAnOverrideNoWindowSettingChanges)
{
    for (const bool maximized : {false, true})
    {
        SCOPED_TRACE(maximized ? "maximized override" : "not maximized override");
        App::UserSettings settings = savedSettings();
        App::WindowOverride::captureWindowGeometry(
            settings, movedAndMaximizedWindow(), Geometry{.width = 1600, .height = 900, .maximized = maximized});
        expectSameWindowSettings(settings, savedSettings());
    }
}

// ========== Through UserConfig::save(): the [window] section is untouched ==========

/// Points the UserConfig singleton at a config file in a fresh temp directory, and back afterwards.
class WindowOverrideConfigFileTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_OriginalPath = App::UserConfig::get().configPath();
        const auto base = std::filesystem::temp_directory_path() / "tasksmack_window_override_";
        std::random_device rd;
        constexpr int MAX_ATTEMPTS = 100;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
        {
            m_TempDir = base;
            m_TempDir += std::to_string(rd());
            std::error_code ec;
            if (std::filesystem::create_directory(m_TempDir, ec) && !ec)
            {
                break;
            }
            if (attempt == MAX_ATTEMPTS - 1)
            {
                FAIL() << "Failed to create a temp directory";
            }
        }
        m_ConfigPath = m_TempDir / "config.toml";
        App::UserConfig::get().resetConfigPathForTesting(m_ConfigPath);
    }

    void TearDown() override
    {
        App::UserConfig::get().resetConfigPathForTesting(m_OriginalPath);
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
    }

    /// The file as text. Text mode, like UserConfig::save() writes it: on Windows that writes "\r\n"
    /// line ends, and reading in text mode turns them back into "\n" for windowSection(). Only the
    /// line ends are translated, so comparing the read-back sections still compares every key, value
    /// and line of them.
    [[nodiscard]] std::string readConfigFile() const
    {
        std::ifstream file(m_ConfigPath);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    /// The file's "[window]" section: from its header line to the next section header (or the end).
    /// The header comment also mentions "[window]", so only a line that is exactly the header counts.
    [[nodiscard]] static std::string windowSection(const std::string& text)
    {
        const std::size_t header = text.find("\n[window]\n");
        if (header == std::string::npos)
        {
            return {};
        }
        const std::size_t start = header + 1;
        const std::size_t end = text.find("\n[", start);
        return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    /// Writes a config with savedSettings()' geometry, as a previous TaskSmack run would have, then
    /// loads it fresh. Returns the file's text.
    std::string writeSavedConfigAndLoad()
    {
        auto& config = App::UserConfig::get();
        config.settings() = savedSettings();
        config.save();
        std::string text = readConfigFile();
        config.resetConfigPathForTesting(m_ConfigPath);
        config.load();
        return text;
    }

    std::filesystem::path m_TempDir;
    std::filesystem::path m_ConfigPath;
    std::filesystem::path m_OriginalPath;
};

TEST_F(WindowOverrideConfigFileTest, WithAnOverrideTheWindowSectionIsByteIdenticalAndOtherSettingsSave)
{
    const std::string before = writeSavedConfigAndLoad();
    ASSERT_FALSE(windowSection(before).empty()) << before;

    // A measurement run: the window was opened at the override's size and maximized, and another
    // setting changed during the run.
    auto& config = App::UserConfig::get();
    config.settings().refreshIntervalMs = 500;
    App::WindowOverride::captureWindowGeometry(
        config.settings(), movedAndMaximizedWindow(), Geometry{.width = 1600, .height = 900, .maximized = true});
    config.save();

    const std::string after = readConfigFile();
    EXPECT_EQ(windowSection(after), windowSection(before));
    EXPECT_NE(windowSection(after).find("width = 3000"), std::string::npos) << after;
    EXPECT_NE(after.find("interval_ms = 500"), std::string::npos) << after;
}

TEST_F(WindowOverrideConfigFileTest, WithoutAnOverrideTheWindowSectionIsSaved)
{
    const std::string before = writeSavedConfigAndLoad();
    ASSERT_FALSE(windowSection(before).empty()) << before;

    auto& config = App::UserConfig::get();
    App::WindowOverride::captureWindowGeometry(config.settings(), movedAndMaximizedWindow(), std::nullopt);
    config.save();

    const std::string after = readConfigFile();
    EXPECT_NE(windowSection(after), windowSection(before));
    EXPECT_NE(windowSection(after).find("width = 1600"), std::string::npos) << after;
    EXPECT_NE(windowSection(after).find("maximized = true"), std::string::npos) << after;
}

} // namespace
