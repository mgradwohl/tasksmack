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

TEST_F(RootDumpFolderTest, AParentTheGroupOrOthersMayWriteIsRefused)
{
    using std::filesystem::perms;
    for (const perms extra : {perms::group_write, perms::others_write})
    {
        std::filesystem::permissions(m_Parent, perms::owner_all | perms::group_exec | perms::others_exec | extra);
        EXPECT_TRUE(makePrivateFolder(m_Parent, FOLDER_NAME, ::geteuid()).has_value()) << static_cast<unsigned>(extra);
    }
    std::filesystem::permissions(m_Parent, perms::owner_all | perms::group_exec | perms::others_exec);
    EXPECT_EQ(makePrivateFolder(m_Parent, FOLDER_NAME, ::geteuid()), std::nullopt) << "0711: only the owner may write";
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

TEST_F(RootDumpFolderTest, TheFolderIsMadeInTheHomeWithoutItsTrailingSlashAndOnlyIfTakenByTheRefusalCheck)
{
    // #4262: the seam rootDumpFolder() uses with root's home, 0 and rootDumpRefusal(); a scratch home here.
    const auto takes = [](const std::filesystem::path&) -> std::optional<std::string>
    {
        return std::nullopt;
    };
    EXPECT_EQ(rootDumpFolderIn(m_Parent.string() + "//", ::geteuid(), takes), m_Parent / FOLDER_NAME)
        << "the trailing slashes go, so O_NOFOLLOW still refuses a home that is a link";
    EXPECT_EQ(modeOf(m_Parent / FOLDER_NAME) & 0777, S_IRWXU);
    const auto refuses = [](const std::filesystem::path&) -> std::optional<std::string>
    {
        return "a nodev mount";
    };
    EXPECT_EQ(rootDumpFolderIn(m_Parent.string(), ::geteuid(), refuses), std::filesystem::path{}) << "fail closed";
    EXPECT_EQ(rootDumpFolderIn(m_Parent.string(), ::geteuid() + 1, takes), std::filesystem::path{}) << "not the owner's home";
}

/// As root only (CI's root pass, root_test_filter): rootDumpRefusal(), the real check, refuses a home under a folder
/// anyone may write (/tmp), so no default is offered there.
TEST(RootDumpFolderRootTest, ARootHomeUnderAFolderOthersMayWriteGetsNoDefault)
{
    if (::geteuid() != 0)
    {
        GTEST_SKIP() << "needs root";
    }
    const std::filesystem::path home = std::filesystem::temp_directory_path() / ("tasksmack-root-home-4262-" + std::to_string(::getpid()));
    std::filesystem::create_directories(home);
    std::filesystem::permissions(home, std::filesystem::perms::owner_all);
    EXPECT_EQ(rootDumpFolderIn(home.string() + "/", 0, &rootDumpRefusal), std::filesystem::path{});
    std::error_code error;
    std::filesystem::remove_all(home, error);
}

} // namespace
} // namespace Platform::RootDumpFolder
