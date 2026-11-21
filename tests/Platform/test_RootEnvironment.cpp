/// @file test_RootEnvironment.cpp
/// @brief Platform::RootEnvironment (#4158, #4159, #4179): a root session whose environment may be a user's keeps only
/// an allow-list, with root's PATH and HOME. Tested on a fake environment; the process's own is left alone.

#include "Platform/Linux/RootEnvironment.h"
#include "Platform/Linux/SessionUser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// #4666: the process's own PATH is the one applyToThisProcess() works out (ROOT_PATH's root-only folders), not ROOT_PATH
// itself: a /usr/local another account may write is left out of what SDL's zenity lookup searches.
TEST(RootEnvironmentTest, PathIsTheOneGivenSoAnUntrustedUsrLocalIsLeftOut)
{
    const std::array<const char*, 3> environment{"PATH=/home/me/bin:/usr/bin", "DISPLAY=:0", nullptr};
    Recorder recorder;
    static_cast<void>(
        applyTo(environment.data(),
                RootValues{.home = "/root", .runtimeDir = std::nullopt, .xauthority = std::nullopt, .path = "/usr/sbin:/usr/bin"},
                recorder.operations()));
    EXPECT_EQ(recorder.set.at("PATH"), "/usr/sbin:/usr/bin");
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

TEST(RootEnvironmentTest, OnlyALocaleNameNotAPathIsKept)
{
    // #4256: as sudo's env_check, a locale value with "/" or "%", or starting with ".", goes; glibc could take it as a
    // path to locale data the user chose.
    for (const char* name : {"LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES", "TASKSMACK_LOCALE"})
    {
        EXPECT_TRUE(isLocaleVariable(name)) << name;
    }
    EXPECT_FALSE(isLocaleVariable("LANGS"));
    EXPECT_FALSE(isLocaleVariable("TASKSMACK_DEMO"));
    for (const char* value : {"en_US.UTF-8", "C.UTF-8", "C", "de_DE@euro", "en_GB:en", ""})
    {
        EXPECT_TRUE(isSafeLocale(value)) << value;
    }
    for (const char* value : {"/home/alice/loc", "../../home/alice/loc", ".hidden", "en_US%2F", "C/../x"})
    {
        EXPECT_FALSE(isSafeLocale(value)) << value;
    }
    EXPECT_FALSE(isSafeLocale(" .evil")) << "trimmed first, as the override reader trims it";
    EXPECT_TRUE(isLocaleVariable("TERM"));
    EXPECT_TRUE(isLocaleVariable("COLORTERM"));
    const std::array<const char*, 9> environment{"LANG=en_US.UTF-8",
                                                 "LC_ALL=/home/alice/loc",
                                                 "LANGUAGE=../x",
                                                 "TASKSMACK_LOCALE=.evil",
                                                 "LC_CTYPE=/x",
                                                 "TERM=/home/alice/t",
                                                 "LC_TIME=en%x",
                                                 "LC_ALL=C",
                                                 nullptr};
    Recorder recorder;
    const std::vector<std::string> removed = applyTo(environment.data(), RootValues{}, recorder.operations());
    for (const char* name : {"LC_ALL", "LANGUAGE", "TASKSMACK_LOCALE", "LC_CTYPE", "TERM", "LC_TIME"})
    {
        EXPECT_NE(std::ranges::find(recorder.unset, name), recorder.unset.end()) << name << " is unset";
    }
    EXPECT_EQ(std::ranges::find(recorder.unset, "LANG"), recorder.unset.end()) << "a plain LANG stays";
    EXPECT_NE(std::ranges::find(removed, "LC_ALL"), removed.end())
        << "a safe LC_ALL beside an unsafe one: the name is unset, every copy (unsetenv)";
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

// #4667: each session gets a copy of its own, so a second user's root session can't rewrite the first one's cookies.
TEST_F(XauthorityCopyTest, EachSessionWritesACopyOfItsOwnThatNoOtherSessionTouches)
{
    const auto read = [](const std::string& path)
    {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::optional<std::string> alice = writeXauthorityCopy(m_Dir.string(), 1000, "alice's cookie");
    const std::optional<std::string> bob = writeXauthorityCopy(m_Dir.string(), 1001, "bob's cookie");
    const std::optional<std::string> aliceAgain = writeXauthorityCopy(m_Dir.string(), 1000, "alice's other cookie");
    ASSERT_TRUE(alice.has_value() && bob.has_value() && aliceAgain.has_value());
    EXPECT_NE(*alice, *bob);
    EXPECT_NE(*alice, *aliceAgain) << "the same user's second session too: never a file another session reads";
    EXPECT_TRUE(std::filesystem::path(*alice).filename().string().starts_with("tasksmack-xauthority-1000-")) << *alice;
    EXPECT_EQ(read(*alice), "alice's cookie") << "untouched by the sessions after it";
    EXPECT_EQ(read(*bob), "bob's cookie");
    EXPECT_EQ(read(*aliceAgain), "alice's other cookie");
    struct stat info = {};
    ASSERT_EQ(::stat(alice->c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777U, 0600U);
    EXPECT_EQ(writeXauthorityCopy((m_Dir / "missing").string(), 1000, "x"), std::nullopt) << "no folder, no copy";
}

TEST_F(XauthorityCopyTest, TheSessionsCopyIsRemovedWhenItExits)
{
    const std::optional<std::string> copy = writeXauthorityCopy(m_Dir.string(), 1000, "cookie");
    ASSERT_TRUE(copy.has_value());
    // In a child process (a death test forks), so this process's own exit handlers are left alone.
    EXPECT_EXIT(
        {
            removeAtExit(*copy);
            std::exit(0);
        },
        ::testing::ExitedWithCode(0),
        "");
    EXPECT_FALSE(std::filesystem::exists(*copy)) << "removed by the exit handler";
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
    // Under /tmp itself, not temp_directory_path(): CI's TMPDIR is under /home/runner, which nobody can't pass through, so
    // every open as nobody would fail there for a reason that isn't the file's (the #4286 CI run).
    const std::filesystem::path dir = std::filesystem::path("/tmp") / ("tasksmack-xauth-root-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    // Another account to switch to: the one sudo names (CI's runner user), else nobody. A uid the kernel can't switch the
    // filesystem uid to (unmapped in a container's user namespace, say) skips, rather than pass or fail on it (#4286 CI).
    uid_t account = 65534;
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - a test, one thread
    if (const char* sudoUid = std::getenv("SUDO_UID"); sudoUid != nullptr && std::atoi(sudoUid) > 0)
    {
        account = static_cast<uid_t>(std::atoi(sudoUid));
    }
    if (::getpwuid(account) == nullptr)
    {
        GTEST_SKIP() << "no passwd entry for uid " << account;
    }
    static_cast<void>(::setfsuid(account));
    const bool canSwitch = ::setfsuid(static_cast<uid_t>(-1)) == static_cast<int>(account);
    static_cast<void>(::setfsuid(0));
    if (!canSwitch)
    {
        GTEST_SKIP() << "the filesystem uid can't be switched to " << account << " here";
    }
    // A folder every account may pass through but not list, so only the files' own modes decide (the #4286 review). Group
    // execute too: setfsuid() leaves root's supplementary groups (group 0 among them), so the kernel judges a root-group
    // folder by its group bits, not its other bits; without g+x it refused the account its own file (the #4286 CI run).
    std::filesystem::permissions(
        dir, std::filesystem::perms::owner_all | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec);
    const std::filesystem::path rootOnly = dir / "root-only";
    std::ofstream(rootOnly) << "secret";
    std::filesystem::permissions(rootOnly, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    const int asRoot = openAsUser(rootOnly.string(), 0);
    EXPECT_GE(asRoot, 0) << "root may: so a -1 below comes from the switch";
    if (asRoot >= 0)
    {
        ::close(asRoot);
    }
    EXPECT_EQ(openAsUser(rootOnly.string(), account), -1) << "that account may not read root's 0600 file";
    const std::filesystem::path nobodys = dir / "nobodys";
    std::ofstream(nobodys) << "cookie";
    ASSERT_EQ(::chown(nobodys.c_str(), account, static_cast<gid_t>(-1)), 0);
    std::filesystem::permissions(nobodys, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    // Whether the kernel lets that account open its own file here at all, with only the filesystem uid switched. CI's
    // root pass refused it (the #4286 run) for a reason the test can't see; then the switch is untestable here, and the
    // errno says why. When the kernel allows it, openAsUser() must too, or the test fails.
    static_cast<void>(::setfsuid(account));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int direct = ::open(nobodys.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    const int directError = errno;
    static_cast<void>(::setfsuid(0));
    if (direct < 0)
    {
        std::filesystem::remove_all(dir);
        GTEST_SKIP() << "with the filesystem uid " << account
                     << ", the kernel refuses its own 0600 file here: " << std::system_category().message(directError);
    }
    ::close(direct);
    const int own = openAsUser(nobodys.string(), account);
    ASSERT_GE(own, 0) << "that account may read its own 0600 file";
    EXPECT_EQ(readOwnedFile(own, account, MOST_XAUTHORITY_BYTES), "cookie");
    ::close(own);
    EXPECT_EQ(::setfsuid(static_cast<uid_t>(-1)), 0) << "root's filesystem uid is back";
    EXPECT_EQ(::setfsgid(static_cast<gid_t>(-1)), 0) << "and its group";
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

} // namespace
} // namespace Platform::RootEnvironment
