/// @file test_WindowsPackageLogo.cpp
/// @brief A packaged app's logo read from disk (#1800): WindowsProcessIconSource, given the logo a
/// manifest names (a .png path), reads the variant WindowsPackageLogoMath.h picks from the files beside
/// it and decodes it with stb_image -- in a non-ASCII folder, as packages and profiles can be named --
/// and reads nothing when the folder holds no variant it draws or doesn't exist.

#include "Platform/IProcessIconSource.h"
#include "Platform/Windows/WindowsProcessIconSource.h"
#include "TestPng.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

namespace Platform
{
namespace
{

/// @p path as UTF-8, as the startup probe hands paths over.
[[nodiscard]] std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

class WindowsPackageLogoTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() / std::format("tasksmack-1800-logo-{}", GetCurrentProcessId());
        std::error_code ignored;
        std::filesystem::remove_all(m_Root, ignored);
        // "Assets" under a folder named in Greek: the path must reach the decoder as UTF-8, not ANSI.
        m_Assets = m_Root / std::filesystem::path(u8"πακέτο") / "Assets";
        std::filesystem::create_directories(m_Assets);
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_Root, ignored);
    }

    void write(const char* name, int side, std::uint8_t red) const
    {
        TestSupport::writeBinaryFile(m_Assets / name, TestSupport::makeSolidPng(side, red, 0, 0, 255));
    }

    [[nodiscard]] std::string logo() const
    {
        return utf8(m_Assets / "Square44x44Logo.png");
    }

    std::filesystem::path m_Root;
    std::filesystem::path m_Assets;
};

TEST_F(WindowsPackageLogoTest, ReadsTheVariantNearestTheSize)
{
    write("Square44x44Logo.scale-100.png", 4, 10);                           // 44 px
    write("Square44x44Logo.targetsize-16_altform-unplated.png", 3, 20);      // 16 px, unplated
    write("Square44x44Logo.targetsize-16.png", 2, 30);                       // 16 px, plated
    write("Square44x44Logo.targetsize-16_contrast-black.png", 5, 40);        // high contrast
    write("Square44x44Logo.targetsize-16_altform-lightunplated.png", 6, 50); // light theme

    const IconPixels small = WindowsProcessIconSource::loadPackageLogo(logo(), 16).value_or(IconPixels{});
    ASSERT_EQ(small.width, 3); // The unplated 16 px variant
    ASSERT_FALSE(small.rgba.empty());
    EXPECT_EQ(small.rgba[0], 20);

    const IconPixels large = WindowsProcessIconSource::loadPackageLogo(logo(), 32).value_or(IconPixels{});
    ASSERT_EQ(large.width, 4); // scale-100: 44 px, the only variant at least 32
    ASSERT_FALSE(large.rgba.empty());
    EXPECT_EQ(large.rgba[0], 10);
}

TEST_F(WindowsPackageLogoTest, TheIconSourceReadsAPngPathAsALogo)
{
    write("Square44x44Logo.targetsize-24_altform-unplated.png", 3, 60);
    WindowsProcessIconSource source;
    const IconPixels icon = source.loadIcon(logo(), "Widget", 24).value_or(IconPixels{});
    EXPECT_EQ(icon.width, 3);
    EXPECT_EQ(icon.height, 3);
    ASSERT_EQ(icon.rgba.size(), 3U * 3U * 4U);
    EXPECT_EQ(icon.rgba[0], 60);
}

TEST_F(WindowsPackageLogoTest, NoVariantItDrawsIsNoIcon)
{
    write("Square44x44Logo.targetsize-16_contrast-black.png", 3, 70);
    write("SplashScreen.scale-100.png", 3, 80);
    EXPECT_FALSE(WindowsProcessIconSource::loadPackageLogo(logo(), 16).has_value());
}

TEST_F(WindowsPackageLogoTest, AMissingFolderOrUndecodableFileIsNoIcon)
{
    EXPECT_FALSE(WindowsProcessIconSource::loadPackageLogo(utf8(m_Root / "missing" / "Square44x44Logo.png"), 16).has_value());
    EXPECT_FALSE(WindowsProcessIconSource::loadPackageLogo("", 16).has_value());
    EXPECT_FALSE(WindowsProcessIconSource::loadPackageLogo("Square44x44Logo.png", 16).has_value()); // No folder

    TestSupport::writeBinaryFile(m_Assets / "Square44x44Logo.scale-100.png", {'n', 'o', 't', ' ', 'p', 'n', 'g'});
    EXPECT_FALSE(WindowsProcessIconSource::loadPackageLogo(logo(), 16).has_value());
}

} // namespace
} // namespace Platform
