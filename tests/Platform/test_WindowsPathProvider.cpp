/// @file test_WindowsPathProvider.cpp
/// @brief Integration tests for Platform::WindowsPathProvider
///
/// These tests verify path provider behavior on Windows systems.

#include "Platform/Windows/WindowsPathProvider.h"
#include "Platform/Windows/WindowsPathProviderMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

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
            m_hadOriginalValue = true;
            m_originalValue = appData;
            std::free(appData);
        }

        _putenv_s("APPDATA", "");
    }

    ~ScopedAppDataOverride()
    {
        if (m_hadOriginalValue)
        {
            _putenv_s("APPDATA", m_originalValue.c_str());
        }
        else
        {
            _putenv_s("APPDATA", "");
        }
    }

    ScopedAppDataOverride(const ScopedAppDataOverride&) = delete;
    ScopedAppDataOverride& operator=(const ScopedAppDataOverride&) = delete;

  private:
    std::string m_originalValue;
    bool m_hadOriginalValue = false;
};

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
    ScopedAppDataOverride appDataGuard;

    WindowsPathProvider provider;
    const auto dir = provider.getUserConfigDir();
    const auto cwd = std::filesystem::current_path();

    EXPECT_EQ(dir, cwd) << "Missing APPDATA should fall back to the current directory";
    EXPECT_TRUE(dir.is_absolute());
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

// =============================================================================
// resolveExecutableDir / resolveUserConfigDir: pure logic extracted from
// WindowsPathProvider.cpp, with injected OS-primitive predicates so the "keeps failing"
// fallback branches - which need a genuinely broken environment to reach for real - can be
// exercised directly.
// =============================================================================

namespace
{
// Function-local static (lazily initialized on first call, inside a test body where gtest can
// still catch an exception) rather than a namespace-scope global, which clang-tidy flags as
// unsafe static storage duration: std::function's converting constructor is not guaranteed
// noexcept, and an exception thrown during static initialization before main() cannot be caught.
const PathProviderCurrentPathFn& kFailingCurrentPath()
{
    static const PathProviderCurrentPathFn fn = [](std::error_code& ec)
    {
        ec = std::make_error_code(std::errc::io_error);
        return std::filesystem::path{};
    };
    return fn;
}
} // namespace

TEST(ResolveExecutableDirTest, SucceedsOnFirstCallReturnsParentPath)
{
    const std::filesystem::path fakeExe = std::filesystem::temp_directory_path() / "sub" / "TaskSmack.exe";
    const auto fakeExeStr = fakeExe.wstring();

    const auto result = resolveExecutableDir(
        [&fakeExeStr](wchar_t* buffer, std::uint32_t size) -> std::uint32_t
        {
            if (fakeExeStr.size() >= size)
            {
                return size; // simulate truncation, matching real GetModuleFileNameW semantics
            }
            std::ranges::copy(fakeExeStr, buffer);
            return static_cast<std::uint32_t>(fakeExeStr.size());
        },
        kFailingCurrentPath());

    EXPECT_EQ(result, fakeExe.parent_path());
}

TEST(ResolveExecutableDirTest, ImmediateFailureFallsBackToCurrentPath)
{
    std::filesystem::path cwd = std::filesystem::temp_directory_path();
    const auto result = resolveExecutableDir([](wchar_t*, std::uint32_t) -> std::uint32_t { return 0; },
                                             [&cwd](std::error_code& ec)
                                             {
                                                 ec.clear();
                                                 return cwd;
                                             });

    EXPECT_EQ(result, cwd);
}

TEST(ResolveExecutableDirTest, GrowsBufferOnTruncationThenSucceeds)
{
    // Path longer than the initial 260-char buffer, forcing at least one growth iteration.
    const std::wstring longSegment(300, L'a');
    const std::filesystem::path fakeExe = std::filesystem::temp_directory_path() / longSegment / "TaskSmack.exe";
    const auto fakeExeStr = fakeExe.wstring();
    int callCount = 0;

    const auto result = resolveExecutableDir(
        [&fakeExeStr, &callCount](wchar_t* buffer, std::uint32_t size) -> std::uint32_t
        {
            ++callCount;
            if (fakeExeStr.size() >= size)
            {
                return size; // too small; caller must grow and retry
            }
            std::ranges::copy(fakeExeStr, buffer);
            return static_cast<std::uint32_t>(fakeExeStr.size());
        },
        kFailingCurrentPath());

    EXPECT_GT(callCount, 1) << "the 260-char initial buffer must have been too small for this path";
    EXPECT_EQ(result, fakeExe.parent_path());
}

TEST(ResolveExecutableDirTest, NeverFitsEvenAtLongPathLimitFallsBackToCurrentPath)
{
    std::filesystem::path cwd = std::filesystem::temp_directory_path();
    // Always report truncation, regardless of buffer size, forcing growth all the way to the
    // 32767-char long-path limit before giving up.
    const auto result = resolveExecutableDir([](wchar_t*, std::uint32_t size) -> std::uint32_t { return size; },
                                             [&cwd](std::error_code& ec)
                                             {
                                                 ec.clear();
                                                 return cwd;
                                             });

    EXPECT_EQ(result, cwd);
}

TEST(ResolveExecutableDirTest, BothPrimitivesFailingReturnsEmptyPath)
{
    const auto result = resolveExecutableDir([](wchar_t*, std::uint32_t) -> std::uint32_t { return 0; }, kFailingCurrentPath());

    EXPECT_TRUE(result.empty());
}

TEST(ResolveUserConfigDirTest, AppDataPresentAppendsTaskSmack)
{
    const auto result =
        resolveUserConfigDir([]() -> std::optional<std::string> { return R"(C:\Users\test\AppData\Roaming)"; }, kFailingCurrentPath());

    EXPECT_EQ(result, std::filesystem::path(R"(C:\Users\test\AppData\Roaming)") / "TaskSmack");
}

TEST(ResolveUserConfigDirTest, AppDataMissingFallsBackToCurrentPath)
{
    std::filesystem::path cwd = std::filesystem::temp_directory_path();
    const auto result = resolveUserConfigDir([]() -> std::optional<std::string> { return std::nullopt; },
                                             [&cwd](std::error_code& ec)
                                             {
                                                 ec.clear();
                                                 return cwd;
                                             });

    EXPECT_EQ(result, cwd);
}

TEST(ResolveUserConfigDirTest, AppDataEmptyStringFallsBackToCurrentPath)
{
    std::filesystem::path cwd = std::filesystem::temp_directory_path();
    const auto result = resolveUserConfigDir([]() -> std::optional<std::string> { return std::string{}; },
                                             [&cwd](std::error_code& ec)
                                             {
                                                 ec.clear();
                                                 return cwd;
                                             });

    EXPECT_EQ(result, cwd);
}

TEST(ResolveUserConfigDirTest, BothPrimitivesFailingReturnsEmptyPath)
{
    const auto result = resolveUserConfigDir([]() -> std::optional<std::string> { return std::nullopt; }, kFailingCurrentPath());

    EXPECT_TRUE(result.empty());
}

} // namespace
} // namespace Platform
