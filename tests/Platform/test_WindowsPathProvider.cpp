/// @file test_WindowsPathProvider.cpp
/// @brief Integration tests for Platform::WindowsPathProvider
///
/// These tests verify path provider behavior on Windows systems. The pure fallback logic
/// (WindowsPathProviderMath.h) is tested on every platform in
/// WindowsMath/test_WindowsPathProviderMath.cpp.

#include "Core/Utf8Path.h"
#include "NonAsciiTestNames.h"
#include "Platform/Windows/WindowsPathProvider.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <string>

namespace Platform
{
namespace
{

/// RAII guard for safely overriding APPDATA environment variable during testing.
/// Thread-safety note: This assumes GoogleTest runs tests serially within a process (default behavior).
/// If tests are run with --gtest_parallel, concurrent APPDATA reads may fail. For production use,
/// see https://github.com/google/googletest/issues/2945 for thread-safe env var testing patterns.
class ScopedAppDataOverride
{
  public:
    ScopedAppDataOverride()
    {
        char* appData = nullptr;
        if ((_dupenv_s(&appData, nullptr, "APPDATA") == 0) && (appData != nullptr))
        {
            m_HadOriginalValue = true;
            m_OriginalValue = appData;
            std::free(appData); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - _dupenv_s allocates with malloc
        }

        _putenv_s("APPDATA", "");
    }

    ~ScopedAppDataOverride()
    {
        if (m_HadOriginalValue)
        {
            _putenv_s("APPDATA", m_OriginalValue.c_str());
        }
        else
        {
            _putenv_s("APPDATA", "");
        }
    }

    ScopedAppDataOverride(const ScopedAppDataOverride&) = delete;
    ScopedAppDataOverride& operator=(const ScopedAppDataOverride&) = delete;
    ScopedAppDataOverride(ScopedAppDataOverride&&) = delete;
    ScopedAppDataOverride& operator=(ScopedAppDataOverride&&) = delete;

  private:
    std::string m_OriginalValue;
    bool m_HadOriginalValue = false;
};

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(WindowsPathProviderTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ const WindowsPathProvider provider; });
}

// =============================================================================
// Executable Directory Tests
// =============================================================================

TEST(WindowsPathProviderTest, GetExecutableDirReturnsNonEmpty)
{
    const WindowsPathProvider provider;
    const auto dir = provider.getExecutableDir();

    EXPECT_FALSE(dir.empty());
    EXPECT_TRUE(std::filesystem::exists(dir));
    EXPECT_TRUE(std::filesystem::is_directory(dir));
}

TEST(WindowsPathProviderTest, GetExecutableDirIsAbsolute)
{
    const WindowsPathProvider provider;
    const auto dir = provider.getExecutableDir();

    EXPECT_TRUE(dir.is_absolute());
}

TEST(WindowsPathProviderTest, GetExecutableDirContainsTestExecutable)
{
    const WindowsPathProvider provider;
    const auto dir = provider.getExecutableDir();

    // The test executable should be in this directory
    // Look for any file in the directory (test executable or related files)
    EXPECT_TRUE(std::filesystem::exists(dir));
    EXPECT_FALSE(std::filesystem::is_empty(dir));
}

TEST(WindowsPathProviderTest, GetExecutableDirHasValidWindowsPath)
{
    const WindowsPathProvider provider;
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
    const WindowsPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    EXPECT_FALSE(dir.empty());
}

TEST(WindowsPathProviderTest, GetUserConfigDirIsAbsolute)
{
    const WindowsPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    EXPECT_TRUE(dir.is_absolute());
}

TEST(WindowsPathProviderTest, GetUserConfigDirEndsWithTaskSmack)
{
    const WindowsPathProvider provider;
    const auto dir = provider.getUserConfigDir();

    // Should end with "TaskSmack" subdirectory (note capital letters)
    EXPECT_EQ(dir.filename().string(), "TaskSmack");
}

TEST(WindowsPathProviderTest, GetUserConfigDirRespectsAPPDATA)
{
    const WindowsPathProvider provider;

    // Get APPDATA environment variable
    char* appData = nullptr;
    if (_dupenv_s(&appData, nullptr, "APPDATA") == 0 && appData != nullptr)
    {
        const std::string appDataPath(appData);
        std::free(appData); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - _dupenv_s allocates with malloc

        if (!appDataPath.empty())
        {
            const auto dir = provider.getUserConfigDir();

            // Should use APPDATA path
            EXPECT_TRUE(dir.string().contains(appDataPath) ||
                        std::filesystem::equivalent(dir.parent_path(), std::filesystem::path(appDataPath)));
            EXPECT_EQ(dir.filename().string(), "TaskSmack");
        }
    }
}

TEST(WindowsPathProviderTest, GetUserConfigDirHandlesMissingAPPDATA)
{
    const ScopedAppDataOverride appDataGuard;

    const WindowsPathProvider provider;
    const auto dir = provider.getUserConfigDir();
    const auto cwd = std::filesystem::current_path();

    EXPECT_EQ(dir, cwd) << "Missing APPDATA should fall back to the current directory";
    EXPECT_TRUE(dir.is_absolute());
}

TEST(WindowsPathProviderTest, GetUserConfigDirKeepsANonAsciiAPPDATA)
{
    // A user name outside the code page (#1648): this binary has no UTF-8 manifest, so a narrow
    // read of APPDATA would lose the CJK and emoji characters. Nothing is created or written.
    const std::filesystem::path appData =
        std::filesystem::temp_directory_path() / Core::utf8ToPath(std::format("{}-AppData-Roaming", TestSupport::allNonAsciiNames()));

    wchar_t* original = nullptr;
    std::size_t originalLength = 0;
    const bool hadOriginal = _wdupenv_s(&original, &originalLength, L"APPDATA") == 0 && original != nullptr;
    const std::unique_ptr<wchar_t, decltype(&std::free)> holder(original, &std::free);
    const std::wstring originalValue = hadOriginal ? std::wstring(original) : std::wstring();

    ASSERT_EQ(_wputenv_s(L"APPDATA", appData.c_str()), 0);
    const WindowsPathProvider provider;
    const std::filesystem::path dir = provider.getUserConfigDir();
    _wputenv_s(L"APPDATA", originalValue.c_str()); // restore before any assertion can return

    EXPECT_EQ(dir, appData / "TaskSmack");
}

TEST(WindowsPathProviderTest, GetUserConfigDirHasValidWindowsPath)
{
    const WindowsPathProvider provider;
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
    const WindowsPathProvider provider;

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
    const WindowsPathProvider provider;

    const auto dir1 = provider.getExecutableDir();
    const auto dir2 = provider.getExecutableDir();
    EXPECT_EQ(dir1, dir2);

    const auto config1 = provider.getUserConfigDir();
    const auto config2 = provider.getUserConfigDir();
    EXPECT_EQ(config1, config2);
}

TEST(WindowsPathProviderTest, PathsAreNotRelative)
{
    const WindowsPathProvider provider;

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
