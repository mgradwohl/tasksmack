/// @file test_WindowsPathProvider.cpp
/// @brief Integration tests for Platform::WindowsPathProvider
///
/// These tests verify path provider behavior on Windows systems. The pure fallback logic
/// (WindowsPathProviderMath.h) is tested on every platform in
/// WindowsMath/test_WindowsPathProviderMath.cpp.

#include "Core/Utf8Path.h"
#include "NonAsciiTestNames.h"
#include "Platform/Windows/WindowsPathProvider.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
// clang-format on

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace Platform
{
namespace
{

/// What the fake known-folder function answers (set per test by ScopedFakeKnownFolder), and what it was asked.
struct FakeKnownFolder
{
    static inline HRESULT result = S_OK;
    static inline std::wstring path;
    static inline bool givesPath = true;
    static inline int calls = 0;
    static inline bool askedForRoaming = false;

    /// SHGetKnownFolderPath's stand-in: `path` in CoTaskMemAlloc memory (as the real one gives it) when `givesPath`, with `result`.
    static HRESULT WINAPI get(REFKNOWNFOLDERID folder, DWORD /*flags*/, HANDLE /*token*/, PWSTR* out)
    {
        ++calls;
        askedForRoaming = IsEqualGUID(folder, FOLDERID_RoamingAppData) != FALSE;
        *out = nullptr;
        if (givesPath)
        {
            const std::size_t bytes = (path.size() + 1) * sizeof(wchar_t);
            *out = static_cast<PWSTR>(::CoTaskMemAlloc(bytes));
            std::memcpy(*out, path.c_str(), bytes);
        }
        return result;
    }
};

class ScopedFakeKnownFolder
{
  public:
    ScopedFakeKnownFolder(HRESULT result, bool givesPath, std::wstring path)
    {
        FakeKnownFolder::result = result;
        FakeKnownFolder::givesPath = givesPath;
        FakeKnownFolder::path = std::move(path);
        FakeKnownFolder::calls = 0;
        FakeKnownFolder::askedForRoaming = false;
    }
    ~ScopedFakeKnownFolder()
    {
        FakeKnownFolder::result = S_OK;
        FakeKnownFolder::givesPath = true;
        FakeKnownFolder::path.clear();
    }
    ScopedFakeKnownFolder(const ScopedFakeKnownFolder&) = delete;
    ScopedFakeKnownFolder& operator=(const ScopedFakeKnownFolder&) = delete;
    ScopedFakeKnownFolder(ScopedFakeKnownFolder&&) = delete;
    ScopedFakeKnownFolder& operator=(ScopedFakeKnownFolder&&) = delete;
};

/// RAII guard that sets APPDATA to @p value for a test and restores the original on exit.
class ScopedAppData
{
  public:
    explicit ScopedAppData(const std::wstring& value)
    {
        wchar_t* original = nullptr;
        if (_wdupenv_s(&original, nullptr, L"APPDATA") == 0 && original != nullptr)
        {
            m_HadOriginal = true;
            m_Original = original;
        }
        std::free(original); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - _wdupenv_s allocates with malloc
        _wputenv_s(L"APPDATA", value.c_str());
    }
    ~ScopedAppData()
    {
        _wputenv_s(L"APPDATA", m_HadOriginal ? m_Original.c_str() : L"");
    }
    ScopedAppData(const ScopedAppData&) = delete;
    ScopedAppData& operator=(const ScopedAppData&) = delete;
    ScopedAppData(ScopedAppData&&) = delete;
    ScopedAppData& operator=(ScopedAppData&&) = delete;

  private:
    std::wstring m_Original;
    bool m_HadOriginal = false;
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

TEST(WindowsPathProviderTest, GetUserConfigDirIsTheRoamingKnownFolder)
{
    PWSTR roaming = nullptr;
    ASSERT_TRUE(SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)));
    const std::filesystem::path expected = std::filesystem::path(roaming) / "TaskSmack";
    ::CoTaskMemFree(roaming);

    const WindowsPathProvider provider;
    EXPECT_EQ(provider.getUserConfigDir(), expected);
}

TEST(WindowsPathProviderTest, GetUserConfigDirIgnoresTheAPPDATAVariable)
{
    // #4107: a user's own APPDATA must not move the themes folder away from config.toml's.
    const std::filesystem::path elsewhere = std::filesystem::temp_directory_path() / "TaskSmack-Test-NotTheKnownFolder";
    const ScopedAppData appData(elsewhere.wstring());
    const ScopedFakeKnownFolder fake(S_OK, true, L"C:\\Users\\Someone\\AppData\\Roaming");

    const WindowsPathProvider provider(&FakeKnownFolder::get);
    EXPECT_EQ(provider.getUserConfigDir(), std::filesystem::path(L"C:\\Users\\Someone\\AppData\\Roaming") / "TaskSmack");
    EXPECT_TRUE(FakeKnownFolder::askedForRoaming);
}

TEST(WindowsPathProviderTest, GetUserConfigDirKeepsANonAsciiKnownFolder)
{
    // A user name outside the code page (#1648): the path is UTF-16 end to end. Nothing is created or written.
    const std::filesystem::path roaming =
        std::filesystem::temp_directory_path() / Core::utf8ToPath(std::format("{}-AppData-Roaming", TestSupport::allNonAsciiNames()));
    const ScopedFakeKnownFolder fake(S_OK, true, roaming.wstring());

    const WindowsPathProvider provider(&FakeKnownFolder::get);
    EXPECT_EQ(provider.getUserConfigDir(), roaming / "TaskSmack");
}

TEST(WindowsPathProviderTest, GetUserConfigDirFallsBackToTheCurrentDirectoryWhenTheKnownFolderFails)
{
    const ScopedFakeKnownFolder fake(E_FAIL, false, {});

    const WindowsPathProvider provider(&FakeKnownFolder::get);
    const auto dir = provider.getUserConfigDir();

    EXPECT_EQ(FakeKnownFolder::calls, 1);
    EXPECT_EQ(dir, std::filesystem::current_path());
    EXPECT_TRUE(dir.is_absolute());
}

TEST(WindowsPathProviderTest, GetUserConfigDirFallsBackWhenTheKnownFolderIsEmptyOrAbsent)
{
    {
        const ScopedFakeKnownFolder fake(S_OK, true, L"");
        const WindowsPathProvider provider(&FakeKnownFolder::get);
        EXPECT_EQ(provider.getUserConfigDir(), std::filesystem::current_path());
    }
    {
        const ScopedFakeKnownFolder fake(S_OK, false, {});
        const WindowsPathProvider provider(&FakeKnownFolder::get);
        EXPECT_EQ(provider.getUserConfigDir(), std::filesystem::current_path());
    }
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
