/// @file test_RuntimeDir.cpp
/// @brief The XDG runtime directory fallback (#1395, Core/RuntimeDir.h), against temporary
/// directories: a usable systemd directory is taken; otherwise the fallback is created owner-only, an
/// existing one with loose permissions is tightened, and a symlink or a file in its place is refused.

#include "Core/RuntimeDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

// #1395: a fallback that can't be created (a file where its parent should be) is refused.
TEST_F(RuntimeDirTest, AFallbackThatCantBeCreatedIsRefused)
{
    std::ofstream(path("file")) << "not a directory";
    EXPECT_EQ(chooseRuntimeDir(path("no-such-run-user"), path("file") + "/fallback"), std::nullopt);
}

// #1395: an existing fallback another user owns is left alone, not chmodded and used.
TEST_F(RuntimeDirTest, AFallbackAnotherUserOwnsIsRefused)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "running as root: no root-owned directory belongs to another user";
    }
    // The filesystem root: a real directory, owned by root.
    EXPECT_EQ(chooseRuntimeDir(path("no-such-run-user"), "/"), std::nullopt);
}

TEST_F(RuntimeDirTest, AMissingPathIsNotUsable)
{
    EXPECT_FALSE(isUsableRuntimeDir(path("missing")));
}

// #4709, #4810: WSLg without systemd sets XDG_RUNTIME_DIR to its 0777 /mnt/wslg/runtime-dir, refused for TaskSmack's own
// files; its wayland-0 is still the session's display. TaskSmack connects to the socket it checked, through its
// descriptor, and hands libwayland the connection, never the path another account could swap.
TEST_F(RuntimeDirTest, AWaylandSocketInARefusedFolderIsConnectedToAsChecked)
{
    const std::string folder = path("wslg-runtime-dir");
    ASSERT_TRUE(std::filesystem::create_directory(folder));
    std::filesystem::permissions(folder, std::filesystem::perms::all); // 0777, as WSLg makes it
    ASSERT_FALSE(isUsableRuntimeDir(folder));

    const std::string socketPath = folder + "/wayland-0";
    const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(socketPath.size(), sizeof(address.sun_path));
    std::ranges::copy(socketPath, std::begin(address.sun_path));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - the sockets API takes a generic sockaddr
    ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 4), 0);

    const std::optional<int> connected = connectWaylandSocketIn(folder, "wayland-0", ::getuid());
    ASSERT_TRUE(connected.has_value());
    const int accepted = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    EXPECT_GE(accepted, 0) << "the connection reached the socket that was checked";
    ASSERT_EQ(::write(*connected, "x", 1), 1);
    char byte = 0;
    EXPECT_EQ(::read(accepted, &byte, 1), 1);
    EXPECT_EQ(byte, 'x');
    ::close(accepted);
    ::close(*connected);

    EXPECT_EQ(connectWaylandSocketIn(folder, "wayland-0", ::getuid() + 1), std::nullopt) << "another account's socket";
    EXPECT_EQ(connectWaylandSocketIn(folder, "wayland-1", ::getuid()), std::nullopt) << "nothing there";
    EXPECT_EQ(connectWaylandSocketIn(folder, socketPath, ::getuid()), std::nullopt) << "already absolute: left as it is";
    EXPECT_EQ(connectWaylandSocketIn(folder, "../wayland-0", ::getuid()), std::nullopt) << "not a plain socket name";
    EXPECT_EQ(connectWaylandSocketIn(folder, "", ::getuid()), std::nullopt);
    std::ofstream(folder + "/not-a-socket") << "x";
    EXPECT_EQ(connectWaylandSocketIn(folder, "not-a-socket", ::getuid()), std::nullopt) << "a regular file";
    std::filesystem::create_symlink(socketPath, folder + "/linked-0");
    EXPECT_EQ(connectWaylandSocketIn(folder, "linked-0", ::getuid()), std::nullopt) << "never through a link";
    ::close(listener);
    EXPECT_EQ(connectWaylandSocketIn(folder, "wayland-0", ::getuid()), std::nullopt) << "a socket nobody answers";
}

} // namespace
} // namespace Core::RuntimeDir
