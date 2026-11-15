/// @file test_ConfigDirOverride.cpp
/// @brief The TASKSMACK_CONFIG_DIR parser and resolver (#1596)

#include "Core/ConfigDirOverride.h"
#include "Mocks/HandedOverTestFolder.h"
#include "Platform/IHandedOverFolder.h"

#include <SDL3/SDL_stdinc.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace Core::ConfigDirOverride
{
namespace
{

TEST(ConfigDirOverrideTest, UnsetIsNoOverride)
{
    EXPECT_FALSE(parse(nullptr).has_value());
}

TEST(ConfigDirOverrideTest, BlankIsNoOverride)
{
    EXPECT_FALSE(parse("").has_value());
    EXPECT_FALSE(parse("   ").has_value());
    EXPECT_FALSE(parse("\t\r\n ").has_value());
}

TEST(ConfigDirOverrideTest, ValueIsTheDirectory)
{
    const auto dir = parse("tasksmack-test/run1");
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(dir.value_or(std::filesystem::path{}), std::filesystem::path("tasksmack-test/run1"));
}

TEST(ConfigDirOverrideTest, SurroundingWhitespaceIsTrimmed)
{
    const auto dir = parse("  tasksmack-test \t");
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(dir.value_or(std::filesystem::path{}), std::filesystem::path("tasksmack-test"));
}

TEST(ConfigDirOverrideTest, ValueIsReadAsUtf8)
{
    // SDL_getenv() returns UTF-8 on every platform; "tëst" must not go through the ANSI code page.
    const auto dir = parse("t\xC3\xABst");
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(dir.value_or(std::filesystem::path{}), std::filesystem::path(u8"tëst"));
}

TEST(ConfigDirOverrideTest, ResolveUsesTheOverrideWhenSet)
{
    const std::filesystem::path platformDir = std::filesystem::temp_directory_path() / "platform";
    const std::filesystem::path overrideDir = std::filesystem::temp_directory_path() / "override";
    EXPECT_EQ(resolve(overrideDir, platformDir), overrideDir);
}

TEST(ConfigDirOverrideTest, ResolveKeepsThePlatformDirWhenUnset)
{
    const std::filesystem::path platformDir = std::filesystem::temp_directory_path() / "platform";
    EXPECT_EQ(resolve(std::nullopt, platformDir), platformDir);
    EXPECT_EQ(resolve(parse("  "), platformDir), platformDir);
}

// #2728: an elevated restart's --config-dir is the launching user's directory, used as the override is, but it
// is no test hook, so the Administrator session isn't run as an unattended capture.
TEST(ConfigDirOverrideTest, AHandedOverDirectoryIsUsedButIsNoTestHook)
{
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - a copy: active() changes below
    const std::optional<std::filesystem::path> previous = active();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "handed-over";

    adoptHandedOver(dir);
    ASSERT_TRUE(active().has_value());
    EXPECT_EQ(active().value_or(std::filesystem::path{}), dir.lexically_normal());
    EXPECT_FALSE(fromTestHook());

    setActiveForTesting(dir);
    EXPECT_TRUE(fromTestHook()) << "an override of a test's own is a test hook again";
    setActiveForTesting(std::nullopt);
    EXPECT_FALSE(fromTestHook()) << "no override at all";

    setActiveForTesting(previous);
}

// #3759: a TaskSmack run as root for a user keeps that user's config folder, the less-privileged side's, so it counts as
// handed over, though no override names it; a test's own override starts clean again.
TEST(ConfigDirOverrideTest, ARootSessionsInvokingUsersFolderCountsAsHandedOver)
{
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - a copy: active() changes below
    const std::optional<std::filesystem::path> previous = active();
    setActiveForTesting(std::nullopt);
    EXPECT_FALSE(handedOver());

    markLessPrivilegedFolder();
    EXPECT_TRUE(handedOver());
    EXPECT_FALSE(active().has_value()) << "the folder is still the one the platform picks";
    EXPECT_FALSE(fromTestHook());

    EXPECT_FALSE(handedOverFolderReleased()) << "a root session holds no folder, and doesn't fail closed for it";

    setActiveForTesting(previous);
    EXPECT_FALSE(handedOver()) << "a test's override starts with nothing handed over";
}

// #3787: only a weak reference to the held folder is kept, so once its owner (main.cpp) lets it go, nothing here
// keeps it, and the hand-off says it is no longer held.
TEST(ConfigDirOverrideTest, TheHeldFolderIsOnlyEverTheOwnersToKeep)
{
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - a copy: active() changes below
    const std::optional<std::filesystem::path> previous = active();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "handed-over";
    auto folder = std::make_shared<TestSupport::HandedOverTestFolder>(dir);

    adoptHandedOver(dir, folder);
    EXPECT_TRUE(handedOver());
    EXPECT_EQ(handedOverFolder(), folder);
    EXPECT_EQ(folder.use_count(), 1) << "held by its owner alone";
    EXPECT_FALSE(handedOverFolderReleased());

    folder.reset();
    EXPECT_TRUE(handedOver()) << "still a hand-off, which fails closed";
    EXPECT_EQ(handedOverFolder(), nullptr);
    EXPECT_TRUE(handedOverFolderReleased());

    adoptHandedOver(dir);
    EXPECT_EQ(handedOverFolder(), nullptr) << "handed over with no held folder at all";
    EXPECT_TRUE(handedOverFolderReleased());
    setActiveForTesting(previous);
    EXPECT_FALSE(handedOver());
    EXPECT_EQ(handedOverFolder(), nullptr);
    EXPECT_FALSE(handedOverFolderReleased());
}

// #3774: a handed-over folder named with a trailing separator is the same folder as without one, to ConfigDirOverride
// and to the held folder alike, so UserConfig's comparison of the two doesn't refuse every save.
TEST(ConfigDirOverrideTest, AHandedOverFolderIsKeptWithoutATrailingSeparator)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "handed-over";
    std::filesystem::path withSeparator = dir;
    withSeparator += std::filesystem::path::preferred_separator;
    EXPECT_EQ(Platform::handedOverFolderPath(withSeparator), Platform::handedOverFolderPath(dir));
    EXPECT_TRUE(Platform::handedOverFolderPath(withSeparator).has_filename());
    EXPECT_EQ(Platform::handedOverFolderPath(dir.root_path()), dir.root_path()) << "a root keeps its separator";

    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - a copy: active() changes below
    const std::optional<std::filesystem::path> previous = active();
    adoptHandedOver(withSeparator);
    EXPECT_EQ(active().value_or(std::filesystem::path{}), Platform::handedOverFolderPath(dir));
    EXPECT_EQ(active().value_or(std::filesystem::path{}) / "config.toml", Platform::handedOverFolderPath(dir) / "config.toml");
    setActiveForTesting(previous);
}

/// In a death test's own child: TASKSMACK_CONFIG_DIR set to @p envDir, then a hand-off of @p handedDir, before
/// anything reads active(). Exits 0 when active() is the hand-off and nothing made @p envDir.
[[noreturn]] void handOffInChild(const std::filesystem::path& envDir, const std::filesystem::path& handedDir)
{
    const auto utf8 = envDir.u8string();
    static_cast<void>(SDL_setenv_unsafe(std::string(ENV_VAR).c_str(), std::string(utf8.begin(), utf8.end()).c_str(), 1));
    adoptHandedOver(handedDir);
    const bool handOffUsed =
        active() == std::optional<std::filesystem::path>(handedDir.lexically_normal()) && handedOver() && !fromTestHook();
    std::error_code ec;
    const bool envDirUntouched = !std::filesystem::exists(envDir, ec) && !ec;
    std::exit(handOffUsed && envDirUntouched ? 0 : 1); // NOLINT(concurrency-mt-unsafe) - the death test's own child
}

// #3780: a hand-off session never reads TASKSMACK_CONFIG_DIR, so the directory it names is never made, as an
// elevated TaskSmack would make it. Run in a child process (the threadsafe style re-executes this test), where
// nothing has read active() yet: in this one, earlier tests have.
TEST(ConfigDirOverrideDeathTest, AHandOffNeverTouchesTheTestHookDirectory)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::filesystem::path temp = std::filesystem::temp_directory_path();
    const std::filesystem::path envDir =
        temp / std::format("TaskSmack.HandOffEnvDir.{}", std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path handedDir = temp / "TaskSmack.HandedOver";
    EXPECT_EXIT(handOffInChild(envDir, handedDir), ::testing::ExitedWithCode(0), "");
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(envDir, ec));
}

} // namespace
} // namespace Core::ConfigDirOverride
