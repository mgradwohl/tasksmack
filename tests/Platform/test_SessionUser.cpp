/// @file test_SessionUser.cpp
/// @brief Whom a TaskSmack run as root opens links and folders as (#2461)

#include "Platform/Linux/DetachedSpawn.h"
#include "Platform/Linux/SessionUser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

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
            return Account{.gid = 1001, .name = "someone", .home = "/home/someone", .shell = "/bin/bash"};
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

} // namespace
} // namespace Platform::SessionUser
