/// @file test_FontFileCache.cpp
/// @brief Tests for UI::FontFileCache (#1170): each font file is read once and every font built
/// from it shares that one buffer, which must stay where it is for the atlas's lifetime.

#include "UI/FontFileCache.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace UI
{
namespace
{

using FileMap = std::map<std::filesystem::path, std::vector<std::byte>>;
using ReadCounts = std::map<std::filesystem::path, int>;

/// A reader that serves @p files' made-up contents and counts in @p reads how often each path is read.
[[nodiscard]] auto countingReader(ReadCounts& reads, const FileMap& files) -> FontFileCache::Reader
{
    return [&reads, &files](const std::filesystem::path& path) -> std::optional<std::vector<std::byte>>
    {
        ++reads[path];
        if (const auto found = files.find(path); found != files.end())
        {
            return found->second;
        }
        return std::nullopt;
    };
}

[[nodiscard]] auto bytes(std::size_t count, unsigned char fill) -> std::vector<std::byte>
{
    return std::vector<std::byte>(count, static_cast<std::byte>(fill));
}

TEST(FontFileCacheTest, EachFileIsReadOnceAndTheSameBufferIsReturned)
{
    // loadAllFonts() asks for Inter and Font Awesome a dozen times each per build, and builds
    // again on every display-scale change: one read per file, ever.
    ReadCounts reads;
    const FileMap files{{"Inter.ttf", bytes(200, 1)}, {"fa.otf", bytes(300, 2)}};
    FontFileCache cache(countingReader(reads, files));

    const auto first = cache.get("Inter.ttf");
    for (int i = 0; i < 12; ++i)
    {
        const auto again = cache.get("Inter.ttf");
        EXPECT_EQ(again.data(), first.data());
        EXPECT_EQ(again.size(), 200U);
        EXPECT_EQ(cache.get("fa.otf").size(), 300U);
    }
    EXPECT_EQ(reads["Inter.ttf"], 1);
    EXPECT_EQ(reads["fa.otf"], 1);
    EXPECT_EQ(cache.fileCount(), 2U);
}

TEST(FontFileCacheTest, BuffersStayPutAsMoreFilesAreAdded)
{
    // The atlas keeps the pointer it was given, so adding a file must not move earlier buffers.
    ReadCounts reads;
    FileMap files;
    for (int i = 0; i < 64; ++i)
    {
        files.emplace("font" + std::to_string(i) + ".ttf", bytes(128, static_cast<unsigned char>(i)));
    }
    FontFileCache cache(countingReader(reads, files));
    const auto first = cache.get("font0.ttf");
    for (int i = 1; i < 64; ++i)
    {
        (void) cache.get("font" + std::to_string(i) + ".ttf");
    }
    EXPECT_EQ(cache.get("font0.ttf").data(), first.data());
    EXPECT_EQ(first[0], std::byte{0});
}

TEST(FontFileCacheTest, UnreadableFileIsEmptyAndRetriedLater)
{
    ReadCounts reads;
    const FileMap files;
    FontFileCache cache(countingReader(reads, files));
    EXPECT_TRUE(cache.get("missing.ttf").empty());
    EXPECT_TRUE(cache.get("missing.ttf").empty());
    EXPECT_EQ(reads["missing.ttf"], 2);
    EXPECT_EQ(cache.fileCount(), 0U);
}

TEST(FontFileCacheTest, EmptyFileCountsAsUnreadable)
{
    ReadCounts reads;
    const FileMap files{{"empty.ttf", {}}};
    FontFileCache cache(countingReader(reads, files));
    EXPECT_TRUE(cache.get("empty.ttf").empty());
    EXPECT_EQ(cache.fileCount(), 0U);
}

TEST(FontFileCacheTest, ReadFontFileReadsTheWholeFile)
{
    const auto dir = std::filesystem::temp_directory_path();
    const auto path = dir / ("tasksmack_fontcache_" + std::to_string(std::random_device{}()) + ".bin");
    std::vector<char> contents(4096);
    for (std::size_t i = 0; i < contents.size(); ++i)
    {
        contents[i] = static_cast<char>(i % 251);
    }
    {
        std::ofstream out(path, std::ios::binary);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    const auto read = readFontFile(path);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    const std::vector<std::byte> data = read.value_or(std::vector<std::byte>{});
    ASSERT_EQ(data.size(), contents.size());
    for (std::size_t i = 0; i < contents.size(); ++i)
    {
        ASSERT_EQ(static_cast<char>(data[i]), contents[i]) << i;
    }
    EXPECT_FALSE(readFontFile(dir / "tasksmack_no_such_font_file.ttf").has_value());
}

} // namespace
} // namespace UI
