/// @file test_ReadWholeFile.cpp
/// @brief Core::readWholeFile (#4315, #4652): the bytes of a file opened by its path, and nullopt, never a part of it as
/// if whole, when it can't be opened or a read fails (a folder's EISDIR stands in for EIO or ESTALE here).

#include "Core/ReadWholeFile.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace Core
{
namespace
{

TEST(ReadWholeFileTest, ReadsEveryByteExactly)
{
    const Platform::TestSupport::ScopedTempDir dir("ts_read_whole_file");
    const std::filesystem::path path = dir.path / "config.toml";
    std::string bytes = "[ui]\r\nname = \"a\"\n";
    bytes.push_back('\0');
    bytes.append(40000, 'x'); // more than one read's worth
    {
        std::ofstream out(path, std::ios::binary);
        out << bytes;
    }
    EXPECT_EQ(readWholeFile(path), std::optional<std::string>(bytes));
}

TEST(ReadWholeFileTest, AnEmptyFileIsEmptyAndAMissingOneIsNothing)
{
    const Platform::TestSupport::ScopedTempDir dir("ts_read_whole_file_empty");
    {
        std::ofstream out(dir.path / "empty.toml");
    }
    EXPECT_EQ(readWholeFile(dir.path / "empty.toml"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(readWholeFile(dir.path / "missing.toml"), std::nullopt);
}

// #4652: a folder opens on POSIX and its read fails (EISDIR), as a read from a failing disk or an NFS home fails with EIO
// or ESTALE; std::ifstream's buffer took that for the end of the file and handed back "" as the whole of it.
TEST(ReadWholeFileTest, AReadThatFailsIsNothingNotAnEmptyFile)
{
    const Platform::TestSupport::ScopedTempDir dir("ts_read_whole_file_dir");
    std::filesystem::create_directories(dir.path / "config.toml");
    EXPECT_EQ(readWholeFile(dir.path / "config.toml"), std::nullopt);
}

} // namespace
} // namespace Core
