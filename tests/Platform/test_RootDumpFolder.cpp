// A Linux root session's own default dump folder (#4181): made 0700 by descriptor, never through a link.

#include "Platform/Linux/RootDumpFolder.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Platform::RootDumpFolder
{
namespace
{

/// A fresh 0700 folder of this test's own, removed afterwards.
class RootDumpFolderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Parent = std::filesystem::temp_directory_path() / ("tasksmack-root-dump-folder-4181-" + std::to_string(::getpid()) + "-" +
                                                             ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(m_Parent);
        ASSERT_TRUE(std::filesystem::create_directory(m_Parent));
        std::filesystem::permissions(m_Parent, std::filesystem::perms::owner_all);
    }
    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(m_Parent, error);
    }
    [[nodiscard]] static mode_t modeOf(const std::filesystem::path& path)
    {
        struct stat info = {};
        EXPECT_EQ(::lstat(path.c_str(), &info), 0) << path;
        return info.st_mode;
    }
    std::filesystem::path m_Parent;
};

TEST_F(RootDumpFolderTest, MakesAnOwnerOnlyFolderAndReusesIt)
{
    EXPECT_EQ(makePrivateFolder(m_Parent, FOLDER_NAME, ::geteuid()), std::nullopt);
    const std::filesystem::path folder = m_Parent / FOLDER_NAME;
    EXPECT_TRUE(S_ISDIR(modeOf(folder)));
    EXPECT_EQ(modeOf(folder) & 0777, S_IRWXU) << "0700";
    EXPECT_EQ(makePrivateFolder(m_Parent, FOLDER_NAME, ::geteuid()), std::nullopt) << "the one already there is used";
}

TEST_F(RootDumpFolderTest, ALooserFolderOfTheOwnersIsTightened)
{
    const std::filesystem::path folder = m_Parent / FOLDER_NAME;
    ASSERT_TRUE(std::filesystem::create_directory(folder));
    std::filesystem::permissions(folder, std::filesystem::perms::all);
    EXPECT_EQ(makePrivateFolder(m_Parent, FOLDER_NAME, ::geteuid()), std::nullopt);
    EXPECT_EQ(modeOf(folder) & 0777, S_IRWXU);
}

TEST_F(RootDumpFolderTest, ALinkAtTheNameIsRefusedNotFollowed)
{
    const std::filesystem::path elsewhere = m_Parent / "elsewhere";
    ASSERT_TRUE(std::filesystem::create_directory(elsewhere));
    std::filesystem::permissions(elsewhere, std::filesystem::perms::all);
    std::filesystem::create_directory_symlink(elsewhere, m_Parent / FOLDER_NAME);
    EXPECT_TRUE(makePrivateFolder(m_Parent, FOLDER_NAME, ::geteuid()).has_value());
    EXPECT_EQ(modeOf(elsewhere) & 0777, 0777U) << "the link's target is left as it was";
}

TEST_F(RootDumpFolderTest, AFileAtTheNameIsRefused)
{
    {
        const std::filesystem::path file = m_Parent / FOLDER_NAME;
        ASSERT_EQ(::close(::creat(file.c_str(), S_IRUSR | S_IWUSR)), 0);
    }
    EXPECT_TRUE(makePrivateFolder(m_Parent, FOLDER_NAME, ::geteuid()).has_value());
}

TEST_F(RootDumpFolderTest, AParentOthersMayWriteOrSomeoneElsesIsRefused)
{
    std::filesystem::permissions(m_Parent, std::filesystem::perms::all);
    EXPECT_TRUE(makePrivateFolder(m_Parent, FOLDER_NAME, ::geteuid()).has_value()) << "a parent anyone may write";
    std::filesystem::permissions(m_Parent, std::filesystem::perms::owner_all);
    EXPECT_TRUE(makePrivateFolder(m_Parent, FOLDER_NAME, ::geteuid() + 1).has_value()) << "not the owner's parent";
    EXPECT_FALSE(std::filesystem::exists(m_Parent / FOLDER_NAME)) << "nothing is made under a refused parent";
}

TEST_F(RootDumpFolderTest, AParentThatIsALinkIsRefused)
{
    const std::filesystem::path link = m_Parent / "home-link";
    const std::filesystem::path real = m_Parent / "home";
    ASSERT_TRUE(std::filesystem::create_directory(real));
    std::filesystem::permissions(real, std::filesystem::perms::owner_all);
    std::filesystem::create_directory_symlink(real, link);
    EXPECT_TRUE(makePrivateFolder(link, FOLDER_NAME, ::geteuid()).has_value());
}

} // namespace
} // namespace Platform::RootDumpFolder
