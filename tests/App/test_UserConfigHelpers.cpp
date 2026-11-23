// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

#include "App/UserConfigHelpers.h"
#include "Domain/SamplingConfig.h"
#include "Platform/ConfigDirectory.h"

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include <filesystem>
#include <optional>

namespace App
{
namespace
{

// ========== Helper Function Tests ==========

TEST(UserConfigHelpersTest, LoadAndClampAppliesClamp)
{
    // In-range value should be preserved
    {
        auto metrics = toml::table{};
        metrics.insert_or_assign("max_sane_rate_bps", 12345);

        auto tbl = toml::table{};
        tbl.insert_or_assign("metrics", std::move(metrics));

        int dst = 0;
        App::UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
        EXPECT_EQ(dst, 12345);
    }

    // Below-min should clamp to 0
    {
        auto metrics = toml::table{};
        metrics.insert_or_assign("max_sane_rate_bps", -100);

        auto tbl = toml::table{};
        tbl.insert_or_assign("metrics", std::move(metrics));

        int dst = 0;
        App::UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
        EXPECT_EQ(dst, 0);
    }

    // Above-max should clamp to 20000
    {
        auto metrics = toml::table{};
        metrics.insert_or_assign("max_sane_rate_bps", 25000);

        auto tbl = toml::table{};
        tbl.insert_or_assign("metrics", std::move(metrics));

        int dst = 0;
        App::UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
        EXPECT_EQ(dst, 20000);
    }
}

TEST(UserConfigHelpersTest, LoadAndNarrowInt64NarrowsAndClamps)
{
    auto sampling = toml::table{};
    sampling.insert_or_assign("interval_ms", static_cast<std::int64_t>(5000));

    auto tbl = toml::table{};
    tbl.insert_or_assign("sampling", std::move(sampling));

    int dst = 0;
    App::UserConfigHelpers::loadAndNarrowInt64(
        tbl, "sampling", "interval_ms", dst, 1000, [](int v) { return Domain::Sampling::clampRefreshInterval(v); });
    EXPECT_EQ(dst, static_cast<int>(Domain::Sampling::clampRefreshInterval(5000)));
}

TEST(UserConfigHelpersTest, LoadAndNarrowIntWithClampUsesDefaultWhenMissing)
{
    auto tbl = toml::table{};
    int dst = 999;
    // Key missing -> destination unchanged
    App::UserConfigHelpers::loadAndNarrowIntWithClamp(tbl, "window", "width", dst, 800, 200, 16000);
    EXPECT_EQ(dst, 999);
}

// ========== isValidConfigDir ==========

TEST(UserConfigHelpersTest, IsValidConfigDirAcceptsAbsolutePath)
{
    // Build a platform-native absolute path: C:\home\user\... on Windows, /home/user/... on POSIX.
    const auto absPath = std::filesystem::current_path().root_path() / "home" / "user" / ".config" / "tasksmack";
    EXPECT_TRUE(App::UserConfigHelpers::isValidConfigDir(absPath));
}

TEST(UserConfigHelpersTest, IsValidConfigDirAcceptsAbsolutePathWithoutTraversal)
{
    const auto absPath = std::filesystem::current_path().root_path() / "tmp" / "tasksmack";
    EXPECT_TRUE(App::UserConfigHelpers::isValidConfigDir(absPath));
}

TEST(UserConfigHelpersTest, IsValidConfigDirRejectsRelativePath)
{
    EXPECT_FALSE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("relative/path")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirRejectsDotDotTraversal)
{
    // A relative path with traversal components is rejected because it is not absolute.
    EXPECT_FALSE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("../../etc/passwd")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirNormalizesAbsoluteTraversal)
{
    // An absolute path with ".." that normalizes away all traversal components is accepted.
    // e.g. on POSIX: root/"home"/"user"/".."/../"etc" → /etc (absolute, no "..").
    // Restricting WHICH absolute paths are allowed is a caller responsibility (e.g. checking
    // the resolved path is under the expected config root).
    const auto root = std::filesystem::current_path().root_path();
    const auto pathWithTraversal = root / "home" / "user" / ".." / ".." / "etc";
    EXPECT_TRUE(App::UserConfigHelpers::isValidConfigDir(pathWithTraversal));
}

TEST(UserConfigHelpersTest, IsValidConfigDirRejectsBareTraversal)
{
    EXPECT_FALSE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("../../etc/shadow")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirAcceptsRootPath)
{
    // root_path() returns "/" on POSIX and e.g. "C:\" on Windows — always absolute.
    EXPECT_TRUE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::current_path().root_path()));
}

TEST(UserConfigHelpersTest, IsValidConfigDirRejectsEmptyPath)
{
    // Empty path is not absolute.
    EXPECT_FALSE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("")));
}

#ifndef _WIN32
// ========== configDirectoryFrom (#1891): the XDG config directory ==========
// POSIX paths only: the function decides the Linux directory (Windows uses %APPDATA%).

using App::UserConfigHelpers::configDirectoryFrom;

/// A passwd lookup that must not happen: $HOME was set.
[[nodiscard]] std::optional<std::filesystem::path> noPasswdLookup()
{
    ADD_FAILURE() << "the passwd entry was read although $HOME is set";
    return std::nullopt;
}

// #4802: a capabilities session's own home (SessionUser::settingsOwnerHome()) bounds what its environment may name,
// as a root session's user's does: XDG_CONFIG_HOME or HOME pointing at another user's folder is passed over.
TEST(UserConfigHelpersTest, ACapabilitiesSessionsEnvironmentNamesSettingsOnlyInsideItsOwnHome)
{
    using Platform::ConfigDirectory::configDirectoryFor;
    const auto another = configDirectoryFor("/home/bob/.config", "/home/alice", noPasswdLookup, "/cwd", "/home/alice");
    EXPECT_EQ(another.directory, std::filesystem::path("/home/alice/.config/tasksmack")) << "never bob's";
    EXPECT_TRUE(another.warning.contains("file capabilities")) << another.warning;
    EXPECT_EQ(configDirectoryFor(std::nullopt, "/home/bob", noPasswdLookup, "/cwd", "/home/alice").directory,
              std::filesystem::path("/home/alice/.config/tasksmack"))
        << "a HOME that isn't the user's own";
    EXPECT_EQ(configDirectoryFor("/home/alice/.xdg", "/home/alice", noPasswdLookup, "/cwd", "/home/alice").directory,
              std::filesystem::path("/home/alice/.xdg/tasksmack"))
        << "their own XDG_CONFIG_HOME, inside their home, as usual";
}

// #3580: run as root for a user (sudo, pkexec), TaskSmack keeps that user's settings, not root's.
TEST(UserConfigHelpersTest, RootRunForAUserUsesThatUsersConfigWhenHomeIsRoots)
{
    using Platform::ConfigDirectory::configDirectoryFor;
    // pkexec, and sudo: HOME is root's, and root's XDG_CONFIG_HOME, if any, is ignored too.
    EXPECT_EQ(configDirectoryFor(std::nullopt, "/root", noPasswdLookup, "/cwd", "/home/u").directory,
              std::filesystem::path("/home/u/.config/tasksmack"));
    EXPECT_EQ(configDirectoryFor("/root/.xdg", "/root", noPasswdLookup, "/cwd", "/home/u").directory,
              std::filesystem::path("/home/u/.config/tasksmack"));
    EXPECT_EQ(configDirectoryFor(std::nullopt, std::nullopt, noPasswdLookup, "/cwd", "/home/u").directory,
              std::filesystem::path("/home/u/.config/tasksmack"))
        << "no HOME at all: the user's, never root's passwd entry";
    // sudo -E: HOME is the user's own, so their environment decides as usual, XDG_CONFIG_HOME included.
    EXPECT_EQ(configDirectoryFor("/home/u/.xdg", "/home/u", noPasswdLookup, "/cwd", "/home/u").directory,
              std::filesystem::path("/home/u/.xdg/tasksmack"));
    // sudo -E with XDG_CONFIG_HOME outside their home (#4153): root would write there, so it's their home's, and says so.
    for (const char* outside : {"/etc", "/home/u2/.xdg", "/home/u/../v", "/"})
    {
        const auto choice = configDirectoryFor(outside, "/home/u", noPasswdLookup, "/cwd", "/home/u");
        EXPECT_EQ(choice.directory, std::filesystem::path("/home/u/.config/tasksmack")) << outside;
        EXPECT_NE(choice.warning.find("XDG_CONFIG_HOME"), std::string::npos) << outside;
    }
    EXPECT_EQ(configDirectoryFor("/home/u/./a/../.xdg", "/home/u/", noPasswdLookup, "/cwd", "/home/u/").directory,
              std::filesystem::path("/home/u/.xdg/tasksmack"))
        << "inside, once normalized, with a trailing separator on the home";
    // A home of "/" vouches for nothing: every absolute path is lexically inside it.
    EXPECT_EQ(configDirectoryFor("/etc", "/", noPasswdLookup, "/cwd", "/").directory, std::filesystem::path("/.config/tasksmack"));
    // #4172: root with an environment that may be a user's, but no SUDO_UID to say whose (su, doas keepenv): root's own
    // folder from its passwd entry, and neither the user's XDG_CONFIG_HOME nor HOME.
    const auto rootsPasswd = []
    {
        return std::optional<std::filesystem::path>("/root");
    };
    const auto suChoice = configDirectoryFor("/etc", "/home/u", rootsPasswd, "/cwd", std::nullopt, true);
    EXPECT_EQ(suChoice.directory, std::filesystem::path("/root/.config/tasksmack"));
    EXPECT_TRUE(suChoice.warning.contains("Ignoring XDG_CONFIG_HOME and HOME")) << suChoice.warning;
    EXPECT_EQ(configDirectoryFor(std::nullopt, std::nullopt, rootsPasswd, "/cwd", std::nullopt, true).warning, "")
        << "nothing set, nothing to say";
    EXPECT_EQ(configDirectoryFor(std::nullopt, "/root", rootsPasswd, "/cwd", std::nullopt, true).warning, "")
        << "plain su: HOME is root's own, the same folder, nothing to say";
    EXPECT_FALSE(configDirectoryFor(std::nullopt, "/home/u", rootsPasswd, "/cwd", std::nullopt, true).warning.empty()) << "HOME alone";
    // Root's passwd entry unreadable: /root, never the working directory su kept (the #4182 review).
    const auto unreadablePasswd = []
    {
        return std::optional<std::filesystem::path>{};
    };
    EXPECT_EQ(configDirectoryFor("/etc", "/home/u", unreadablePasswd, "/home/u/cwd", std::nullopt, true).directory,
              std::filesystem::path("/root/.config/tasksmack"));
    // A known invoking user still wins (sudo): their home's, as before.
    EXPECT_EQ(configDirectoryFor("/etc", "/root", rootsPasswd, "/cwd", "/home/u", true).directory,
              std::filesystem::path("/home/u/.config/tasksmack"));
    // Not run for another user: unchanged, XDG_CONFIG_HOME anywhere.
    EXPECT_EQ(configDirectoryFor("/etc", "/home/u", noPasswdLookup, "/cwd", std::nullopt).directory,
              std::filesystem::path("/etc/tasksmack"));
    EXPECT_EQ(configDirectoryFor(std::nullopt, "/root", noPasswdLookup, "/cwd", std::nullopt).directory,
              std::filesystem::path("/root/.config/tasksmack"));
    EXPECT_EQ(configDirectoryFor("/home/u/.xdg", "/home/u", noPasswdLookup, "/cwd", std::nullopt).directory,
              std::filesystem::path("/home/u/.xdg/tasksmack"));
}

TEST(UserConfigHelpersTest, IsInsideComparesWholeComponentsAfterNormalizing)
{
    using Platform::ConfigDirectory::isInside;
    EXPECT_TRUE(isInside("/home/u", "/home/u"));
    EXPECT_TRUE(isInside("/home/u/", "/home/u"));
    EXPECT_TRUE(isInside("/home/u/.xdg", "/home/u/"));
    EXPECT_TRUE(isInside("//home//u//x", "/home/u"));
    EXPECT_TRUE(isInside("/home/u/.", "/home/u"));
    EXPECT_FALSE(isInside("/home/u2", "/home/u"));
    EXPECT_FALSE(isInside("/home/u/..", "/home/u"));
    EXPECT_FALSE(isInside("/home/u/../v", "/home/u"));
    EXPECT_FALSE(isInside("/home", "/home/u"));
    EXPECT_FALSE(isInside("/home/U", "/home/u"));
    EXPECT_FALSE(isInside("home/u/x", "/home/u")) << "relative";
    EXPECT_FALSE(isInside("", "/home/u"));
}

TEST(UserConfigHelpersTest, ConfigDirectoryIsXdgConfigHomesWhenItIsSafe)
{
    const auto choice = configDirectoryFrom("/home/u/.xdg", "/home/u", noPasswdLookup, "/cwd");
    EXPECT_EQ(choice.directory, std::filesystem::path("/home/u/.xdg/tasksmack"));
    EXPECT_TRUE(choice.warning.empty());
    // Normalized: a "." or a "dir/.." that stays inside is fine.
    EXPECT_EQ(configDirectoryFrom("/home/u/./x/../.xdg", "/home/u", noPasswdLookup, "/cwd").directory,
              std::filesystem::path("/home/u/.xdg/tasksmack"));
}

TEST(UserConfigHelpersTest, AnUnsafeXdgConfigHomeFallsBackToHomesConfigWithAWarning)
{
    // Relative: it would follow the current directory.
    auto choice = configDirectoryFrom("relative/xdg", "/home/u", noPasswdLookup, "/cwd");
    EXPECT_EQ(choice.directory, std::filesystem::path("/home/u/.config/tasksmack"));
    EXPECT_TRUE(choice.warning.contains("Ignoring unsafe config directory")) << choice.warning;
    // Climbing out of the root with "..": still a "..", so not safe either.
    choice = configDirectoryFrom("../../etc", "/home/u", noPasswdLookup, "/cwd");
    EXPECT_EQ(choice.directory, std::filesystem::path("/home/u/.config/tasksmack"));
    EXPECT_FALSE(choice.warning.empty());
}

TEST(UserConfigHelpersTest, WithoutXdgConfigHomeTheHomeConfigIsUsed)
{
    const auto choice = configDirectoryFrom(std::nullopt, "/home/u", noPasswdLookup, "/cwd");
    EXPECT_EQ(choice.directory, std::filesystem::path("/home/u/.config/tasksmack"));
    EXPECT_TRUE(choice.warning.empty());
}

TEST(UserConfigHelpersTest, WithoutHomeThePasswdEntryThenTheCurrentDirectoryIsUsed)
{
    int lookups = 0;
    const auto passwd = [&lookups]() -> std::optional<std::filesystem::path>
    {
        ++lookups;
        return std::filesystem::path("/var/lib/u");
    };
    EXPECT_EQ(configDirectoryFrom(std::nullopt, std::nullopt, passwd, "/cwd").directory,
              std::filesystem::path("/var/lib/u/.config/tasksmack"));
    EXPECT_EQ(lookups, 1);
    // No passwd entry either: the current directory, as the last resort (absolute, so no warning).
    const auto none = []
    {
        return std::optional<std::filesystem::path>{};
    };
    const auto choice = configDirectoryFrom(std::nullopt, std::nullopt, none, "/cwd");
    EXPECT_EQ(choice.directory, std::filesystem::path("/cwd"));
    EXPECT_TRUE(choice.warning.empty());
    // And a safe XDG directory still wins over all of them.
    EXPECT_EQ(configDirectoryFrom("/x", std::nullopt, none, "/cwd").directory, std::filesystem::path("/x/tasksmack"));
}

TEST(UserConfigHelpersTest, ARelativeHomeFallsBackToTheCurrentDirectoryWithAWarning)
{
    // $HOME relative (misconfigured): its .config isn't safe to write, nor is an XDG directory judged against it.
    const auto choice = configDirectoryFrom("/home/u/.xdg", "relative-home", noPasswdLookup, "/cwd");
    EXPECT_EQ(choice.directory, std::filesystem::path("/cwd"));
    EXPECT_TRUE(choice.warning.contains("not absolute")) << choice.warning;
}
#endif

} // namespace
} // namespace App

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
