/// @file test_ConfigDirOverride.cpp
/// @brief The TASKSMACK_CONFIG_DIR parser and resolver (#1596)

#include "Core/ConfigDirOverride.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

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

    setActiveForTesting(previous);
    EXPECT_FALSE(handedOver()) << "a test's override starts with nothing handed over";
}

} // namespace
} // namespace Core::ConfigDirOverride
