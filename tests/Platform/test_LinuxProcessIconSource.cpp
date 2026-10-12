/// @file test_LinuxProcessIconSource.cpp
/// @brief LinuxProcessIconSource (#1800) against a temporary tree: a process's executable from its
/// /proc/[pid]/exe link (" (deleted)" dropped), and its icon end to end -- desktop entry, hicolor
/// theme, PNG decoded with stb_image -- for a PNG written by the test itself.

#include "Platform/IProcessIconSource.h"
#include "Platform/Linux/DesktopEntryIcons.h"
#include "Platform/Linux/LinuxProcessIconSource.h"
#include "ScopedTempDir.h"

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

/// CRC-32 (PNG's chunk checksum).
[[nodiscard]] std::uint32_t crc32(const std::vector<std::uint8_t>& bytes)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t byte : bytes)
    {
        crc ^= byte;
        int bit = 0;
        while (bit < 8)
        {
            crc = (crc & 1U) != 0U ? (0xEDB88320U ^ (crc >> 1U)) : (crc >> 1U);
            ++bit;
        }
    }
    return ~crc;
}

void appendBigEndian(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

void appendChunk(std::vector<std::uint8_t>& png, std::string_view type, const std::vector<std::uint8_t>& data)
{
    appendBigEndian(png, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> typed(type.begin(), type.end());
    typed.insert(typed.end(), data.begin(), data.end());
    png.insert(png.end(), typed.begin(), typed.end());
    appendBigEndian(png, crc32(typed));
}

/// A 2x2 RGBA PNG of @p rgba (16 bytes), its pixels in one stored (uncompressed) deflate block.
[[nodiscard]] std::vector<std::uint8_t> makePng(const std::array<std::uint8_t, 16>& rgba)
{
    std::vector<std::uint8_t> raw;
    std::size_t row = 0;
    while (row < 2)
    {
        raw.push_back(0); // Filter: none
        raw.insert(
            raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(row * 8), rgba.begin() + static_cast<std::ptrdiff_t>((row + 1) * 8));
        ++row;
    }
    std::vector<std::uint8_t> zlib{0x78, 0x01, 0x01};
    const auto length = static_cast<std::uint16_t>(raw.size());
    zlib.push_back(static_cast<std::uint8_t>(length & 0xFFU));
    zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
    const auto inverted = static_cast<std::uint16_t>(0xFFFFU - length);
    zlib.push_back(static_cast<std::uint8_t>(inverted & 0xFFU));
    zlib.push_back(static_cast<std::uint8_t>(inverted >> 8U));
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : raw)
    {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    appendBigEndian(zlib, (b << 16U) | a);

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    appendBigEndian(header, 2);                   // Width
    appendBigEndian(header, 2);                   // Height
    header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, no interlace
    appendChunk(png, "IHDR", header);
    appendChunk(png, "IDAT", zlib);
    appendChunk(png, "IEND", {});
    return png;
}

constexpr std::array<std::uint8_t, 16> PIXELS{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 10, 20, 30, 40};

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size())); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): ofstream writes chars
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
    const auto icon = LinuxProcessIconSource::decodePng(png);
    ASSERT_TRUE(icon.has_value());
    EXPECT_EQ(icon->width, 2);
    EXPECT_EQ(icon->height, 2);
    EXPECT_EQ(icon->rgba, std::vector<std::uint8_t>(PIXELS.begin(), PIXELS.end()));
}

TEST(LinuxProcessIconSourceTest, ANonPngIsNoIcon)
{
    const TestSupport::ScopedTempDir dir("tasksmack_icon_bad");
    const std::filesystem::path bad = dir.path / "icon.png";
    writeText(bad, "<svg/>");
    EXPECT_FALSE(LinuxProcessIconSource::decodePng(bad).has_value());
    EXPECT_FALSE(LinuxProcessIconSource::decodePng(dir.path / "missing.png").has_value());
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
    EXPECT_EQ(byPath->width, 2);
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
    EXPECT_EQ(source.executablePath(42), "/usr/bin/upgraded");
    EXPECT_EQ(source.executablePath(43), "/usr/bin/app");
    EXPECT_EQ(source.executablePath(44), ""); // No such process
    EXPECT_EQ(source.executablePath(0), "");
}

} // namespace
} // namespace Platform
