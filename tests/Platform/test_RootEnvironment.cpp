/// @file test_RootEnvironment.cpp
/// @brief Platform::RootEnvironment (#4158, #4159, #4179): a root session whose environment may be a user's keeps only
/// an allow-list, with root's PATH and HOME. Tested on a fake environment; the process's own is left alone.

#include "Platform/Linux/RootEnvironment.h"
#include "Platform/Linux/SessionUser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Platform::RootEnvironment
{
namespace
{

TEST(RootEnvironmentTest, TheDisplayLocaleTerminalAndWhoStartedItAreKept)
{
    for (const char* name : {"DISPLAY",
                             "WAYLAND_DISPLAY",
                             "XAUTHORITY",
                             "XDG_RUNTIME_DIR",
                             "LANG",
                             "LC_ALL",
                             "LC_TIME",
                             "TERM",
                             "SUDO_UID",
                             "PKEXEC_UID",
                             "DOAS_USER",
                             "TASKSMACK_LOG_LEVEL",
                             "TASKSMACK_SCREENSHOT"})
    {
        EXPECT_TRUE(isKept(name)) << name;
    }
    // The ones a list of bad names kept missing (#4158, #4159, #4179), and the user's folders.
    for (const char* name : {"GTK_MODULES",
                             "GIO_EXTRA_MODULES",
                             "GDK_PIXBUF_MODULE_FILE",
                             "PYTHONPATH",
                             "SDL_OPENGL_LIBRARY",
                             "LIBGL_DRIVERS_PATH",
                             "DBUS_SESSION_BUS_ADDRESS",
                             "XDG_CONFIG_HOME",
                             "XDG_DATA_HOME",
                             "TMPDIR",
                             "LD_PRELOAD",
                             "display",
                             "lc_all",
                             ""})
    {
        EXPECT_FALSE(isKept(name)) << name;
    }
}

/// A fake environment's operations, recording every unset and set.
struct Recorder
{
    std::vector<std::string> unset;
    std::map<std::string, std::string> set;
    std::string failSet; ///< a name whose set fails
    [[nodiscard]] Operations operations()
    {
        return Operations{
            .unset =
                [this](const std::string& name)
            {
                unset.push_back(name);
                return true;
            },
            .set =
                [this](const std::string& name, const std::string& value)
            {
                if (name == failSet)
                {
                    return false;
                }
                set[name] = value;
                return true;
            },
        };
    }
};

TEST(RootEnvironmentTest, EverythingElseGoesAndRootsValuesReplaceTheUsers)
{
    const std::array<const char*, 12> environment{
        "DISPLAY=:0",
        "GTK_MODULES=/home/me/evil",
        "PATH=/home/me/bin:/usr/bin",
        "HOME=/home/me",
        "PYTHONPATH=/home/me/py",
        "LC_ALL=C.UTF-8",
        "SUDO_UID=1000",
        "XDG_DATA_DIRS=/home/me/x:/usr/share",
        "XDG_RUNTIME_DIR=/run/user/1000",
        "WAYLAND_DISPLAY=wayland-0",
        "NOVALUE",
        nullptr,
    };
    Recorder recorder;
    const std::vector<std::string> removed =
        applyTo(environment.data(),
                RootValues{.home = "/root", .runtimeDir = "/run/user/0", .xauthority = "/run/user/0/tasksmack-xauthority"},
                recorder.operations());
    EXPECT_EQ(removed, (std::vector<std::string>{"GTK_MODULES", "PYTHONPATH", "NOVALUE"}));
    for (const char* kept : {"DISPLAY", "LC_ALL", "SUDO_UID"})
    {
        EXPECT_EQ(std::ranges::find(recorder.unset, kept), recorder.unset.end()) << kept << " is kept, never unset";
    }
    EXPECT_EQ(recorder.set.at("PATH"), std::string(ROOT_PATH)) << "#4159: never the user's PATH";
    EXPECT_EQ(recorder.set.at("HOME"), "/root");
    EXPECT_EQ(recorder.set.at("XDG_DATA_DIRS"), std::string(ROOT_DATA_DIRS)) << "the #4187 review: Vulkan layers, schemas";
    EXPECT_EQ(recorder.set.at("WAYLAND_DISPLAY"), "/run/user/1000/wayland-0") << "the user's socket, by its absolute path";
    EXPECT_EQ(recorder.set.at("XDG_RUNTIME_DIR"), "/run/user/0") << "root's own, where dconf and the like write";
    EXPECT_EQ(recorder.set.at("XAUTHORITY"), "/run/user/0/tasksmack-xauthority")
        << "root's own copy of the user's cookie, never a file the user names (the second #4187 review)";
}

TEST(RootEnvironmentTest, UnknownRootValuesAndAFailedSetUnsetRatherThanKeepTheUsers)
{
    const std::array<const char*, 5> environment{"DISPLAY=:0", "HOME=/home/me", "XDG_RUNTIME_DIR=/run/user/1000", "XAUTHORITY=/x", nullptr};
    Recorder recorder;
    recorder.failSet = "PATH";
    const std::vector<std::string> removed =
        applyTo(environment.data(),
                RootValues{.home = std::nullopt, .runtimeDir = std::nullopt, .xauthority = std::nullopt},
                recorder.operations());
    EXPECT_TRUE(removed.empty());
    for (const char* name : {"XDG_RUNTIME_DIR", "HOME", "PATH", "XAUTHORITY"})
    {
        EXPECT_NE(std::ranges::find(recorder.unset, name), recorder.unset.end()) << name << " unset, not left the user's";
    }
    EXPECT_FALSE(recorder.set.contains("XAUTHORITY")) << "with DISPLAY set, a user-named Xauthority goes when root has no copy";
    // Every replaced name is unset before it's set: no duplicate entry survives.
    EXPECT_NE(std::ranges::find(recorder.unset, "XDG_DATA_DIRS"), recorder.unset.end());
}

TEST(RootEnvironmentTest, OnlyAPlainOrZoneinfoTimeZoneIsKept)
{
    for (const char* tz : {"UTC", "Europe/Paris", ":Europe/Paris", "/usr/share/zoneinfo/UTC", ":/usr/share/zoneinfo/Asia/Tokyo"})
    {
        EXPECT_TRUE(isSafeTimeZone(tz)) << tz;
    }
    for (const char* tz : {"/home/u/f", ":../../../home/u/f", "/usr/share/zoneinfo/../../../home/u/f", "/etc/localtime"})
    {
        EXPECT_FALSE(isSafeTimeZone(tz)) << tz;
    }
    const std::array<const char*, 2> environment{"TZ=/home/u/f", nullptr};
    Recorder recorder;
    EXPECT_EQ(applyTo(environment.data(), RootValues{}, recorder.operations()), (std::vector<std::string>{"TZ"}));
}

/// A scratch folder of this test's own, removed afterwards.
class XauthorityCopyTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = std::filesystem::temp_directory_path() / ("tasksmack-xauthority-4255-" + std::to_string(::getpid()) + "-" +
                                                          ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(m_Dir);
        ASSERT_TRUE(std::filesystem::create_directory(m_Dir));
    }
    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(m_Dir, error);
    }
    [[nodiscard]] std::filesystem::path write(const char* name, const std::string& text) const
    {
        const std::filesystem::path path = m_Dir / name;
        std::ofstream(path) << text;
        return path;
    }
    std::filesystem::path m_Dir;
};

TEST_F(XauthorityCopyTest, OnlyARegularFileOfTheOwnersWithinTheLimitIsRead)
{
    // #4255: the checks that keep root from copying another account's cookie (the second #4187 review).
    const std::filesystem::path cookie = write("cookie", "MIT-MAGIC-COOKIE-1 0123");
    const int fd = ::open(cookie.c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(readOwnedFile(fd, ::geteuid(), MOST_XAUTHORITY_BYTES), "MIT-MAGIC-COOKIE-1 0123");
    EXPECT_EQ(readOwnedFile(fd, ::geteuid() + 1, MOST_XAUTHORITY_BYTES), std::nullopt) << "another uid's file";
    EXPECT_EQ(readOwnedFile(fd, ::geteuid(), 4), std::nullopt) << "over the limit";
    ::close(fd);
    const int folder = ::open(m_Dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ASSERT_GE(folder, 0);
    EXPECT_EQ(readOwnedFile(folder, ::geteuid(), MOST_XAUTHORITY_BYTES), std::nullopt) << "a folder";
    ::close(folder);
    const std::filesystem::path fifo = m_Dir / "fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), S_IRUSR | S_IWUSR), 0);
    const int pipe = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ASSERT_GE(pipe, 0);
    EXPECT_EQ(readOwnedFile(pipe, ::geteuid(), MOST_XAUTHORITY_BYTES), std::nullopt) << "a FIFO";
    ::close(pipe);
}

TEST_F(XauthorityCopyTest, OpenAsUserRefusesALinkAndNeverBlocksOnAFifo)
{
    // The real openAsUser(), as this test's own uid: setfsuid to it and back is allowed unprivileged.
    const std::filesystem::path cookie = write("cookie", "x");
    const int file = openAsUser(cookie.string(), ::geteuid());
    EXPECT_GE(file, 0);
    if (file >= 0)
    {
        ::close(file);
    }
    std::filesystem::create_symlink(cookie, m_Dir / "link");
    EXPECT_EQ(openAsUser((m_Dir / "link").string(), ::geteuid()), -1) << "a link is never followed";
    const std::filesystem::path fifo = m_Dir / "fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), S_IRUSR | S_IWUSR), 0);
    const int pipe = openAsUser(fifo.string(), ::geteuid()); // returns at once: O_NONBLOCK
    EXPECT_GE(pipe, 0);
    if (pipe >= 0)
    {
        ::close(pipe);
    }
    EXPECT_EQ(::setfsuid(static_cast<uid_t>(-1)), static_cast<int>(::geteuid())) << "the ids are put back";
}

TEST_F(XauthorityCopyTest, TheCopysFolderMustBeTheOwnersAndPrivate)
{
    std::filesystem::permissions(m_Dir, std::filesystem::perms::owner_all);
    EXPECT_TRUE(isPrivateFolderOf(m_Dir.string(), ::geteuid()));
    EXPECT_FALSE(isPrivateFolderOf(m_Dir.string(), ::geteuid() + 1)) << "someone else's";
    std::filesystem::permissions(m_Dir, std::filesystem::perms::owner_all | std::filesystem::perms::group_read);
    EXPECT_FALSE(isPrivateFolderOf(m_Dir.string(), ::geteuid())) << "the group may read it";
    std::filesystem::permissions(m_Dir, std::filesystem::perms::owner_all);
    std::filesystem::create_directory_symlink(m_Dir, m_Dir.string() + "-link");
    EXPECT_FALSE(isPrivateFolderOf(m_Dir.string() + "-link", ::geteuid())) << "a link to it";
    std::filesystem::remove(m_Dir.string() + "-link");
}

TEST(RootEnvironmentTest, AnUnsetUserSideValueIsRecordedSoThatChildGetsNone)
{
    // #4259: after ssh -X or startx, then sudo, XAUTHORITY was unset on the user side; the child got root's private copy.
    const auto values = userSideValuesFrom([](std::string_view name) -> std::optional<std::string>
                                           { return name == "PATH" ? std::optional<std::string>("/usr/bin") : std::nullopt; });
    EXPECT_EQ(values.at("PATH"), "/usr/bin");
    ASSERT_TRUE(values.contains("XAUTHORITY"));
    EXPECT_EQ(values.at("XAUTHORITY"), std::nullopt);
    EXPECT_FALSE(values.contains("XDG_DATA_DIRS")) << "an unset one other than XAUTHORITY falls back to the process's";
    auto& live = SessionUser::userSideValues();
    const auto saved = live;
    live = values;
    EXPECT_EQ(SessionUser::userSideVariable("XAUTHORITY"), std::nullopt) << "not the process's own (root's copy)";
    live = saved;
}

/// As root only (CI's root pass, tools/common.sh root_test_filter): the real setfsuid switch to another account.
TEST(RootEnvironmentRootTest, OpenAsUserOpensOnlyWhatThatUserCould)
{
    if (::geteuid() != 0)
    {
        GTEST_SKIP() << "needs root";
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("tasksmack-xauth-root-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    constexpr uid_t NOBODY = 65534;
    if (::getpwuid(NOBODY) == nullptr)
    {
        GTEST_SKIP() << "no passwd entry for uid 65534";
    }
    // The folder nobody may pass through, so only the files' own modes decide (the #4286 review).
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all | std::filesystem::perms::others_exec);
    const std::filesystem::path rootOnly = dir / "root-only";
    std::ofstream(rootOnly) << "secret";
    std::filesystem::permissions(rootOnly, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    const int asRoot = openAsUser(rootOnly.string(), 0);
    EXPECT_GE(asRoot, 0) << "root may: so a -1 below comes from the switch";
    if (asRoot >= 0)
    {
        ::close(asRoot);
    }
    EXPECT_EQ(openAsUser(rootOnly.string(), NOBODY), -1) << "nobody may not read root's 0600 file";
    const std::filesystem::path nobodys = dir / "nobodys";
    std::ofstream(nobodys) << "cookie";
    ASSERT_EQ(::chown(nobodys.c_str(), NOBODY, static_cast<gid_t>(-1)), 0);
    std::filesystem::permissions(nobodys, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    const int own = openAsUser(nobodys.string(), NOBODY);
    ASSERT_GE(own, 0) << "nobody may read its own 0600 file";
    EXPECT_EQ(readOwnedFile(own, NOBODY, MOST_XAUTHORITY_BYTES), "cookie");
    ::close(own);
    EXPECT_EQ(::setfsuid(static_cast<uid_t>(-1)), 0) << "root's filesystem uid is back";
    EXPECT_EQ(::setfsgid(static_cast<gid_t>(-1)), 0) << "and its group";
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

} // namespace
} // namespace Platform::RootEnvironment
