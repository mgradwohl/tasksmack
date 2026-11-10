/// @file test_SessionUser.cpp
/// @brief Whom a TaskSmack run as root opens links and folders as (#2461)

#include "Platform/Linux/DetachedSpawn.h"
#include "Platform/Linux/SessionUser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

namespace Platform::SessionUser
{
namespace
{

/// invokingUserSession() over @p variables, a passwd database with user 1000 only, and /run/user/1000
/// (with its bus) existing.
[[nodiscard]] std::optional<DetachedSpawn::RunAs> sessionFor(uid_t effectiveUid, const std::map<std::string, std::string>& variables)
{
    return invokingUserSession(
        effectiveUid,
        [&variables](std::string_view name) -> std::optional<std::string>
        {
            const auto it = variables.find(std::string(name));
            return it != variables.end() ? std::optional<std::string>(it->second) : std::nullopt;
        },
        [](uid_t uid) -> std::optional<Account>
        {
            if (uid != 1000)
            {
                return std::nullopt;
            }
            return Account{.gid = 1001, .name = "someone", .home = "/home/someone", .shell = "/bin/bash", .groups = {1001, 27, 998}};
        },
        [](const std::string& path) { return path == "/run/user/1000" || path == "/run/user/1000/bus"; });
}

[[nodiscard]] bool has(const std::vector<std::string>& environment, std::string_view entry)
{
    return std::ranges::find(environment, entry) != environment.end();
}

TEST(SessionUserTest, ARunAsRootForAUserOpensAsThatUserInTheirSession)
{
    const std::optional<DetachedSpawn::RunAs> session = sessionFor(0,
                                                                   {{"SUDO_UID", "1000"},
                                                                    {"HOME", "/root"},
                                                                    {"DISPLAY", ":0"},
                                                                    {"WAYLAND_DISPLAY", "wayland-0"},
                                                                    {"LANG", "de_DE.UTF-8"},
                                                                    {"PATH", "/usr/sbin:/usr/bin"},
                                                                    {"LD_PRELOAD", "/tmp/evil.so"},
                                                                    {"SUDO_COMMAND", "/usr/bin/tasksmack"},
                                                                    {"XDG_RUNTIME_DIR", "/run/user/0"}});
    ASSERT_TRUE(session.has_value());
    const DetachedSpawn::RunAs runAs = session.value_or(DetachedSpawn::RunAs{});
    EXPECT_EQ(runAs.uid, 1000U);
    EXPECT_EQ(runAs.gid, 1001U);
    EXPECT_EQ(runAs.groups, (std::vector<gid_t>{1001, 27, 998})) << "the user's groups, as a login gives them (#3298)";
    const std::vector<std::string>& environment = runAs.environment;
    EXPECT_TRUE(has(environment, "HOME=/home/someone"));
    EXPECT_TRUE(has(environment, "USER=someone"));
    EXPECT_TRUE(has(environment, "LOGNAME=someone"));
    EXPECT_TRUE(has(environment, "SHELL=/bin/bash"));
    EXPECT_TRUE(has(environment, "DISPLAY=:0"));
    EXPECT_TRUE(has(environment, "WAYLAND_DISPLAY=wayland-0"));
    EXPECT_TRUE(has(environment, "LANG=de_DE.UTF-8"));
    EXPECT_TRUE(has(environment, "PATH=/usr/sbin:/usr/bin"));
    EXPECT_TRUE(has(environment, "XDG_RUNTIME_DIR=/run/user/1000")) << "the user's, not root's";
    EXPECT_TRUE(has(environment, "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus"));
    EXPECT_FALSE(has(environment, "HOME=/root"));
    EXPECT_FALSE(has(environment, "XDG_RUNTIME_DIR=/run/user/0"));
    EXPECT_TRUE(std::ranges::none_of(environment, [](const std::string& entry) { return entry.starts_with("LD_PRELOAD="); }));
    EXPECT_TRUE(std::ranges::none_of(environment, [](const std::string& entry) { return entry.starts_with("SUDO_"); }));
}

TEST(SessionUserTest, PkexecsUserCountsWhenSudoNamesNone)
{
    const std::optional<DetachedSpawn::RunAs> session = sessionFor(0, {{"PKEXEC_UID", "1000"}});
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session.value_or(DetachedSpawn::RunAs{}).uid, 1000U);
}

TEST(SessionUserTest, ProgramsStartAsTaskSmackWhenThereIsNoOtherUser)
{
    EXPECT_FALSE(sessionFor(1000, {{"SUDO_UID", "1000"}}).has_value()) << "not root";
    EXPECT_FALSE(sessionFor(0, {}).has_value()) << "root's own session";
    EXPECT_FALSE(sessionFor(0, {{"SUDO_UID", "0"}}).has_value()) << "sudo from root";
    EXPECT_FALSE(sessionFor(0, {{"SUDO_UID", "1000x"}}).has_value());
    EXPECT_FALSE(sessionFor(0, {{"SUDO_UID", ""}}).has_value());
    EXPECT_FALSE(sessionFor(0, {{"SUDO_UID", "1234"}}).has_value()) << "no such user";
}

// #3580: the user whose settings an Administrator session keeps, in the same cases as their session above.
TEST(SessionUserTest, TheInvokingUserIsSudosThenPkexecsAndOnlyForRoot)
{
    const auto lookup = [](uid_t uid) -> std::optional<Account>
    {
        return uid == 1000
                 ? std::optional<Account>(Account{.gid = 1001, .name = "someone", .home = "/home/someone", .shell = {}, .groups = {}})
                 : std::nullopt;
    };
    const auto env = [](const std::map<std::string, std::string>& variables)
    {
        return [variables](std::string_view name) -> std::optional<std::string>
        {
            const auto it = variables.find(std::string(name));
            return it != variables.end() ? std::optional<std::string>(it->second) : std::nullopt;
        };
    };
    const std::optional<InvokingUser> pkexec = invokingUser(0, env({{"PKEXEC_UID", "1000"}}), lookup);
    ASSERT_TRUE(pkexec.has_value());
    EXPECT_EQ(pkexec.value_or(InvokingUser{}).uid, 1000U);
    EXPECT_EQ(pkexec.value_or(InvokingUser{}).account.home, "/home/someone");
    EXPECT_TRUE(invokingUser(0, env({{"SUDO_UID", "1000"}, {"PKEXEC_UID", "1234"}}), lookup).has_value()) << "SUDO_UID first";
    EXPECT_FALSE(invokingUser(1000, env({{"PKEXEC_UID", "1000"}}), lookup).has_value()) << "not root";
    EXPECT_FALSE(invokingUser(0, env({}), lookup).has_value()) << "root's own session";
    EXPECT_FALSE(invokingUser(0, env({{"PKEXEC_UID", "0"}}), lookup).has_value());
    EXPECT_FALSE(invokingUser(0, env({{"PKEXEC_UID", "1234"}}), lookup).has_value()) << "no such user";
}

TEST(SessionUserTest, ThisUsersProcessHasNoInvokingUserHome)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "run as root, whoever started it may be named";
    }
    EXPECT_FALSE(invokingUserHome().has_value());
}

// #3374: an Administrator session started with sudo keeps the sudo user's settings, read from this process's own
// environment and passwd database. Only as root: the coverage job's root pass runs it under sudo.
TEST(SessionUserRootTest, UnderSudoTheInvokingUserHomeIsTheSudoUsers)
{
    if (::geteuid() != 0)
    {
        GTEST_SKIP() << "needs root: the coverage job's root pass runs this suite under sudo";
    }
    const std::optional<std::string> sudoUid = processVariable("SUDO_UID");
    uid_t uid = 0;
    if (!sudoUid.has_value() || std::from_chars(sudoUid->data(), sudoUid->data() + sudoUid->size(), uid).ec != std::errc{} || uid == 0)
    {
        GTEST_SKIP() << "root, but not started through sudo by another user";
    }
    const std::optional<Account> account = systemAccount(uid);
    ASSERT_TRUE(account.has_value()) << "SUDO_UID " << *sudoUid << " has no passwd entry";
    ASSERT_FALSE(account->home.empty());
    EXPECT_EQ(invokingUserHome(), std::optional<std::string>(account->home));
}

// #3221: the passwd and environment sources invokingUserSession() reads as root, read here as this user.
TEST(SessionUserTest, TheSystemSourcesReadThisUsersEntryAndVariables)
{
    const std::optional<Account> self = systemAccount(::getuid());
    ASSERT_TRUE(self.has_value()) << "the test's own user is in the passwd database";
    const Account account = self.value_or(Account{});
    EXPECT_EQ(account.gid, ::getgid());
    EXPECT_FALSE(account.name.empty());
    EXPECT_FALSE(account.home.empty());
    // Its groups (#3298): the primary among them, and every group this process runs with that the database lists.
    EXPECT_TRUE(std::ranges::contains(account.groups, ::getgid()));
    EXPECT_EQ(userGroups(account.name, account.gid), account.groups);
    EXPECT_TRUE(userGroups("tasksmack-no-such-user", 4242).size() <= 1U) << "an unknown user is in its primary group at most";
    EXPECT_FALSE(systemAccount(static_cast<uid_t>(-2)).has_value()) << "a UID no entry has";

    ASSERT_EQ(::setenv("TASKSMACK_SESSION_USER_TEST", "value", 1), 0);
    EXPECT_EQ(processVariable("TASKSMACK_SESSION_USER_TEST"), std::optional<std::string>("value"));
    ASSERT_EQ(::unsetenv("TASKSMACK_SESSION_USER_TEST"), 0);
    EXPECT_FALSE(processVariable("TASKSMACK_SESSION_USER_TEST").has_value());

    if (::geteuid() != 0)
    {
        EXPECT_FALSE(invokingUserSession().has_value()) << "not root: programs start as TaskSmack";
    }
}

} // namespace
} // namespace Platform::SessionUser
