/// @file test_LinuxProcessIconSource.cpp
/// @brief LinuxProcessIconSource (#1800) against a temporary tree: a process's executable from its
/// /proc/[pid]/exe link (" (deleted)" dropped), and its icon end to end -- desktop entry, hicolor
/// theme, PNG decoded with stb_image -- for a PNG written by the test itself.

#include "Platform/IProcessIconSource.h"
#include "Platform/Linux/DesktopEntryIcons.h"
#include "Platform/Linux/LinuxProcessIconSource.h"
#include "ScopedTempDir.h"
#include "TestPng.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{
namespace
{

/// A 2x2 RGBA PNG of @p rgba (16 bytes).
[[nodiscard]] std::vector<std::uint8_t> makePng(const std::array<std::uint8_t, 16>& rgba)
{
    return TestSupport::makePng(2, 2, rgba);
}

constexpr std::array<std::uint8_t, 16> PIXELS{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 10, 20, 30, 40};

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    TestSupport::writeBinaryFile(path, bytes);
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

TEST(LinuxProcessIconSourceTest, DecodesAPng)
{
    const TestSupport::ScopedTempDir dir("tasksmack_icon_png");
    const std::filesystem::path png = dir.path / "icon.png";
    writeFile(png, makePng(PIXELS));
    const auto decoded = LinuxProcessIconSource::decodePng(png);
    ASSERT_TRUE(decoded.has_value());
    const IconPixels icon = decoded.value_or(IconPixels{});
    EXPECT_EQ(icon.width, 2);
    EXPECT_EQ(icon.height, 2);
    EXPECT_EQ(icon.rgba, std::vector<std::uint8_t>(PIXELS.begin(), PIXELS.end()));
}

TEST(LinuxProcessIconSourceTest, ANonPngIsNoIcon)
{
    const TestSupport::ScopedTempDir dir("tasksmack_icon_bad");
    const std::filesystem::path bad = dir.path / "icon.png";
    writeText(bad, "<svg/>");
    EXPECT_FALSE(LinuxProcessIconSource::decodePng(bad).has_value());
    EXPECT_FALSE(LinuxProcessIconSource::decodePng(dir.path / "missing.png").has_value());
}

// #4424: stb_image is built for PNG only, so a file named .png that holds another format is no icon, whatever the
// decoder would have made of it. A 1x1, 24-bit BMP, which a build with every decoder reads.
TEST(LinuxProcessIconSourceTest, ABmpNamedPngIsNoIcon)
{
    const TestSupport::ScopedTempDir dir("tasksmack_icon_bmp");
    const std::vector<std::uint8_t> bmp{
        'B', 'M', 58,   0,    0, 0, 0,    0,    0, 0, 54, 0, 0, 0,                                   // file header: 58 bytes, pixels at 54
        40,  0,   0,    0,    1, 0, 0,    0,    1, 0, 0,  0, 1, 0, 24, 0, 0, 0, 0,    0,    4,    0, // info header: 1x1, 1 plane, 24 bpp
        0,   0,   0x13, 0x0B, 0, 0, 0x13, 0x0B, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0xFF, 0x00, 0x00, 0x00, // one blue pixel, padded to 4 bytes
    };
    ASSERT_EQ(bmp.size(), 58U);
    writeFile(dir.path / "icon.png", bmp);
    EXPECT_FALSE(LinuxProcessIconSource::decodePng(dir.path / "icon.png").has_value());
}

// #4424: the side cap is pinned: a PNG one pixel wider than MAX_ICON_IMAGE_SIDE_PX is no icon, and one at it is.
TEST(LinuxProcessIconSourceTest, APngWiderThanTheCapIsNoIcon)
{
    const TestSupport::ScopedTempDir dir("tasksmack_icon_wide");
    const int side = MAX_ICON_IMAGE_SIDE_PX;
    const std::vector<std::uint8_t> tooWideRgba(static_cast<std::size_t>(side + 1) * 4, 0xFF);
    writeFile(dir.path / "wide.png", TestSupport::makePng(side + 1, 1, tooWideRgba));
    EXPECT_FALSE(LinuxProcessIconSource::decodePng(dir.path / "wide.png").has_value());
    const std::vector<std::uint8_t> atCapRgba(static_cast<std::size_t>(side) * 4, 0xFF);
    writeFile(dir.path / "cap.png", TestSupport::makePng(side, 1, atCapRgba));
    EXPECT_TRUE(LinuxProcessIconSource::decodePng(dir.path / "cap.png").has_value());
}

TEST(LinuxProcessIconSourceTest, FindsAProcesssIconThroughItsDesktopEntry)
{
    const TestSupport::ScopedTempDir dir("tasksmack_icon_tree");
    writeText(dir.path / "share/applications/app.desktop", "[Desktop Entry]\nType=Application\nExec=/opt/app/app %U\nIcon=app\n");
    writeFile(dir.path / "share/icons/hicolor/16x16/apps/app.png", makePng(PIXELS));

    DesktopEntryIcons::SearchDirs dirs;
    dirs.applications.push_back(dir.path / "share/applications");
    dirs.iconBases.push_back(dir.path / "share/icons");
    LinuxProcessIconSource source(dirs, dir.path / "proc");

    const auto byPath = source.loadIcon("/opt/app/app", "app", 16);
    ASSERT_TRUE(byPath.has_value());
    EXPECT_EQ(byPath.value_or(IconPixels{}).width, 2);
    const auto byName = source.loadIcon("", "app", 16); // Unreadable exe: the name alone
    EXPECT_TRUE(byName.has_value());
    EXPECT_FALSE(source.loadIcon("/usr/sbin/sshd", "sshd", 16).has_value());
    EXPECT_FALSE(source.loadIcon("/opt/app/app", "app", 0).has_value());
    EXPECT_FALSE(source.caseInsensitivePaths());
}

TEST(LinuxProcessIconSourceTest, ReadsTheExecutableLinkWithoutItsDeletedMark)
{
    const TestSupport::ScopedTempDir dir("tasksmack_icon_proc");
    std::filesystem::create_directories(dir.path / "proc/42");
    std::filesystem::create_symlink("/usr/bin/upgraded (deleted)", dir.path / "proc/42/exe");
    std::filesystem::create_directories(dir.path / "proc/43");
    std::filesystem::create_symlink("/usr/bin/app", dir.path / "proc/43/exe");

    LinuxProcessIconSource source(DesktopEntryIcons::SearchDirs{}, dir.path / "proc");
    EXPECT_EQ(source.executablePath(42, 0), "/usr/bin/upgraded");
    EXPECT_EQ(source.executablePath(43, 0), "/usr/bin/app");
    EXPECT_EQ(source.executablePath(44, 0), ""); // No such process
    EXPECT_EQ(source.executablePath(0, 0), "");
}

// #4192 (the Linux half of #4183): with the row's start time, a reused PID gives no path.
TEST(LinuxProcessIconSourceTest, AReusedPidGivesNoPathWhenTheStartTimeIsKnown)
{
    const TestSupport::ScopedTempDir dir("tasksmack_icon_proc_reuse");
    std::filesystem::create_directories(dir.path / "proc/42");
    std::filesystem::create_symlink("/usr/bin/newcomer", dir.path / "proc/42/exe");
    // Field 22, starttime, is 5000: the process now holding PID 42.
    {
        std::ofstream stat(dir.path / "proc/42/stat");
        stat << "42 (newcomer) S 1 42 42 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 5000 0 0\n";
    }
    LinuxProcessIconSource source(DesktopEntryIcons::SearchDirs{}, dir.path / "proc");
    EXPECT_EQ(source.executablePath(42, 5000), "/usr/bin/newcomer") << "the row's own process";
    EXPECT_EQ(source.executablePath(42, 4000), "") << "the row's process exited and its PID was reused";
    EXPECT_EQ(source.executablePath(42, 0), "/usr/bin/newcomer") << "an unknown start time reads by PID, as before";
    std::filesystem::remove(dir.path / "proc/42/stat");
    EXPECT_EQ(source.executablePath(42, 5000), "") << "no stat to confirm it: no path";
}

} // namespace
} // namespace Platform
