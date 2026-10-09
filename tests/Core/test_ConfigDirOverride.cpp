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

} // namespace
} // namespace Core::ConfigDirOverride
