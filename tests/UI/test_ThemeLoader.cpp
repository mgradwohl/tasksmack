#include "UI/ThemeLoader.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

namespace UI
{
namespace
{

// Helper to compare ImVec4 with tolerance
void expectColorNear(const ImVec4& actual, const ImVec4& expected, float tolerance = 0.01F)
{
    EXPECT_NEAR(actual.x, expected.x, tolerance) << "Red channel mismatch";
    EXPECT_NEAR(actual.y, expected.y, tolerance) << "Green channel mismatch";
    EXPECT_NEAR(actual.z, expected.z, tolerance) << "Blue channel mismatch";
    EXPECT_NEAR(actual.w, expected.w, tolerance) << "Alpha channel mismatch";
}

// ========== hexToImVec4 Tests ==========

TEST(ThemeLoaderTest, HexToImVec4_ValidSixDigit)
{
    // Pure red
    auto red = ThemeLoader::hexToImVec4("#FF0000");
    expectColorNear(red, ImVec4(1.0F, 0.0F, 0.0F, 1.0F));

    // Pure green
    auto green = ThemeLoader::hexToImVec4("#00FF00");
    expectColorNear(green, ImVec4(0.0F, 1.0F, 0.0F, 1.0F));

    // Pure blue
    auto blue = ThemeLoader::hexToImVec4("#0000FF");
    expectColorNear(blue, ImVec4(0.0F, 0.0F, 1.0F, 1.0F));

    // White
    auto white = ThemeLoader::hexToImVec4("#FFFFFF");
    expectColorNear(white, ImVec4(1.0F, 1.0F, 1.0F, 1.0F));

    // Black
    auto black = ThemeLoader::hexToImVec4("#000000");
    expectColorNear(black, ImVec4(0.0F, 0.0F, 0.0F, 1.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_ValidEightDigit)
{
    // Red with 50% alpha (80 hex = 128 decimal = ~0.502)
    auto redHalfAlpha = ThemeLoader::hexToImVec4("#FF000080");
    expectColorNear(redHalfAlpha, ImVec4(1.0F, 0.0F, 0.0F, 128.0F / 255.0F));

    // Fully transparent
    auto transparent = ThemeLoader::hexToImVec4("#FFFFFF00");
    expectColorNear(transparent, ImVec4(1.0F, 1.0F, 1.0F, 0.0F));

    // Fully opaque (explicit FF alpha)
    auto opaque = ThemeLoader::hexToImVec4("#000000FF");
    expectColorNear(opaque, ImVec4(0.0F, 0.0F, 0.0F, 1.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_WithoutHashPrefix)
{
    // Should work without # prefix
    auto red = ThemeLoader::hexToImVec4("FF0000");
    expectColorNear(red, ImVec4(1.0F, 0.0F, 0.0F, 1.0F));

    auto withAlpha = ThemeLoader::hexToImVec4("00FF0080");
    expectColorNear(withAlpha, ImVec4(0.0F, 1.0F, 0.0F, 128.0F / 255.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_LowercaseHex)
{
    auto lower = ThemeLoader::hexToImVec4("#ff8040");
    auto upper = ThemeLoader::hexToImVec4("#FF8040");
    expectColorNear(lower, upper);
}

TEST(ThemeLoaderTest, HexToImVec4_MixedCaseHex)
{
    auto mixed = ThemeLoader::hexToImVec4("#Ff80aB");
    // 0xFF = 255, 0x80 = 128, 0xAB = 171
    expectColorNear(mixed, ImVec4(1.0F, 128.0F / 255.0F, 171.0F / 255.0F, 1.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_GrayValues)
{
    // Test some gray values
    auto gray50 = ThemeLoader::hexToImVec4("#808080");
    expectColorNear(gray50, ImVec4(128.0F / 255.0F, 128.0F / 255.0F, 128.0F / 255.0F, 1.0F));

    auto gray25 = ThemeLoader::hexToImVec4("#404040");
    expectColorNear(gray25, ImVec4(64.0F / 255.0F, 64.0F / 255.0F, 64.0F / 255.0F, 1.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_InvalidLength_ReturnsMagenta)
{
    // Too short
    auto tooShort = ThemeLoader::hexToImVec4("#FFF");
    expectColorNear(tooShort, ImVec4(1.0F, 0.0F, 1.0F, 1.0F)); // Magenta error color

    // Too long
    auto tooLong = ThemeLoader::hexToImVec4("#FFFFFFFFFF");
    expectColorNear(tooLong, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));

    // 7 digits (between 6 and 8)
    auto seven = ThemeLoader::hexToImVec4("#FFFFFFF");
    expectColorNear(seven, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));

    // 5 digits
    auto five = ThemeLoader::hexToImVec4("#FFFFF");
    expectColorNear(five, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_InvalidCharacters_ReturnsMagenta)
{
    // Contains non-hex characters
    auto invalid1 = ThemeLoader::hexToImVec4("#GGGGGG");
    expectColorNear(invalid1, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));

    auto invalid2 = ThemeLoader::hexToImVec4("#XY1234");
    expectColorNear(invalid2, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));

    // Space in string
    auto withSpace = ThemeLoader::hexToImVec4("#FF 000");
    expectColorNear(withSpace, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_EmptyString_ReturnsMagenta)
{
    auto empty = ThemeLoader::hexToImVec4("");
    expectColorNear(empty, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));

    auto justHash = ThemeLoader::hexToImVec4("#");
    expectColorNear(justHash, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_InvalidAlpha_ReturnsMagenta)
{
    // Valid RGB but invalid alpha characters
    auto badAlpha = ThemeLoader::hexToImVec4("#FFFFFFGG");
    expectColorNear(badAlpha, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_BoundaryValues)
{
    // Minimum values
    auto min = ThemeLoader::hexToImVec4("#000000");
    expectColorNear(min, ImVec4(0.0F, 0.0F, 0.0F, 1.0F));

    // Maximum values
    auto max = ThemeLoader::hexToImVec4("#FFFFFF");
    expectColorNear(max, ImVec4(1.0F, 1.0F, 1.0F, 1.0F));

    // Single increment from zero
    auto oneStep = ThemeLoader::hexToImVec4("#010101");
    expectColorNear(oneStep, ImVec4(1.0F / 255.0F, 1.0F / 255.0F, 1.0F / 255.0F, 1.0F));

    // Single decrement from max
    auto almostMax = ThemeLoader::hexToImVec4("#FEFEFE");
    expectColorNear(almostMax, ImVec4(254.0F / 255.0F, 254.0F / 255.0F, 254.0F / 255.0F, 1.0F));
}

TEST(ThemeLoaderTest, HexToImVec4_CommonUIColors)
{
    // Material Design colors
    auto materialRed = ThemeLoader::hexToImVec4("#F44336");
    EXPECT_GT(materialRed.x, 0.9F); // Red-ish
    EXPECT_LT(materialRed.y, 0.3F);
    EXPECT_LT(materialRed.z, 0.3F);

    auto materialBlue = ThemeLoader::hexToImVec4("#2196F3");
    EXPECT_LT(materialBlue.x, 0.2F);
    EXPECT_GT(materialBlue.y, 0.5F);
    EXPECT_GT(materialBlue.z, 0.9F); // Blue-ish
}

// ========== discoverThemes Tests ==========

class ThemeLoaderDiscoveryTest : public ::testing::Test
{
  protected:
    std::filesystem::path m_TempDir;

    void SetUp() override
    {
        // Create a temporary directory for test themes
        m_TempDir = std::filesystem::temp_directory_path() / "tasksmack_theme_test";
        std::filesystem::create_directories(m_TempDir);
    }

    void TearDown() override
    {
        // Clean up temporary directory
        std::filesystem::remove_all(m_TempDir);
    }

    void createThemeFile(const std::string& filename, const std::string& content)
    {
        std::ofstream file(m_TempDir / filename);
        file << content;
    }
};

TEST_F(ThemeLoaderDiscoveryTest, DiscoverThemes_EmptyDirectory)
{
    auto themes = ThemeLoader::discoverThemes(m_TempDir);
    EXPECT_TRUE(themes.empty());
}

TEST_F(ThemeLoaderDiscoveryTest, DiscoverThemes_NonExistentDirectory)
{
    auto themes = ThemeLoader::discoverThemes(m_TempDir / "nonexistent");
    EXPECT_TRUE(themes.empty());
}

TEST_F(ThemeLoaderDiscoveryTest, DiscoverThemes_ValidThemeFile)
{
    createThemeFile("test-theme.toml", R"(
[meta]
name = "Test Theme"
description = "A test theme"

[colors]
windowBg = "#1E1E1E"
)");

    auto themes = ThemeLoader::discoverThemes(m_TempDir);
    ASSERT_EQ(themes.size(), 1);
    EXPECT_EQ(themes[0].id, "test-theme");
    EXPECT_EQ(themes[0].name, "Test Theme");
    EXPECT_EQ(themes[0].description, "A test theme");
}

TEST_F(ThemeLoaderDiscoveryTest, DiscoverThemes_MultipleThemes)
{
    createThemeFile("dark.toml", R"(
[meta]
name = "Dark Theme"
description = "Dark colors"

[colors]
windowBg = "#1E1E1E"
)");

    createThemeFile("light.toml", R"(
[meta]
name = "Light Theme"
description = "Light colors"

[colors]
windowBg = "#FFFFFF"
)");

    auto themes = ThemeLoader::discoverThemes(m_TempDir);
    EXPECT_EQ(themes.size(), 2);

    // Check both themes are found (order may vary)
    bool foundDark = false;
    bool foundLight = false;
    for (const auto& theme : themes)
    {
        if (theme.id == "dark")
        {
            foundDark = true;
        }
        if (theme.id == "light")
        {
            foundLight = true;
        }
    }
    EXPECT_TRUE(foundDark);
    EXPECT_TRUE(foundLight);
}

TEST_F(ThemeLoaderDiscoveryTest, DiscoverThemes_IgnoresNonTomlFiles)
{
    createThemeFile("valid.toml", R"(
[meta]
name = "Valid"
description = "Valid theme"

[colors]
windowBg = "#1E1E1E"
)");

    createThemeFile("readme.txt", "This is not a theme file");
    createThemeFile("config.json", "{}");

    auto themes = ThemeLoader::discoverThemes(m_TempDir);
    EXPECT_EQ(themes.size(), 1);
    EXPECT_EQ(themes[0].id, "valid");
}

// ========== loadThemeInfo Tests ==========

TEST_F(ThemeLoaderDiscoveryTest, LoadThemeInfo_ValidFile)
{
    createThemeFile("info-test.toml", R"(
[meta]
name = "Info Test"
description = "Testing info loading"

[colors]
windowBg = "#1E1E1E"
)");

    auto info = ThemeLoader::loadThemeInfo(m_TempDir / "info-test.toml");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->id, "info-test");
    EXPECT_EQ(info->name, "Info Test");
    EXPECT_EQ(info->description, "Testing info loading");
}

TEST_F(ThemeLoaderDiscoveryTest, LoadThemeInfo_MissingMetaSection)
{
    createThemeFile("no-meta.toml", R"(
[colors]
windowBg = "#1E1E1E"
)");

    auto info = ThemeLoader::loadThemeInfo(m_TempDir / "no-meta.toml");
    // Should still return info with filename as id
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->id, "no-meta");
}

TEST_F(ThemeLoaderDiscoveryTest, LoadThemeInfo_NonExistentFile)
{
    auto info = ThemeLoader::loadThemeInfo(m_TempDir / "does-not-exist.toml");
    EXPECT_FALSE(info.has_value());
}

TEST_F(ThemeLoaderDiscoveryTest, LoadThemeInfo_InvalidToml)
{
    createThemeFile("invalid.toml", R"(
[meta
name = "Broken"
)");

    auto info = ThemeLoader::loadThemeInfo(m_TempDir / "invalid.toml");
    EXPECT_FALSE(info.has_value());
}

// ========== loadTheme Tests ==========

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_ValidFile)
{
    // Use actual TOML structure that ThemeLoader expects
    createThemeFile("full-theme.toml", R"(
[meta]
name = "Full Theme"
description = "A complete theme"

[accents]
colors = ["#0078D4", "#E74856", "#10893E", "#8E8CD8", "#F7630C", "#00B7C3", "#FFB900", "#E3008C"]

[progress]
low = "#00FF00"
medium = "#FFFF00"
high = "#FF0000"

[semantic]
text_primary = "#FFFFFF"
text_disabled = "#808080"
text_muted = "#CCCCCC"
text_error = "#FF0000"
text_warning = "#FFA500"
text_success = "#00FF00"
text_info = "#00FFFF"

[status]
running = "#00FF00"
sleeping = "#0000FF"
disk_sleep = "#FFA500"
zombie = "#FF0000"
stopped = "#FF00FF"
idle = "#808080"

[charts]
cpu = "#0078D4"
memory = "#10893E"
io = "#E74856"

[cpu_breakdown]
user = "#0078D4"
system = "#E74856"
iowait = "#FFB900"
idle = "#808080"

[charts.gpu]
utilization = "#0078D4"
memory = "#10893E"
temperature = "#E74856"
power = "#FFB900"
encoder = "#00B7C3"
decoder = "#8E8CD8"
clock = "#E3008C"
fan = "#808080"

[buttons.success]
normal = "#10893E"
hovered = "#2AA84E"
active = "#0A6B2E"

[ui.window]
background = "#1E1E1E"
child_background = "#252526"
popup_background = "#2D2D30"
border = "#3F3F46"

[ui.frame]
background = "#333337"
background_hovered = "#3E3E42"
background_active = "#0078D4"

[ui.title]
background = "#2D2D30"
background_active = "#0078D4"
background_collapsed = "#3F3F46"

[ui.bars]
menu = "#2D2D30"
status = "#2D2D30"

[ui.scrollbar]
background = "#1E1E1E"
grab = "#5A5A5A"
grab_hovered = "#808080"
grab_active = "#0078D4"

[ui.controls]
check_mark = "#FFFFFF"
slider_grab = "#5A5A5A"
slider_grab_active = "#0078D4"

[ui.button]
normal = "#333337"
hovered = "#3E3E42"
active = "#0078D4"

[ui.header]
normal = "#333337"
hovered = "#3E3E42"
active = "#0078D4"

[ui.separator]
normal = "#3F3F46"
hovered = "#5A5A5A"
active = "#0078D4"

[ui.resize_grip]
normal = "#3F3F46"
hovered = "#5A5A5A"
active = "#0078D4"

[ui.tab]
normal = "#2D2D30"
hovered = "#3E3E42"
active = "#0078D4"
active_overline = "#FFFFFF"
unfocused = "#252526"
unfocused_active = "#3F3F46"
unfocused_active_overline = "#808080"

[ui.docking]
preview = "#0078D480"
empty_background = "#1E1E1E"

[ui.plot]
lines = "#0078D4"
lines_hovered = "#60CDFF"
histogram = "#10893E"
histogram_hovered = "#6CCB5F"

[ui.table]
header_background = "#333337"
border_strong = "#3F3F46"
border_light = "#2D2D30"
row_background = "#00000000"
row_background_alt = "#FFFFFF0D"

[ui.misc]
text_selected_background = "#0078D480"
drag_drop_target = "#FFB900"
nav_highlight = "#0078D4"
nav_windowing_highlight = "#FFFFFFB3"
nav_windowing_dim_background = "#0000004D"
modal_window_dim_background = "#0000004D"
)");

    auto theme = ThemeLoader::loadTheme(m_TempDir / "full-theme.toml");
    ASSERT_TRUE(theme.has_value());
    EXPECT_EQ(theme->name, "Full Theme");

    // Check that semantic text_primary was parsed correctly
    expectColorNear(theme->textPrimary, ImVec4(1.0F, 1.0F, 1.0F, 1.0F));

    // Check progress colors
    expectColorNear(theme->progressLow, ImVec4(0.0F, 1.0F, 0.0F, 1.0F));    // #00FF00
    expectColorNear(theme->progressMedium, ImVec4(1.0F, 1.0F, 0.0F, 1.0F)); // #FFFF00
    expectColorNear(theme->progressHigh, ImVec4(1.0F, 0.0F, 0.0F, 1.0F));   // #FF0000
}

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_NonExistentFile)
{
    auto theme = ThemeLoader::loadTheme(m_TempDir / "does-not-exist.toml");
    EXPECT_FALSE(theme.has_value());
}

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_InvalidToml)
{
    createThemeFile("broken.toml", "this is not valid toml {{{");

    auto theme = ThemeLoader::loadTheme(m_TempDir / "broken.toml");
    EXPECT_FALSE(theme.has_value());
}

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_ArrayColorFormat)
{
    // Note: ThemeLoader primarily uses hex strings, but let's test that
    // it at least loads a valid theme file. Array format testing would
    // require understanding how the parser handles inline tables/arrays.
    // For now, just verify the loader handles a complete theme file.
    createThemeFile("array-colors.toml", R"(
[meta]
name = "Array Colors"

[accents]
colors = ["#0078D4", "#E74856", "#10893E", "#8E8CD8", "#F7630C", "#00B7C3", "#FFB900", "#E3008C"]

[progress]
low = "#10893E"
medium = "#FFB900"
high = "#E74856"

[semantic]
text_primary = "#FFFFFF"
text_disabled = "#808080"
text_muted = "#CCCCCC"
text_error = "#FF0000"
text_warning = "#FFA500"
text_success = "#00FF00"
text_info = "#00FFFF"

[status]
running = "#00FF00"
sleeping = "#0000FF"
disk_sleep = "#FFA500"
zombie = "#FF0000"
stopped = "#FF00FF"
idle = "#808080"

[charts]
cpu = "#0078D4"
memory = "#10893E"
io = "#E74856"

[cpu_breakdown]
user = "#0078D4"
system = "#E74856"
iowait = "#FFB900"
idle = "#808080"

[charts.gpu]
utilization = "#0078D4"
memory = "#10893E"
temperature = "#E74856"
power = "#FFB900"
encoder = "#00B7C3"
decoder = "#8E8CD8"
clock = "#E3008C"
fan = "#808080"

[buttons.success]
normal = "#10893E"
hovered = "#2AA84E"
active = "#0A6B2E"

[ui.window]
background = "#1E1E1E"
child_background = "#252526"
popup_background = "#2D2D30"
border = "#3F3F46"

[ui.frame]
background = "#333337"
background_hovered = "#3E3E42"
background_active = "#0078D4"

[ui.title]
background = "#2D2D30"
background_active = "#0078D4"
background_collapsed = "#3F3F46"

[ui.bars]
menu = "#2D2D30"
status = "#2D2D30"

[ui.scrollbar]
background = "#1E1E1E"
grab = "#5A5A5A"
grab_hovered = "#808080"
grab_active = "#0078D4"

[ui.controls]
check_mark = "#FFFFFF"
slider_grab = "#5A5A5A"
slider_grab_active = "#0078D4"

[ui.button]
normal = "#333337"
hovered = "#3E3E42"
active = "#0078D4"

[ui.header]
normal = "#333337"
hovered = "#3E3E42"
active = "#0078D4"

[ui.separator]
normal = "#3F3F46"
hovered = "#5A5A5A"
active = "#0078D4"

[ui.resize_grip]
normal = "#3F3F46"
hovered = "#5A5A5A"
active = "#0078D4"

[ui.tab]
normal = "#2D2D30"
hovered = "#3E3E42"
active = "#0078D4"
active_overline = "#FFFFFF"
unfocused = "#252526"
unfocused_active = "#3F3F46"
unfocused_active_overline = "#808080"

[ui.docking]
preview = "#0078D480"
empty_background = "#1E1E1E"

[ui.plot]
lines = "#0078D4"
lines_hovered = "#60CDFF"
histogram = "#10893E"
histogram_hovered = "#6CCB5F"

[ui.table]
header_background = "#333337"
border_strong = "#3F3F46"
border_light = "#2D2D30"
row_background = "#00000000"
row_background_alt = "#FFFFFF0D"

[ui.misc]
text_selected_background = "#0078D480"
drag_drop_target = "#FFB900"
nav_highlight = "#0078D4"
nav_windowing_highlight = "#FFFFFFB3"
nav_windowing_dim_background = "#0000004D"
modal_window_dim_background = "#0000004D"
)");

    auto theme = ThemeLoader::loadTheme(m_TempDir / "array-colors.toml");
    ASSERT_TRUE(theme.has_value());
    EXPECT_EQ(theme->name, "Array Colors");

    // Verify accent colors from the array
    expectColorNear(theme->accents[0], ImVec4(0x00 / 255.0F, 0x78 / 255.0F, 0xD4 / 255.0F, 1.0F)); // Windows Blue
}

} // namespace
} // namespace UI
