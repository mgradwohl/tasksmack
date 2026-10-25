/// @file test_ProcParsing.cpp
/// @brief ProcParsing's file readers and number parser on ordinary files (#1891): a whole read, a read
/// bounded by the buffer, a file larger than one chunk, and the failures -- a missing file and a
/// directory, whose read() fails with EISDIR, so each reader discards what it has and reports nothing.

#include "Platform/Linux/ProcParsing.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>

namespace Platform::ProcParsing
{
namespace
{

using TestSupport::ScopedTempDir;

/// Writes @p contents to @p path.
void writeFile(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

TEST(ProcParsingReadTest, AFileIsReadWhole)
{
    const ScopedTempDir dir("proc_parsing_read");
    const auto file = dir.path / "stat";
    writeFile(file, "1 (init) S 0");
    std::array<char, 64> buf{};
    const std::size_t read = readProcFileAt(AT_FDCWD, file.c_str(), buf.data(), buf.size());
    EXPECT_EQ(std::string_view(buf.data(), read), "1 (init) S 0");
    EXPECT_EQ(readProcFile(file.c_str(), buf.data(), buf.size()), read);
    EXPECT_EQ(readProcFileOnceAt(AT_FDCWD, file.c_str(), buf.data(), buf.size()), read);
}

TEST(ProcParsingReadTest, AReadStopsAtTheBuffersEnd)
{
    const ScopedTempDir dir("proc_parsing_bounded");
    const auto file = dir.path / "status";
    writeFile(file, "0123456789");
    std::array<char, 4> buf{};
    EXPECT_EQ(readProcFileAt(AT_FDCWD, file.c_str(), buf.data(), buf.size()), 4U);
    EXPECT_EQ(std::string_view(buf.data(), buf.size()), "0123");
}

TEST(ProcParsingReadTest, AFullReadTakesAFileLargerThanOneChunk)
{
    const ScopedTempDir dir("proc_parsing_full");
    const auto file = dir.path / "maps";
    const std::string contents(10'000, 'x');
    writeFile(file, contents);
    const std::vector<char> read = readProcFileFull(file.c_str());
    EXPECT_EQ(std::string(read.begin(), read.end()), contents);
}

TEST(ProcParsingReadTest, AMissingFileReadsAsNothing)
{
    const ScopedTempDir dir("proc_parsing_missing");
    const auto file = dir.path / "gone";
    std::array<char, 16> buf{};
    EXPECT_EQ(readProcFileAt(AT_FDCWD, file.c_str(), buf.data(), buf.size()), 0U);
    EXPECT_EQ(readProcFileOnceAt(AT_FDCWD, file.c_str(), buf.data(), buf.size()), 0U);
    EXPECT_TRUE(readProcFileFull(file.c_str()).empty());
}

TEST(ProcParsingReadTest, AFileThatCantBeReadReadsAsNothing)
{
    // A directory opens read-only but read() fails (EISDIR): an I/O error, not EOF, so nothing is kept.
    const ScopedTempDir dir("proc_parsing_unreadable");
    std::array<char, 16> buf{};
    EXPECT_EQ(readProcFileAt(AT_FDCWD, dir.path.c_str(), buf.data(), buf.size()), 0U);
    EXPECT_EQ(readProcFileOnceAt(AT_FDCWD, dir.path.c_str(), buf.data(), buf.size()), 0U);
    EXPECT_TRUE(readProcFileFull(dir.path.c_str()).empty());
}

TEST(ProcParsingParseTest, ParseDoubleReadsANumberAndLeavesTheCursorOnFailure)
{
    constexpr std::string_view TEXT = "  12.5 rest";
    const char* p = TEXT.data();
    double value = 0.0;
    ASSERT_TRUE(parseDouble(p, TEXT.data() + TEXT.size(), value));
    EXPECT_DOUBLE_EQ(value, 12.5);
    EXPECT_EQ(std::string_view(p, 5), " rest");

    const char* const before = p;
    EXPECT_FALSE(parseDouble(p, TEXT.data() + TEXT.size(), value)); // "rest" is not a number
    EXPECT_EQ(p, before);
    EXPECT_DOUBLE_EQ(value, 12.5);
}

} // namespace
} // namespace Platform::ProcParsing
