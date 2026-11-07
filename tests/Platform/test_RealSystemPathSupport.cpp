// The path comparison the CI real-system tests check images with (tests/Support/CiRealSystemWindows.h; #3490), run
// everywhere: unlike those tests it changes nothing, so its suite isn't a CiRealSystem* one. CMake's
// $<TARGET_FILE> gives forward slashes, QueryFullProcessImageNameW backslashes and long names, and a runner's
// %TEMP% may be spelled with an 8.3 component; all of them name the same file.

#include "Support/CiRealSystemWindows.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <filesystem>
#include <string>

namespace
{

using TestSupport::CiRealSystem::normalisedPath;
using TestSupport::CiRealSystem::samePath;
using TestSupport::CiRealSystem::sameText;
using TestSupport::CiRealSystem::ScratchFolder;
using TestSupport::CiRealSystem::uniqueSuffix;

TEST(RealSystemPathTest, SlashesAndCaseDontMatter)
{
    EXPECT_TRUE(samePath(L"C:/NoSuchFolder/Build/TaskSmackCiService.exe", L"c:\\nosuchfolder\\build\\TASKSMACKCISERVICE.EXE"));
    EXPECT_TRUE(samePath(L"C:\\NoSuchFolder\\Build\\.\\x\\..\\Tool.exe", L"C:/NoSuchFolder/Build/Tool.exe"));
    EXPECT_EQ(normalisedPath(L"C:/NoSuchFolder/a.exe"), L"C:\\NoSuchFolder\\a.exe");
    EXPECT_FALSE(samePath(L"C:/NoSuchFolder/a.exe", L"C:/NoSuchFolder/b.exe"));
    EXPECT_FALSE(samePath(L"C:/NoSuchFolder/a.exe", L"C:/NoSuchFolder/a.exe.bak"));
}

TEST(RealSystemPathTest, SameTextComparesNamesAsTheyStand)
{
    EXPECT_TRUE(sameText(L".DLL", L".dll"));
    EXPECT_FALSE(sameText(L"a/b", L"a\\b")) << "a name, not a path";
}

TEST(RealSystemPathTest, AnExistingShortNameMatchesItsLongName)
{
    // A folder of the test's own, with a name too long for 8.3, and a file in it.
    const ScratchFolder folder(std::filesystem::temp_directory_path() / (L"TaskSmack Real System Path Test " + uniqueSuffix()));
    const std::filesystem::path file = folder.path() / L"A long file name.txt";
    HANDLE created = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(created, INVALID_HANDLE_VALUE) << GetLastError();
    CloseHandle(created);
    std::wstring shortName(MAX_PATH, L'\0');
    const DWORD length = GetShortPathNameW(file.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    ASSERT_GT(length, 0U) << GetLastError();
    shortName.resize(length);
    EXPECT_TRUE(samePath(shortName, file.wstring()));
    EXPECT_TRUE(samePath(std::filesystem::path(shortName).generic_wstring(), file.wstring())) << "short and forward slashes";
    if (sameText(shortName, file.wstring()))
    {
        GTEST_SKIP() << "this volume makes no 8.3 names, so the short spelling is the long one";
    }
}

} // namespace
