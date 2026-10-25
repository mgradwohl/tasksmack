// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

#include "App/UserConfigHelpers.h"
#include "Domain/SamplingConfig.h"

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
