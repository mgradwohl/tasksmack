/// @file test_WindowsFileLocation.cpp
/// @brief Open file location on Windows (#3426): Explorer asked through SHOpenFolderAndSelectItems, then
/// explorer.exe /select, and WindowsProcessActions::openFileLocation() reading the path it shows.
///
/// Never the real shell calls: every table here is fakes, so no test opens Explorer or any window. The
/// process fakes hand out this test process opened for query only, as test_WindowsProcessActionsSeams does.

#include "Platform/IProcessActions.h"
#include "Platform/Windows/WindowsFileLocation.h"
#include "Platform/Windows/WindowsProcessActions.h"

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

#include <algorithm>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

/// What the faked calls do and saw. Function pointers can't capture, so the fakes share this state;
/// each test starts from reset().
struct FakeShell
{
    static inline HRESULT selectResult = S_OK;
    static inline DWORD startError = ERROR_SUCCESS;
    static inline bool noExplorer = false;                // explorerPath() answers empty, as when the Windows folder can't be read
    static inline std::vector<std::wstring> selected;     // each path selectInFolder() was given
    static inline std::vector<std::wstring> applications; // each application startProcess() was given
    static inline std::vector<std::wstring> commandLines; // and its command line
    static inline bool elevated = false;                  // isElevated()'s answer (#3669)
    static inline std::string unelevatedFailure;          // selectUnelevated()'s answer: empty, it ran
    static inline std::vector<std::wstring> unelevated;   // each explorer and arguments selectUnelevated() was given

    static void reset()
    {
        selectResult = S_OK;
        startError = ERROR_SUCCESS;
        noExplorer = false;
        selected.clear();
        applications.clear();
        commandLines.clear();
        elevated = false;
        unelevatedFailure.clear();
        unelevated.clear();
    }

    static HRESULT selectInFolder(const std::wstring& path)
    {
        selected.push_back(path);
        return selectResult;
    }
    static DWORD startProcess(const std::wstring& application, std::wstring& commandLine)
    {
        applications.push_back(application);
        commandLines.push_back(commandLine);
        return startError;
    }
    static std::wstring explorerPath()
    {
        return noExplorer ? std::wstring{} : std::wstring(L"C:\\Windows\\explorer.exe");
    }

    static bool isElevated()
    {
        return elevated;
    }
    static std::string selectUnelevated(const std::wstring& explorer, const std::wstring& commandArguments)
    {
        unelevated.push_back(explorer + L" " + commandArguments);
        return unelevatedFailure;
    }

    [[nodiscard]] static Windows::FileLocationFunctions functions()
    {
        return {
            .selectInFolder = &selectInFolder,
            .startProcess = &startProcess,
            .explorerPath = &explorerPath,
            .isElevated = &isElevated,
            .selectUnelevated = &selectUnelevated,
        };
    }
};

/// The process side: OpenProcess and GetProcessTimes as test_WindowsProcessActionsSeams fakes them, and
/// QueryFullProcessImageNameW answering a path of the test's choosing.
struct FakeProcess
{
    static constexpr DWORD TARGET_TICKS = 4321;
    static inline std::wstring imagePath;
    static inline DWORD queryError = ERROR_SUCCESS; // QueryFullProcessImageNameW fails with this, when not 0
    static inline DWORD openError = ERROR_SUCCESS;  // OpenProcess fails with this, when not 0
    static inline int opens = 0;

    static void reset()
    {
        imagePath = L"C:\\Program Files\\Ünïcödé Tools\\app, v2.exe";
        queryError = ERROR_SUCCESS;
        openError = ERROR_SUCCESS;
        opens = 0;
    }

    static HANDLE WINAPI openProcess(DWORD /*access*/, BOOL /*inherit*/, DWORD /*pid*/)
    {
        ++opens;
        if (openError != ERROR_SUCCESS)
        {
            SetLastError(openError);
            return nullptr;
        }
        return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    }
    static BOOL WINAPI
    getProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
    {
        creation->dwLowDateTime = TARGET_TICKS;
        creation->dwHighDateTime = 0;
        return TRUE;
    }
    static BOOL WINAPI queryFullProcessImageName(HANDLE /*process*/, DWORD /*flags*/, LPWSTR path, PDWORD size)
    {
        if (queryError != ERROR_SUCCESS)
        {
            SetLastError(queryError);
            return FALSE;
        }
        if (*size <= imagePath.size())
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        std::ranges::copy(imagePath, path);
        path[imagePath.size()] = L'\0'; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the API's buffer
        *size = static_cast<DWORD>(imagePath.size());
        return TRUE;
    }

    [[nodiscard]] static WindowsProcessFunctions functions()
    {
        WindowsProcessFunctions fns{};
        fns.openProcess = &openProcess;
        fns.getProcessTimes = &getProcessTimes;
        fns.queryFullProcessImageName = &queryFullProcessImageName;
        fns.fileLocation = FakeShell::functions();
        return fns;
    }
};

constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = FakeProcess::TARGET_TICKS};

class WindowsFileLocationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeShell::reset();
        FakeProcess::reset();
    }
};

TEST_F(WindowsFileLocationTest, TheCommandLineQuotesThePathWhole)
{
    EXPECT_EQ(Windows::explorerSelectCommandLine(L"C:\\Program Files\\My App, v2\\app.exe"),
              L"explorer.exe /select,\"C:\\Program Files\\My App, v2\\app.exe\"");
    EXPECT_EQ(Windows::explorerSelectCommandLine(L"C:\\Tools\\édition 東京.exe"), L"explorer.exe /select,\"C:\\Tools\\édition 東京.exe\"");
}

TEST_F(WindowsFileLocationTest, TheShellIsAskedWithThePathExactlyAndExplorerIsNotStarted)
{
    const std::wstring path = L"C:\\Program Files\\Ünïcödé Tools\\東京 app.exe";
    const ProcessActionResult result = Windows::revealInExplorer(FakeShell::functions(), path);

    EXPECT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(FakeShell::selected.size(), 1U);
    EXPECT_EQ(FakeShell::selected.front(), path);
    EXPECT_TRUE(FakeShell::applications.empty());
}

TEST_F(WindowsFileLocationTest, AFailedShellCallFallsBackToExplorerSelectQuoted)
{
    FakeShell::selectResult = E_FAIL;
    const std::wstring path = L"C:\\Program Files\\Ünïcödé Tools\\app, v2.exe";
    const ProcessActionResult result = Windows::revealInExplorer(FakeShell::functions(), path);

    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(FakeShell::selected.size(), 1U);
    ASSERT_EQ(FakeShell::applications.size(), 1U);
    EXPECT_EQ(FakeShell::applications.front(), L"C:\\Windows\\explorer.exe");
    EXPECT_EQ(FakeShell::commandLines.front(), L"explorer.exe /select,\"C:\\Program Files\\Ünïcödé Tools\\app, v2.exe\"");
}

TEST_F(WindowsFileLocationTest, BothFailingIsReportedAsAnError)
{
    FakeShell::selectResult = E_FAIL;
    FakeShell::startError = ERROR_FILE_NOT_FOUND;
    const ProcessActionResult result = Windows::revealInExplorer(FakeShell::functions(), L"C:\\app.exe");

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied);
    EXPECT_TRUE(result.errorMessage.starts_with("Explorer couldn't be started: ")) << result.errorMessage;
}

TEST_F(WindowsFileLocationTest, NoExplorerPathIsReportedWithTheShellsError)
{
    FakeShell::selectResult = E_FAIL;
    FakeShell::noExplorer = true;
    const ProcessActionResult result = Windows::revealInExplorer(FakeShell::functions(), L"C:\\app.exe");

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("0x80004005"), std::string::npos) << result.errorMessage;
    EXPECT_TRUE(FakeShell::applications.empty());
}

TEST_F(WindowsFileLocationTest, APathWithAQuoteOrNoPathIsRefusedBeforeAnythingIsAsked)
{
    EXPECT_FALSE(Windows::revealInExplorer(FakeShell::functions(), L"C:\\a\" /e,\"C:\\b.exe").success);
    EXPECT_FALSE(Windows::revealInExplorer(FakeShell::functions(), L"").success);
    EXPECT_TRUE(FakeShell::selected.empty());
    EXPECT_TRUE(FakeShell::applications.empty());
}

TEST_F(WindowsFileLocationTest, TheCapabilityIsSetByTheRealTableAndNotWithoutTheCalls)
{
    // The real table is only asked what it offers here: openFileLocation() is never called on it.
    EXPECT_TRUE(WindowsProcessActions(realWindowsProcessFunctions()).actionCapabilities().canOpenFileLocation);
    EXPECT_TRUE(WindowsProcessActions(FakeProcess::functions()).actionCapabilities().canOpenFileLocation);

    WindowsProcessFunctions noShell = FakeProcess::functions();
    noShell.fileLocation = {};
    WindowsProcessActions withoutShell(noShell);
    EXPECT_FALSE(withoutShell.actionCapabilities().canOpenFileLocation);
    EXPECT_FALSE(withoutShell.openFileLocation(TARGET).success);
    EXPECT_EQ(FakeProcess::opens, 0);

    WindowsProcessFunctions noQuery = FakeProcess::functions();
    noQuery.queryFullProcessImageName = nullptr;
    EXPECT_FALSE(WindowsProcessActions(noQuery).actionCapabilities().canOpenFileLocation);
}

TEST_F(WindowsFileLocationTest, OpenFileLocationShowsTheProcesssImagePathExactly)
{
    WindowsProcessActions actions(FakeProcess::functions());
    const ProcessActionResult result = actions.openFileLocation(TARGET);

    EXPECT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(FakeShell::selected.size(), 1U);
    EXPECT_EQ(FakeShell::selected.front(), FakeProcess::imagePath);
}

TEST_F(WindowsFileLocationTest, OpenFileLocationFallsBackToExplorerWithTheImagePathQuoted)
{
    FakeShell::selectResult = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    WindowsProcessActions actions(FakeProcess::functions());
    const ProcessActionResult result = actions.openFileLocation(TARGET);

    EXPECT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(FakeShell::commandLines.size(), 1U);
    EXPECT_EQ(FakeShell::commandLines.front(), L"explorer.exe /select,\"" + FakeProcess::imagePath + L"\"");
}

TEST_F(WindowsFileLocationTest, AnUnreadablePathIsAnErrorAndExplorerIsNotAsked)
{
    FakeProcess::queryError = ERROR_GEN_FAILURE;
    WindowsProcessActions actions(FakeProcess::functions());
    const ProcessActionResult result = actions.openFileLocation(TARGET);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.starts_with("Its executable can't be read: ")) << result.errorMessage;
    EXPECT_TRUE(FakeShell::selected.empty());
    EXPECT_TRUE(FakeShell::applications.empty());
}

TEST_F(WindowsFileLocationTest, AProcessThatCantBeOpenedIsAnError)
{
    FakeProcess::openError = ERROR_ACCESS_DENIED;
    WindowsProcessActions actions(FakeProcess::functions());
    const ProcessActionResult result = actions.openFileLocation(TARGET);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied); // the user's own Explorer: the elevated helper never serves it
    EXPECT_EQ(result.errorMessage, "Its executable can't be read: access is denied");
    EXPECT_TRUE(FakeShell::selected.empty());
}

TEST_F(WindowsFileLocationTest, AReusedPidAndTheSystemProcessAreRefused)
{
    WindowsProcessActions actions(FakeProcess::functions());
    EXPECT_FALSE(actions.openFileLocation({.pid = 4242, .startTimeTicks = FakeProcess::TARGET_TICKS + 1}).success);

    const ProcessActionResult system = actions.openFileLocation({.pid = 4, .startTimeTicks = FakeProcess::TARGET_TICKS});
    EXPECT_FALSE(system.success);
    EXPECT_EQ(system.errorMessage, "The System process has no executable file");
    EXPECT_EQ(actions.openFileLocation({.pid = 0, .startTimeTicks = FakeProcess::TARGET_TICKS}).errorMessage, "Invalid PID");
    EXPECT_TRUE(FakeShell::selected.empty());
    EXPECT_TRUE(FakeShell::applications.empty());
}

// #3669: an elevated TaskSmack asks the signed-in user's Explorer to run explorer.exe /select, and never calls
// SHOpenFolderAndSelectItems or starts explorer.exe itself, either of which could run Explorer as administrator.
TEST_F(WindowsFileLocationTest, ElevatedItAsksTheSignedInUsersExplorer)
{
    FakeShell::reset();
    FakeShell::elevated = true;
    const ProcessActionResult result = Windows::revealInExplorer(FakeShell::functions(), L"C:\\Program Files\\App, Inc\\app.exe");
    EXPECT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(FakeShell::unelevated.size(), 1U);
    EXPECT_EQ(FakeShell::unelevated.front(), L"C:\\Windows\\explorer.exe /select,\"C:\\Program Files\\App, Inc\\app.exe\"");
    EXPECT_TRUE(FakeShell::selected.empty()) << "SHOpenFolderAndSelectItems from an elevated process";
    EXPECT_TRUE(FakeShell::applications.empty()) << "explorer.exe started elevated";
}

TEST_F(WindowsFileLocationTest, ElevatedWithNoWayAsTheUserItWithholdsAndNamesThePath)
{
    FakeShell::reset();
    FakeShell::elevated = true;
    FakeShell::unelevatedFailure = "Explorer isn't running";
    ProcessActionResult result = Windows::revealInExplorer(FakeShell::functions(), L"C:\\Tools\\app.exe");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.withheldPath, "C:\\Tools\\app.exe");
    EXPECT_TRUE(result.errorMessage.contains("C:\\Tools\\app.exe")) << result.errorMessage;
    EXPECT_FALSE(result.permissionDenied);
    EXPECT_TRUE(FakeShell::selected.empty());
    EXPECT_TRUE(FakeShell::applications.empty()) << "no explorer.exe /select fallback while elevated";

    // No unelevated way at all, or no explorer.exe path: the same.
    FakeShell::reset();
    FakeShell::elevated = true;
    Windows::FileLocationFunctions none = FakeShell::functions();
    none.selectUnelevated = nullptr;
    result = Windows::revealInExplorer(none, L"C:\\Tools\\app.exe");
    EXPECT_EQ(result.withheldPath, "C:\\Tools\\app.exe");
    EXPECT_TRUE(FakeShell::selected.empty());
    EXPECT_TRUE(FakeShell::applications.empty());
    FakeShell::noExplorer = true;
    result = Windows::revealInExplorer(FakeShell::functions(), L"C:\\Tools\\app.exe");
    EXPECT_EQ(result.withheldPath, "C:\\Tools\\app.exe");
    EXPECT_TRUE(FakeShell::unelevated.empty());

    // Not elevated: as before, and nothing is withheld.
    FakeShell::reset();
    result = Windows::revealInExplorer(FakeShell::functions(), L"C:\\Tools\\app.exe");
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.withheldPath.empty());
    EXPECT_EQ(FakeShell::selected.size(), 1U);
    EXPECT_TRUE(FakeShell::unelevated.empty());
    EXPECT_EQ(Windows::explorerSelectArguments(L"C:\\a b\\c.exe"), L"/select,\"C:\\a b\\c.exe\"");
}

} // namespace
} // namespace Platform
