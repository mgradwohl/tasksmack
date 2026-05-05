#include "UI/ThemeLoader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

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

// Shared minimal-valid theme body (all required sections, no [meta]).
// Used by tests that need a complete theme without a meta section, or that
// prepend their own [meta] block on top of this body.
constexpr const char* k_FullThemeTomlBody = R"(
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
io_write = "#F7630C"

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
)";

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
        // Create a unique temporary directory per test to avoid conflicts
        // Use a combination of temp path + test name + time-based suffix
        auto basePath = std::filesystem::temp_directory_path() / "tasksmack_theme_test";
        auto testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
        auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string uniqueName = std::string(testInfo->test_suite_name()) + "_" + testInfo->name() + "_" + std::to_string(timestamp);
        m_TempDir = basePath / uniqueName;
        std::filesystem::create_directories(m_TempDir);
    }

    void TearDown() override
    {
        // Clean up temporary directory with retry logic for Windows
        // Windows may hold file handles briefly after close
        std::error_code errorCode;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            std::filesystem::remove_all(m_TempDir, errorCode);
            if (!errorCode)
            {
                break;
            }
            // Brief delay before retry on Windows
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Ignore final error - CI cleanup will handle any stragglers
    }

    void createThemeFile(const std::string& filename, const std::string& content)
    {
        std::ofstream file(m_TempDir / filename);
        file << content;
        // RAII handles flush and close automatically
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
    ASSERT_EQ(themes.size(), 1U);
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
    EXPECT_EQ(themes.size(), 2U);

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
    EXPECT_EQ(themes.size(), 1U);
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
io_write = "#F7630C"

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

    // Check I/O chart colors (read vs write must be distinct)
    // #E74856 = (231/255, 72/255, 86/255, 1.0)
    expectColorNear(theme->chartIo, ImVec4(0xE7 / 255.0F, 0x48 / 255.0F, 0x56 / 255.0F, 1.0F));
    // #F7630C = (247/255, 99/255, 12/255, 1.0)
    expectColorNear(theme->chartIoWrite, ImVec4(0xF7 / 255.0F, 0x63 / 255.0F, 0x0C / 255.0F, 1.0F));
    // Read and write must not be the same color
    EXPECT_FALSE(theme->chartIo.x == theme->chartIoWrite.x && theme->chartIo.y == theme->chartIoWrite.y &&
                 theme->chartIo.z == theme->chartIoWrite.z)
        << "I/O read and write chart colors must be visually distinct";
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

// ========== Missing Meta Section Tests ==========

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_MissingMetaSection_NameIsEmpty)
{
    // k_FullThemeTomlBody has all required sections but no [meta] section.
    // scheme.name should remain empty (default) since the meta block is absent.
    createThemeFile("no-meta-theme.toml", k_FullThemeTomlBody);

    auto theme = ThemeLoader::loadTheme(m_TempDir / "no-meta-theme.toml");
    ASSERT_TRUE(theme.has_value());
    // No [meta] section → name stays default-constructed (empty)
    EXPECT_TRUE(theme->name.empty());
    // Colors should still parse correctly from the rest of the file
    expectColorNear(theme->progressLow, ImVec4(0.0F, 1.0F, 0.0F, 1.0F));
}

// ========== Missing Required Color Keys Tests ==========

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_MissingRequiredColors_FallsBackToErrorColor)
{
    // A TOML file with [meta] and [charts] but missing required progress/semantic/status/etc.
    // Missing required keys (those without explicit C++ fallbacks) → errorColor (magenta).
    createThemeFile("sparse-theme.toml", R"(
[meta]
name = "Sparse Theme"

[accents]
colors = ["#0078D4", "#E74856", "#10893E", "#8E8CD8", "#F7630C", "#00B7C3", "#FFB900", "#E3008C"]

[charts]
cpu = "#0078D4"
memory = "#10893E"
io = "#E74856"
# io_write intentionally absent — falls back to chartMemory (#10893E)

[cpu_breakdown]
user = "#0078D4"
system = "#E74856"
iowait = "#FFB900"
idle = "#808080"
)");

    auto theme = ThemeLoader::loadTheme(m_TempDir / "sparse-theme.toml");
    ASSERT_TRUE(theme.has_value());
    EXPECT_EQ(theme->name, "Sparse Theme");

    // Required colors that are absent without a default should be errorColor (magenta)
    const ImVec4 magenta{1.0F, 0.0F, 1.0F, 1.0F};
    expectColorNear(theme->progressLow, magenta);
    expectColorNear(theme->progressMedium, magenta);
    expectColorNear(theme->progressHigh, magenta);

    // Optional colors with explicit C++ defaults should NOT be magenta
    // io_write falls back to chartMemory (#10893E = 16/255, 137/255, 62/255)
    expectColorNear(theme->chartIoWrite, theme->chartMemory);
}

// ========== Invalid Color Values Tests ==========

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_InvalidColorValue_FallsBackToErrorColor)
{
    // A theme file where one required color has an invalid hex string.
    // The invalid entry should produce magenta; valid entries should parse normally.
    createThemeFile("invalid-color-theme.toml", R"(
[meta]
name = "Invalid Color Theme"

[accents]
colors = ["#0078D4", "#E74856", "#10893E", "#8E8CD8", "#F7630C", "#00B7C3", "#FFB900", "#E3008C"]

[progress]
low = "not-a-color"
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
io_write = "#F7630C"

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

    auto theme = ThemeLoader::loadTheme(m_TempDir / "invalid-color-theme.toml");
    ASSERT_TRUE(theme.has_value());
    EXPECT_EQ(theme->name, "Invalid Color Theme");

    // The invalid color "not-a-color" should produce magenta error color
    expectColorNear(theme->progressLow, ImVec4(1.0F, 0.0F, 1.0F, 1.0F));

    // Valid entries should parse correctly
    expectColorNear(theme->progressMedium, ImVec4(1.0F, 1.0F, 0.0F, 1.0F)); // #FFFF00
    expectColorNear(theme->progressHigh, ImVec4(1.0F, 0.0F, 0.0F, 1.0F));   // #FF0000
}

// ========== Duplicate Theme Names Tests ==========

TEST_F(ThemeLoaderDiscoveryTest, DiscoverThemes_DuplicateNames_BothReturned)
{
    // Two theme files with the same display name but different filenames.
    // discoverThemes should find and return both.
    createThemeFile("theme-a.toml", R"(
[meta]
name = "Same Name"
description = "First theme"
)");

    createThemeFile("theme-b.toml", R"(
[meta]
name = "Same Name"
description = "Second theme"
)");

    auto themes = ThemeLoader::discoverThemes(m_TempDir);
    EXPECT_EQ(themes.size(), 2U);

    // Both IDs are distinct even though names are the same
    bool foundA = false;
    bool foundB = false;
    for (const auto& t : themes)
    {
        EXPECT_EQ(t.name, "Same Name");
        if (t.id == "theme-a")
        {
            foundA = true;
        }
        if (t.id == "theme-b")
        {
            foundB = true;
        }
    }
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);
}

// ========== Sort-Order Test ==========

TEST_F(ThemeLoaderDiscoveryTest, DiscoverThemes_SortedAlphabeticallyByName)
{
    createThemeFile("z-theme.toml", R"(
[meta]
name = "Zebra"
description = "Sorts last"
)");

    createThemeFile("a-theme.toml", R"(
[meta]
name = "Apple"
description = "Sorts first"
)");

    createThemeFile("m-theme.toml", R"(
[meta]
name = "Mango"
description = "Sorts middle"
)");

    auto themes = ThemeLoader::discoverThemes(m_TempDir);
    ASSERT_EQ(themes.size(), 3U);

    // discoverThemes sorts by name ascending
    EXPECT_EQ(themes[0].name, "Apple");
    EXPECT_EQ(themes[1].name, "Mango");
    EXPECT_EQ(themes[2].name, "Zebra");
}

// ========== I/O Write Color Tests ==========

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_IoWriteColor_FallsBackToMemoryWhenAbsent)
{
    // When io_write is not specified, chartIoWrite should fall back to chartMemory
    createThemeFile("no-io-write.toml", R"(
[meta]
name = "No IO Write"

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
# io_write intentionally absent — should fall back to chartMemory

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

    auto theme = ThemeLoader::loadTheme(m_TempDir / "no-io-write.toml");
    ASSERT_TRUE(theme.has_value());

    // chartIoWrite falls back to chartMemory (#10893E) when io_write is absent
    expectColorNear(theme->chartIoWrite, theme->chartMemory);
}

// ========== Network Chart Color Tests ==========

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_NetTxRx_ParsedWhenPresent)
{
    // Standalone minimal TOML — avoids duplicate [charts] error that would occur
    // if we appended a second [charts] block after k_FullThemeTomlBody.
    createThemeFile("net-colors.toml", R"(
[meta]
name = "Net Color Theme"
description = "Tests net_tx/net_rx parsing"

[charts]
cpu = "#0078D4"
memory = "#10893E"
io = "#E74856"
net_tx = "#FFC107"
net_rx = "#AB47BC"
)");

    auto theme = ThemeLoader::loadTheme(m_TempDir / "net-colors.toml");
    ASSERT_TRUE(theme.has_value());

    // #FFC107 = (255/255, 193/255, 7/255)
    expectColorNear(theme->chartNetTx, ImVec4(0xFF / 255.0F, 0xC1 / 255.0F, 0x07 / 255.0F, 1.0F));
    // #AB47BC = (171/255, 71/255, 188/255)
    expectColorNear(theme->chartNetRx, ImVec4(0xAB / 255.0F, 0x47 / 255.0F, 0xBC / 255.0F, 1.0F));

    // net_tx and net_rx must be distinct from each other (epsilon-based, all 4 channels)
    constexpr float k_DistinctTolerance = 0.01F;
    const bool sameColor = (std::abs(theme->chartNetTx.x - theme->chartNetRx.x) < k_DistinctTolerance &&
                            std::abs(theme->chartNetTx.y - theme->chartNetRx.y) < k_DistinctTolerance &&
                            std::abs(theme->chartNetTx.z - theme->chartNetRx.z) < k_DistinctTolerance &&
                            std::abs(theme->chartNetTx.w - theme->chartNetRx.w) < k_DistinctTolerance);
    EXPECT_FALSE(sameColor) << "chartNetTx and chartNetRx must be visually distinct";
}

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_NetTxRx_FallsBackToCpuMemoryWhenAbsent)
{
    // k_FullThemeTomlBody has no net_tx/net_rx entries.
    // chartNetTx should fall back to chartCpu; chartNetRx to chartMemory.
    const std::string toml = std::string("[meta]\nname = \"Fallback Net\"\n\n") + k_FullThemeTomlBody;
    createThemeFile("fallback-net.toml", toml);

    auto theme = ThemeLoader::loadTheme(m_TempDir / "fallback-net.toml");
    ASSERT_TRUE(theme.has_value());

    // When absent, net_tx falls back to chartCpu (#0078D4)
    expectColorNear(theme->chartNetTx, theme->chartCpu);
    // When absent, net_rx falls back to chartMemory (#10893E)
    expectColorNear(theme->chartNetRx, theme->chartMemory);
}

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_NetTxRxFill_FallsBackToLineColorWhenAbsent)
{
    // Provide net_tx/net_rx line colors but omit the fill variants.
    // chartNetTxFill should fall back to chartNetTx; chartNetRxFill to chartNetRx.
    // Use a standalone TOML — appending a second [charts] block after k_FullThemeTomlBody
    // would cause a toml++ duplicate-table parse error.
    createThemeFile("net-fill-fallback.toml", R"(
[meta]
name = "Net Fill Fallback"

[charts]
cpu = "#0078D4"
memory = "#10893E"
io = "#E74856"
net_tx = "#FFC107"
net_rx = "#AB47BC"
# net_tx_fill and net_rx_fill intentionally absent — should fall back to line colors
)");

    auto theme = ThemeLoader::loadTheme(m_TempDir / "net-fill-fallback.toml");
    ASSERT_TRUE(theme.has_value());

    // Fill falls back to the line color (alpha may differ from a real 0.3-alpha fill,
    // but the fallback path in ThemeLoader passes the line color as-is)
    expectColorNear(theme->chartNetTxFill, theme->chartNetTx);
    expectColorNear(theme->chartNetRxFill, theme->chartNetRx);
}

// ========== Priority Badge Text Color Tests ==========

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_PriorityBadgeTextColor_ParsedWhenPresent)
{
    const std::string toml = std::string("[meta]\nname = \"Badge Color\"\n\n") + k_FullThemeTomlBody + R"(
[priority]
high   = "#E74856"
normal = "#10893E"
low    = "#8E8CD8"
badge_text_color = "#1A1A1A"
)";
    createThemeFile("badge-color.toml", toml);

    auto theme = ThemeLoader::loadTheme(m_TempDir / "badge-color.toml");
    ASSERT_TRUE(theme.has_value());

    // #1A1A1A = (26/255, 26/255, 26/255) — near-black for light themes
    expectColorNear(theme->priorityBadgeTextColor, ImVec4(0x1A / 255.0F, 0x1A / 255.0F, 0x1A / 255.0F, 1.0F));
}

TEST_F(ThemeLoaderDiscoveryTest, LoadTheme_PriorityBadgeTextColor_DefaultsToWhiteWhenAbsent)
{
    // k_FullThemeTomlBody has no [priority] section, so badge_text_color is absent.
    // It should default to white (1,1,1,1).
    const std::string toml = std::string("[meta]\nname = \"No Badge Color\"\n\n") + k_FullThemeTomlBody;
    createThemeFile("no-badge-color.toml", toml);

    auto theme = ThemeLoader::loadTheme(m_TempDir / "no-badge-color.toml");
    ASSERT_TRUE(theme.has_value());

    // Default badge text color must be fully opaque white
    expectColorNear(theme->priorityBadgeTextColor, ImVec4(1.0F, 1.0F, 1.0F, 1.0F));
}

} // namespace
} // namespace UI
