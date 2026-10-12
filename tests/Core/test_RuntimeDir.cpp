/// @file test_RuntimeDir.cpp
/// @brief The XDG runtime directory fallback (#1395, Core/RuntimeDir.h), against temporary
/// directories: a usable systemd directory is taken; otherwise the fallback is created owner-only, an
/// existing one with loose permissions is tightened, and a symlink or a file in its place is refused.

#include "Core/RuntimeDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>

namespace Core::RuntimeDir
{
namespace
{

class RuntimeDirTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        ASSERT_FALSE(ec);
        std::random_device random;
        for (int attempt = 0; attempt < 100 && m_Dir.empty(); ++attempt)
        {
            std::filesystem::path candidate = base / ("tasksmack_runtime_dir_" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate, ec) && !ec)
            {
                m_Dir = std::move(candidate);
            }
        }
        ASSERT_FALSE(m_Dir.empty());
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    /// @p name under the test's own directory, as a string (the functions take the strings
    /// ensureXdgRuntimeDir() builds).
    [[nodiscard]] std::string path(const char* name) const
    {
        return (m_Dir / name).string();
    }

    [[nodiscard]] static std::filesystem::perms permsOf(const std::string& p)
    {
        return std::filesystem::status(p).permissions() & std::filesystem::perms::mask;
    }

    std::filesystem::path m_Dir;
};

TEST(RuntimeDirPermissionsTest, OnlyOwnerOnlyAccessIsSafe)
{
    EXPECT_TRUE(hasSafeOwnerOnlyPermissions(std::filesystem::perms::owner_all));
    EXPECT_FALSE(hasSafeOwnerOnlyPermissions(std::filesystem::perms::owner_all | std::filesystem::perms::group_read));
    EXPECT_FALSE(hasSafeOwnerOnlyPermissions(std::filesystem::perms::owner_all | std::filesystem::perms::others_exec));
    // The owner must be able to read, write and enter it.
    EXPECT_FALSE(hasSafeOwnerOnlyPermissions(std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec));
}

TEST_F(RuntimeDirTest, AUsableSystemdDirectoryIsChosen)
{
    const std::string systemd = path("run-user");
    std::filesystem::create_directory(systemd);
    std::filesystem::permissions(systemd, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    EXPECT_TRUE(isUsableRuntimeDir(systemd));
    EXPECT_EQ(chooseRuntimeDir(systemd, path("fallback")), std::optional<std::string>(systemd));
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(path("fallback"), ec)); // the fallback isn't touched
}

TEST_F(RuntimeDirTest, WithoutOneTheFallbackIsCreatedOwnerOnly)
{
    const std::string fallback = path("fallback");
    EXPECT_EQ(chooseRuntimeDir(path("no-such-run-user"), fallback), std::optional<std::string>(fallback));
    EXPECT_TRUE(std::filesystem::is_directory(fallback));
    EXPECT_EQ(permsOf(fallback), std::filesystem::perms::owner_all);
    EXPECT_TRUE(isUsableRuntimeDir(fallback));
}

TEST_F(RuntimeDirTest, ASystemdDirectoryOthersCanReadIsPassedOver)
{
    const std::string systemd = path("run-user");
    std::filesystem::create_directory(systemd);
    std::filesystem::permissions(systemd,
                                 std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec,
                                 std::filesystem::perm_options::replace);
    EXPECT_FALSE(isUsableRuntimeDir(systemd));
    EXPECT_EQ(chooseRuntimeDir(systemd, path("fallback")), std::optional<std::string>(path("fallback")));
}

TEST_F(RuntimeDirTest, AnExistingFallbackWithLoosePermissionsIsTightened)
{
    const std::string fallback = path("fallback");
    std::filesystem::create_directory(fallback);
    std::filesystem::permissions(
        fallback, std::filesystem::perms::owner_all | std::filesystem::perms::others_all, std::filesystem::perm_options::replace);
    EXPECT_EQ(chooseRuntimeDir(path("no-such-run-user"), fallback), std::optional<std::string>(fallback));
    EXPECT_EQ(permsOf(fallback), std::filesystem::perms::owner_all);
}

TEST_F(RuntimeDirTest, ASymlinkInPlaceOfTheFallbackIsRefused)
{
    // On a shared /tmp another user could plant one, pointing wherever they like.
    const std::string target = path("elsewhere");
    std::filesystem::create_directory(target);
    const std::string fallback = path("fallback");
    std::filesystem::create_directory_symlink(target, fallback);
    EXPECT_FALSE(isUsableRuntimeDir(fallback));
    EXPECT_EQ(chooseRuntimeDir(path("no-such-run-user"), fallback), std::nullopt);
}

TEST_F(RuntimeDirTest, AFileInPlaceOfTheFallbackIsRefused)
{
    const std::string fallback = path("fallback");
    std::ofstream(fallback) << "not a directory";
    EXPECT_FALSE(isUsableRuntimeDir(fallback));
    EXPECT_EQ(chooseRuntimeDir(path("no-such-run-user"), fallback), std::nullopt);
}

TEST_F(RuntimeDirTest, AMissingPathIsNotUsable)
{
    EXPECT_FALSE(isUsableRuntimeDir(path("missing")));
}

} // namespace
} // namespace Core::RuntimeDir
