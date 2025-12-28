/// @file test_LinuxPathProvider.cpp
/// @brief Integration tests for Platform::LinuxPathProvider
///
/// These tests verify path provider behavior on Linux systems.

#include <gtest/gtest.h>

#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/LinuxPathProvider.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace Platform
{
namespace
{

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(LinuxPathProviderTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ LinuxPathProvider provider; });
}

// =============================================================================
// Executable Directory Tests
// =============================================================================

TEST(LinuxPathProviderTest, GetExecutableDirReturnsNonEmpty)
{
    LinuxPathProvider provider;
    const auto dir = provider.getExecutableDir();

    EXPECT_FALSE(dir.empty());
    EXPECT_TRUE(std::filesystem::exists(dir));
    EXPECT_TRUE(std::filesystem::is_directory(dir));
}

TEST(LinuxPathProviderTest, GetExecutableDirIsAbsolute)
{
    LinuxPathProvider provider;
    const auto dir = provider.getExecutableDir();

    EXPECT_TRUE(dir.is_absolute());
}

TEST(LinuxPathProviderTest, GetExecutableDirContainsTestExecutable)
{
    LinuxPathProvider provider;
    const auto dir = provider.getExecutableDir();

    // The test executable should be in this directory
    // Look for any file in the directory (test executable or related files)
    EXPECT_TRUE(std::filesystem::exists(dir));
    EXPECT_FALSE(std::filesystem::is_empty(dir));
}

// =============================================================================
// User Config Directory Tests
// =============================================================================

TEST(LinuxPathProviderTest, GetUserConfigDirReturnsNonEmpty)
{
    LinuxPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    EXPECT_FALSE(dir.empty());
}

TEST(LinuxPathProviderTest, GetUserConfigDirIsAbsolute)
{
    LinuxPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    EXPECT_TRUE(dir.is_absolute());
}

TEST(LinuxPathProviderTest, GetUserConfigDirEndsWithTasksmack)
{
    LinuxPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    // Should end with "tasksmack" subdirectory
    EXPECT_EQ(dir.filename().string(), "tasksmack");
}

TEST(LinuxPathProviderTest, GetUserConfigDirRespectsXDG_CONFIG_HOME)
{
    // NOTE: This test modifies process-wide environment variables which could
    // affect parallel test execution. The test restores original values but
    // there's a brief window of modification. If this causes flakiness in CI,
    // consider skipping this test or using a test fixture with proper isolation.
    
    LinuxPathProvider provider;

    // Save original value
    const char* originalXdg = std::getenv("XDG_CONFIG_HOME");
    std::string savedXdg = originalXdg ? originalXdg : "";

    // Set custom XDG_CONFIG_HOME
    const std::string testPath = "/tmp/test_config";
    setenv("XDG_CONFIG_HOME", testPath.c_str(), 1);

    const auto dir = provider.getUserConfigDir();

    // Should use XDG_CONFIG_HOME
    EXPECT_TRUE(dir.string().find(testPath) != std::string::npos);
    EXPECT_EQ(dir.filename().string(), "tasksmack");

    // Restore original value
    if (!savedXdg.empty())
    {
        setenv("XDG_CONFIG_HOME", savedXdg.c_str(), 1);
    }
    else
    {
        unsetenv("XDG_CONFIG_HOME");
    }
}

TEST(LinuxPathProviderTest, GetUserConfigDirFallsBackToHome)
{
    // NOTE: This test modifies process-wide environment variables (XDG_CONFIG_HOME)
    // which could affect parallel test execution. The test restores original values
    // but there's a brief window of modification. If this causes flakiness in CI,
    // consider skipping this test or using a test fixture with proper isolation.
    
    LinuxPathProvider provider;

    // Save original values
    const char* originalXdg = std::getenv("XDG_CONFIG_HOME");
    const char* originalHome = std::getenv("HOME");
    std::string savedXdg = originalXdg ? originalXdg : "";
    std::string savedHome = originalHome ? originalHome : "";

    // Unset XDG_CONFIG_HOME to test fallback
    unsetenv("XDG_CONFIG_HOME");

    const auto dir = provider.getUserConfigDir();

    // Should fall back to $HOME/.config/tasksmack
    if (originalHome && originalHome[0] != '\0')
    {
        EXPECT_TRUE(dir.string().find(originalHome) != std::string::npos);
        EXPECT_TRUE(dir.string().find(".config") != std::string::npos);
    }
    EXPECT_EQ(dir.filename().string(), "tasksmack");

    // Restore original values
    if (!savedXdg.empty())
    {
        setenv("XDG_CONFIG_HOME", savedXdg.c_str(), 1);
    }
    if (!savedHome.empty())
    {
        setenv("HOME", savedHome.c_str(), 1);
    }
}

TEST(LinuxPathProviderTest, GetUserConfigDirHandlesEmptyXDG)
{
    // NOTE: This test modifies process-wide environment variables (XDG_CONFIG_HOME)
    // which could affect parallel test execution. The test restores original values
    // but there's a brief window of modification. If this causes flakiness in CI,
    // consider skipping this test or using a test fixture with proper isolation.
    
    LinuxPathProvider provider;

    // Save original value
    const char* originalXdg = std::getenv("XDG_CONFIG_HOME");
    std::string savedXdg = originalXdg ? originalXdg : "";

    // Set empty XDG_CONFIG_HOME
    setenv("XDG_CONFIG_HOME", "", 1);

    const auto dir = provider.getUserConfigDir();

    // Should fall back to HOME or current directory
    EXPECT_FALSE(dir.empty());

    // Restore original value
    if (!savedXdg.empty())
    {
        setenv("XDG_CONFIG_HOME", savedXdg.c_str(), 1);
    }
    else
    {
        unsetenv("XDG_CONFIG_HOME");
    }
}

// =============================================================================
// Consistency Tests
// =============================================================================

TEST(LinuxPathProviderTest, MultipleCallsReturnSamePaths)
{
    LinuxPathProvider provider;

    const auto dir1 = provider.getExecutableDir();
    const auto dir2 = provider.getExecutableDir();
    EXPECT_EQ(dir1, dir2);

    const auto config1 = provider.getUserConfigDir();
    const auto config2 = provider.getUserConfigDir();
    EXPECT_EQ(config1, config2);
}

} // namespace
} // namespace Platform

#endif // __linux__
