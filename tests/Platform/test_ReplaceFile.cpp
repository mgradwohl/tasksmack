/// @file test_ReplaceFile.cpp
/// @brief A file replaced whole (#3771): a new file and a replaced one get the text, a replaced one keeps its
/// permissions, nothing planted beside it is followed or written, and a failure leaves the old file and no staging file.

#include <gtest/gtest.h>

#if defined(__linux__)

#include "Platform/Linux/ReplaceFile.h"
#include "Platform/ScopedTempDir.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <unistd.h>

namespace Platform
{
namespace
{

[[nodiscard]] std::string contents(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::size_t filesIn(const std::filesystem::path& dir)
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir))
    {
        ++count;
    }
    return count;
}

TEST(ReplaceFileTest, MakesOrReplacesTheFileAndKeepsItsPermissions)
{
    const TestSupport::ScopedTempDir dir("tasksmack_replace_file");
    const std::filesystem::path path = dir.path / "entry.desktop";
    EXPECT_EQ(ReplaceFile::replace(path, "first\n"), "");
    EXPECT_EQ(contents(path), "first\n");

    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    EXPECT_EQ(ReplaceFile::replace(path, "second\n"), "");
    EXPECT_EQ(contents(path), "second\n");
    EXPECT_EQ(std::filesystem::status(path).permissions(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    EXPECT_EQ(filesIn(dir.path), 1U) << "no staging file left behind";
}

// The old staging names were predictable (".tasksmack-<pid>.tmp", ".partial"): a link planted there would have been
// followed by std::ofstream's truncate. The staging name now can't be guessed, and is made with O_EXCL and
// O_NOFOLLOW: what was planted at the old names is left alone, and the link's target is untouched.
TEST(ReplaceFileTest, NothingPlantedBesideTheFileIsFollowedOrWritten)
{
    const TestSupport::ScopedTempDir dir("tasksmack_replace_planted");
    const std::filesystem::path path = dir.path / "entry.desktop";
    const std::filesystem::path victim = dir.path / "victim";
    std::ofstream(victim) << "keep me";
    for (const std::string& name : {std::string("entry.desktop.partial"), "entry.desktop.tasksmack-" + std::to_string(::getpid()) + ".tmp"})
    {
        std::filesystem::create_symlink(victim, dir.path / name);
    }
    EXPECT_EQ(ReplaceFile::replace(path, "entry\n"), "");
    EXPECT_EQ(contents(path), "entry\n");
    EXPECT_EQ(contents(victim), "keep me");
    EXPECT_EQ(filesIn(dir.path), 4U) << "the entry, the victim and the two planted links, nothing else";
}

TEST(ReplaceFileTest, AFailureLeavesTheOldFileAndNoStagingFile)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root writes into a read-only directory regardless";
    }
    const TestSupport::ScopedTempDir dir("tasksmack_replace_failure");
    const std::filesystem::path path = dir.path / "entry.desktop";
    ASSERT_EQ(ReplaceFile::replace(path, "old\n"), "");
    std::filesystem::permissions(dir.path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    const std::string failed = ReplaceFile::replace(path, "new\n");
    std::filesystem::permissions(dir.path, std::filesystem::perms::owner_all);
    EXPECT_NE(failed, "");
    EXPECT_NE(failed.find("couldn't create"), std::string::npos) << failed;
    EXPECT_EQ(contents(path), "old\n");
    EXPECT_EQ(filesIn(dir.path), 1U);
}

} // namespace
} // namespace Platform

#endif
