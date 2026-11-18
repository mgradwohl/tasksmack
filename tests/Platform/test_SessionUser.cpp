/// @file test_SessionUser.cpp
/// @brief Whom a TaskSmack run as root opens links and folders as (#2461)

#include "Platform/Linux/DetachedSpawn.h"
#include "Platform/Linux/SessionUser.h"
#include "Platform/SessionAccount.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

// #3682: an Administrator session started for a user runs as root, another account than that user's: its per-user
// startup is root's. Root's own session, a user's own TaskSmack and an unknown user are the same account.
TEST(SessionUserTest, AnAdministratorSessionStartedForAUserIsAnotherAccount)
{
    const auto lookup = [](uid_t uid) -> std::optional<Account>
    {
        if (uid == 0)
        {
            return Account{.gid = 0, .name = "root", .home = "/root", .shell = {}, .groups = {}};
        }
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
    const Platform::SessionAccount pkexec = sessionAccount(0, env({{"PKEXEC_UID", "1000"}}), lookup);
    EXPECT_TRUE(pkexec.otherAccount);
    EXPECT_EQ(pkexec.account, "root");
    EXPECT_TRUE(sessionAccount(0, env({{"SUDO_UID", "1000"}}), lookup).otherAccount);
    EXPECT_EQ(sessionAccount(0, env({}), lookup), Platform::SessionAccount{}) << "root's own session";
    EXPECT_EQ(sessionAccount(1000, env({{"PKEXEC_UID", "1000"}}), lookup), Platform::SessionAccount{}) << "the user's own";
    EXPECT_EQ(sessionAccount(0, env({{"PKEXEC_UID", "1234"}}), lookup), Platform::SessionAccount{}) << "no such user";

    // A passwd database without root's entry still names it.
    const auto noRoot = [&lookup](uid_t uid)
    {
        return uid == 0 ? std::nullopt : lookup(uid);
    };
    EXPECT_EQ(sessionAccount(0, env({{"PKEXEC_UID", "1000"}}), noRoot).account, "root");
}

// #3773: this process's invoking user is looked up once, not on every open-link action: the same result each time,
// and any other uid still asks the passwd database.
TEST(SessionUserTest, ThisProcesssInvokingUserIsLookedUpOnce)
{
    const std::optional<InvokingUser>& first = processInvokingUser();
    EXPECT_EQ(&first, &processInvokingUser()) << "one cached answer";
    const uid_t self = ::getuid();
    const std::optional<Account> own = processAccount(self);
    const std::optional<Account> direct = systemAccount(self);
    ASSERT_EQ(own.has_value(), direct.has_value());
    if (own.has_value())
    {
        EXPECT_EQ(own->name, direct->name);
        EXPECT_EQ(own->home, direct->home);
    }
    if (::geteuid() != 0)
    {
        EXPECT_FALSE(first.has_value()) << "not root: no invoking user";
    }
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
TEST(SessionUserTest, AnotherUsersEnvironmentIsDecidedWithNoLookup)
{
    using Platform::SessionUser::environmentMayBeAnotherUsers;
    const auto envOf = [](std::initializer_list<std::pair<const char*, const char*>> vars)
    {
        std::map<std::string, std::string, std::less<>> values;
        for (const auto& [name, value] : vars)
        {
            values.emplace(name, value);
        }
        return [values = std::move(values)](std::string_view name) -> std::optional<std::string>
        {
            if (const auto found = values.find(name); found != values.end())
            {
                return found->second;
            }
            return std::nullopt;
        };
    };
    // #4154: sudo, pkexec and doas name the user; plain su has a user's process above it. No passwd lookup.
    EXPECT_TRUE(environmentMayBeAnotherUsers(0, false, false, envOf({{"SUDO_UID", "1000"}}), false));
    EXPECT_TRUE(environmentMayBeAnotherUsers(0, false, false, envOf({{"PKEXEC_UID", "1000"}}), false));
    EXPECT_TRUE(environmentMayBeAnotherUsers(0, false, false, envOf({{"DOAS_USER", "alice"}}), false));
    EXPECT_TRUE(environmentMayBeAnotherUsers(0, false, false, envOf({{"HOME", "/root"}}), true)) << "plain su";
    EXPECT_TRUE(environmentMayBeAnotherUsers(0, false, false, envOf({{"SUDO_UID", "0"}}), true)) << "sudo SUDO_UID=0 TaskSmack";
    // Only sudo and pkexec set these: even a malformed one counts (the #4170 review).
    for (const char* odd : {"+1000", " 1000", "1000 ", "-1", "x", "99999999999"})
    {
        EXPECT_TRUE(environmentMayBeAnotherUsers(0, false, false, envOf({{"SUDO_UID", odd}}), false)) << odd;
    }
    // sudo -u bob: not root, but a SUDO_UID that isn't this uid.
    EXPECT_TRUE(environmentMayBeAnotherUsers(1001, false, false, envOf({{"SUDO_UID", "1000"}}), false));
    // #4171: secure execution (setuid, setgid, the FAQ's file capabilities), and capabilities held another way.
    EXPECT_TRUE(environmentMayBeAnotherUsers(1000, true, false, envOf({}), false));
    EXPECT_TRUE(environmentMayBeAnotherUsers(0, true, false, envOf({}), false));
    EXPECT_TRUE(environmentMayBeAnotherUsers(1000, false, true, envOf({}), false)) << "ambient capabilities";
    // Root's own session: a root login, a service, cron, a container (release.yml's upgrade check: root, HOME a scratch
    // folder, SDL_VIDEODRIVER=offscreen, nothing above it but root).
    EXPECT_FALSE(environmentMayBeAnotherUsers(0, false, false, envOf({{"HOME", "/root"}}), false));
    EXPECT_FALSE(
        environmentMayBeAnotherUsers(0, false, false, envOf({{"HOME", "/tmp/upgrade-home"}, {"SDL_VIDEODRIVER", "offscreen"}}), false));
    EXPECT_FALSE(
        environmentMayBeAnotherUsers(0, false, false, envOf({{"SUDO_UID", "0"}, {"DOAS_USER", "root"}, {"PKEXEC_UID", ""}}), false));
    EXPECT_FALSE(environmentMayBeAnotherUsers(0, false, true, envOf({}), false)) << "root holds capabilities anyway";
    // A user's own session.
    EXPECT_FALSE(environmentMayBeAnotherUsers(1000, false, false, envOf({{"SUDO_UID", "1000"}, {"DOAS_USER", "x"}}), true));
}

TEST(SessionUserTest, AUserAboveIsFoundUpTheProcessTree)
{
    using Platform::SessionUser::aUserIsAbove;
    using Platform::SessionUser::ProcessIds;
    const auto tree = [](std::map<int, ProcessIds> table)
    {
        return [table = std::move(table)](int pid) -> std::optional<ProcessIds>
        {
            if (const auto found = table.find(pid); found != table.end())
            {
                return found->second;
            }
            return std::nullopt;
        };
    };
    // su: TaskSmack (root) <- su (real uid alice, effective root) <- bash (alice) <- init.
    EXPECT_TRUE(aUserIsAbove(400, tree({{400, {300, 0, 0}}, {300, {200, 1000, 0}}, {200, {1, 1000, 1000}}})));
    // A root login, a service or a container: only root above, up to pid 1.
    EXPECT_FALSE(aUserIsAbove(400, tree({{400, {300, 0, 0}}, {300, {200, 0, 0}}, {200, {1, 0, 0}}})));
    // TaskSmack itself isn't counted, and an ancestor that can't be read ends the walk.
    EXPECT_FALSE(aUserIsAbove(400, tree({{400, {300, 1000, 1000}}})));
    // A loop in a fake table ends.
    EXPECT_FALSE(aUserIsAbove(400, tree({{400, {300, 0, 0}}, {300, {400, 0, 0}}})));
}

// #4186: a SUDO_UID or PKEXEC_UID is the invoking user only when that user runs a process above TaskSmack.
TEST(SessionUserTest, OnlyAnInvokerAboveTheProcessIsTrusted)
{
    using Platform::SessionUser::ProcessIds;
    const std::map<int, ProcessIds> sudo{{400, {300, 0, 0}}, {300, {200, 0, 0}}, {200, {1, 1000, 1000}}};
    const auto ids = [&sudo](int pid) -> std::optional<ProcessIds>
    {
        if (const auto found = sudo.find(pid); found != sudo.end())
        {
            return found->second;
        }
        return std::nullopt;
    };
    // sudo: TaskSmack <- sudo (root) <- alice's shell.
    EXPECT_TRUE(uidIsAbove(400, 1000, ids));
    EXPECT_FALSE(uidIsAbove(400, 1001, ids)) << "su -p with SUDO_UID=1001: no process of 1001's above";
    EXPECT_FALSE(uidIsAbove(400, 1000, [](int) -> std::optional<ProcessIds> { return std::nullopt; })) << "nothing known";
    // #4193 review: an administrator (1000) su'd into user 2000's shell, whose .bashrc forged SUDO_UID=1000, then su -p.
    const std::map<int, ProcessIds> suInto{
        {400, {300, 0, 0}}, {300, {250, 2000, 0}}, {250, {200, 2000, 2000}}, {200, {150, 1000, 0}}, {150, {1, 1000, 1000}}};
    const auto suIds = [&suInto](int pid) -> std::optional<ProcessIds>
    {
        if (const auto found = suInto.find(pid); found != suInto.end())
        {
            return found->second;
        }
        return std::nullopt;
    };
    EXPECT_FALSE(uidIsAbove(400, 1000, suIds)) << "only the nearest user counts: 2000's shell started this";
    EXPECT_TRUE(uidIsAbove(400, 2000, suIds));
    // A setuid-to-1000 helper run by 2000 (effective 1000 only) isn't 1000.
    EXPECT_FALSE(uidIsAbove(400,
                            1000,
                            [](int pid) -> std::optional<ProcessIds>
                            {
                                return pid == 400 ? std::optional(ProcessIds{.parent = 300, .realUid = 0, .effectiveUid = 0})
                                                  : std::optional(ProcessIds{.parent = 1, .realUid = 2000, .effectiveUid = 1000});
                            }));
    // TaskSmack itself isn't counted.
    EXPECT_FALSE(uidIsAbove(
        400,
        0x7fff,
        [](int pid) -> std::optional<ProcessIds>
        { return pid == 400 ? std::optional(ProcessIds{.parent = 0, .realUid = 0x7fff, .effectiveUid = 0x7fff}) : std::nullopt; }));

    const auto above1000 = [](uid_t uid)
    {
        return uid == 1000;
    };
    EXPECT_EQ(invokerVariable("SUDO_UID", std::string("1000"), above1000), "1000");
    EXPECT_EQ(invokerVariable("SUDO_UID", std::string("1001"), above1000), std::nullopt) << "a forged SUDO_UID is dropped";
    EXPECT_EQ(invokerVariable("PKEXEC_UID", std::string("1001"), above1000), std::nullopt);
    EXPECT_EQ(invokerVariable("SUDO_UID", std::string("0"), above1000), "0") << "root: invokingUser() refuses it anyway";
    EXPECT_EQ(invokerVariable("SUDO_UID", std::string("x1"), above1000), "x1") << "malformed: refused anyway";
    EXPECT_EQ(invokerVariable("HOME", std::string("/home/a"), above1000), "/home/a") << "other variables pass";
    EXPECT_EQ(invokerVariable("SUDO_UID", std::nullopt, above1000), std::nullopt);

    // invokingUser() over the trusted value: a forged one names no one, so the session has no invoking user.
    const auto account = [](uid_t uid) -> std::optional<Account>
    {
        return uid == 1000 || uid == 1001 ? std::optional(Account{.gid = uid, .name = "u", .home = "/home/u", .shell = {}, .groups = {}})
                                          : std::nullopt;
    };
    const auto forged = [&](std::string_view name)
    {
        return invokerVariable(name, name == "SUDO_UID" ? std::optional<std::string>("1001") : std::nullopt, above1000);
    };
    EXPECT_FALSE(invokingUser(0, forged, account).has_value());
}

} // namespace Platform::SessionUser
