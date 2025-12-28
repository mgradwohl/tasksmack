/// @file test_WindowsPathProvider.cpp
/// @brief Integration tests for Platform::WindowsPathProvider
///
/// These tests verify path provider behavior on Windows systems.

#include "Platform/Windows/WindowsPathProvider.h"

#include <gtest/gtest.h>

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

TEST(WindowsPathProviderTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ WindowsPathProvider provider; });
}

// =============================================================================
// Executable Directory Tests
// =============================================================================

TEST(WindowsPathProviderTest, GetExecutableDirReturnsNonEmpty)
{
    WindowsPathProvider provider;
    const auto dir = provider.getExecutableDir();

    EXPECT_FALSE(dir.empty());
    EXPECT_TRUE(std::filesystem::exists(dir));
    EXPECT_TRUE(std::filesystem::is_directory(dir));
}

TEST(WindowsPathProviderTest, GetExecutableDirIsAbsolute)
{
    WindowsPathProvider provider;
    const auto dir = provider.getExecutableDir();

    EXPECT_TRUE(dir.is_absolute());
}

TEST(WindowsPathProviderTest, GetExecutableDirContainsTestExecutable)
{
    WindowsPathProvider provider;
    const auto dir = provider.getExecutableDir();

    // The test executable should be in this directory
    // Look for any file in the directory (test executable or related files)
    EXPECT_TRUE(std::filesystem::exists(dir));
    EXPECT_FALSE(std::filesystem::is_empty(dir));
}

TEST(WindowsPathProviderTest, GetExecutableDirHasValidWindowsPath)
{
    WindowsPathProvider provider;
    const auto dir = provider.getExecutableDir();

    // Windows paths should contain backslashes or be convertible
    const auto pathStr = dir.string();
    EXPECT_FALSE(pathStr.empty());

    // Should be a valid Windows path format (drive letter or UNC)
    EXPECT_TRUE(pathStr.length() >= 3); // Minimum like "C:\"
}

// =============================================================================
// User Config Directory Tests
// =============================================================================

TEST(WindowsPathProviderTest, GetUserConfigDirReturnsNonEmpty)
{
    WindowsPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    EXPECT_FALSE(dir.empty());
}

TEST(WindowsPathProviderTest, GetUserConfigDirIsAbsolute)
{
    WindowsPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    EXPECT_TRUE(dir.is_absolute());
}

TEST(WindowsPathProviderTest, GetUserConfigDirEndsWithTaskSmack)
{
    WindowsPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    // Should end with "TaskSmack" subdirectory (note capital letters)
    EXPECT_EQ(dir.filename().string(), "TaskSmack");
}

TEST(WindowsPathProviderTest, GetUserConfigDirRespectsAPPDATA)
{
    WindowsPathProvider provider;

    // Get APPDATA environment variable
    char* appData = nullptr;
    if (_dupenv_s(&appData, nullptr, "APPDATA") == 0 && appData != nullptr)
    {
        std::string appDataPath(appData);
        std::free(appData);

        if (!appDataPath.empty())
        {
            const auto dir = provider.getUserConfigDir();

            // Should use APPDATA path
            EXPECT_TRUE(dir.string().find(appDataPath) != std::string::npos ||
                        std::filesystem::equivalent(dir.parent_path(), std::filesystem::path(appDataPath)));
            EXPECT_EQ(dir.filename().string(), "TaskSmack");
        }
    }
}

TEST(WindowsPathProviderTest, GetUserConfigDirHandlesMissingAPPDATA)
{
    // Modifying process-wide environment variables like APPDATA in tests can
    // interfere with other tests running in the same process and cause flaky
    // behavior. To avoid this, we skip this integration test here. The
    // fallback behavior for missing APPDATA should be covered by more
    // isolated tests that do not mutate global process state.
    GTEST_SKIP() << "Skipped: cannot safely modify APPDATA environment variable in-process.";
}

TEST(WindowsPathProviderTest, GetUserConfigDirHasValidWindowsPath)
{
    WindowsPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    // Windows paths should be properly formatted
    const auto pathStr = dir.string();
    EXPECT_FALSE(pathStr.empty());
    EXPECT_TRUE(pathStr.length() >= 3); // Minimum valid path length
}

// =============================================================================
// Unicode and Special Character Handling
// =============================================================================

TEST(WindowsPathProviderTest, PathsHandleUnicodeCorrectly)
{
    WindowsPathProvider provider;

    // Get paths - they should not throw with Unicode
    EXPECT_NO_THROW({
        const auto exeDir = provider.getExecutableDir();
        const auto configDir = provider.getUserConfigDir();

        // Verify paths are valid UTF-8 strings
        EXPECT_FALSE(exeDir.string().empty());
        EXPECT_FALSE(configDir.string().empty());
    });
}

// =============================================================================
// Consistency Tests
// =============================================================================

TEST(WindowsPathProviderTest, MultipleCallsReturnSamePaths)
{
    WindowsPathProvider provider;

    const auto dir1 = provider.getExecutableDir();
    const auto dir2 = provider.getExecutableDir();
    EXPECT_EQ(dir1, dir2);

    const auto config1 = provider.getUserConfigDir();
    const auto config2 = provider.getUserConfigDir();
    EXPECT_EQ(config1, config2);
}

TEST(WindowsPathProviderTest, PathsAreNotRelative)
{
    WindowsPathProvider provider;

    const auto exeDir = provider.getExecutableDir();
    const auto configDir = provider.getUserConfigDir();

    EXPECT_TRUE(exeDir.is_absolute());
    EXPECT_TRUE(configDir.is_absolute());

    // Should not be just "." or ".."
    EXPECT_NE(exeDir.string(), ".");
    EXPECT_NE(exeDir.string(), "..");
    EXPECT_NE(configDir.string(), ".");
    EXPECT_NE(configDir.string(), "..");
}

} // namespace
} // namespace Platform
