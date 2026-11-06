/// @file test_WindowsRunProgram.cpp
/// @brief Run new task on Windows (#3427): the command line split and opened with ShellExecuteExW, the
/// runas verb for administrator, and a dismissed UAC prompt reported as cancelled.
///
/// Never the real calls: every table here is fakes, so no test starts a program or raises UAC. The real
/// table is only asked what it offers (actionCapabilities()), never to run anything.

#include "Platform/IProcessActions.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsRunProgram.h"

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

#include <cstddef>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

/// What the faked calls do and saw. Function pointers can't capture, so the fakes share this state;
/// each test starts from reset().
struct FakeShellRun
{
    static inline DWORD error = ERROR_SUCCESS;         // what shellExecute() answers
    static inline bool elevated = false;               // what isElevated() answers
    static inline int elevationChecks = 0;             // how often isElevated() was asked
    static inline std::vector<Windows::ShellRun> runs; // each call shellExecute() was given

    static void reset()
    {
        error = ERROR_SUCCESS;
        elevated = false;
        elevationChecks = 0;
        runs.clear();
    }

    static DWORD shellExecute(const Windows::ShellRun& run)
    {
        runs.push_back(run);
        return error;
    }
    static bool isElevated()
    {
        ++elevationChecks;
        return elevated;
    }

    [[nodiscard]] static Windows::RunProgramFunctions functions()
    {
        return {.shellExecute = &shellExecute, .isElevated = &isElevated};
    }

    /// A process-actions table with only the Run new task calls: nothing else is reached.
    [[nodiscard]] static WindowsProcessFunctions processFunctions()
    {
        WindowsProcessFunctions fns{};
        fns.runProgram = functions();
        return fns;
    }
};

class WindowsRunProgramTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeShellRun::reset();
    }
};

TEST_F(WindowsRunProgramTest, TheVerbIsRunasOnlyForAdministratorWhenNotElevated)
{
    EXPECT_STREQ(Windows::runVerb(false, false), L"open");
    EXPECT_STREQ(Windows::runVerb(true, false), L"runas");
    EXPECT_STREQ(Windows::runVerb(false, true), L"open");
    EXPECT_STREQ(Windows::runVerb(true, true), L"open"); // elevated already: the task inherits it
}

TEST_F(WindowsRunProgramTest, AProgramIsOpenedWithItsParametersAsTyped)
{
    const ProcessActionResult result = Windows::runShellProgram(
        FakeShellRun::functions(), false, {.commandLine = R"(  notepad "C:\My Files\a b.txt"  )", .asAdministrator = false});
    EXPECT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(FakeShellRun::runs.size(), 1U);
    EXPECT_EQ(FakeShellRun::runs[0].verb, L"open");
    EXPECT_EQ(FakeShellRun::runs[0].file, L"notepad");
    EXPECT_EQ(FakeShellRun::runs[0].parameters, LR"("C:\My Files\a b.txt")");
}

TEST_F(WindowsRunProgramTest, AQuotedProgramWithSpacesAndUnicodeIsOneFile)
{
    // "C:\Program Files\Café ☕\日本.exe" --name "Zoë ünï"
    const std::string line =
        "\"C:\\Program Files\\Caf\xC3\xA9 \xE2\x98\x95\\\xE6\x97\xA5\xE6\x9C\xAC.exe\" --name \"Zo\xC3\xAB \xC3\xBCn\xC3\xAF\"";
    const ProcessActionResult result =
        Windows::runShellProgram(FakeShellRun::functions(), false, {.commandLine = line, .asAdministrator = false});
    EXPECT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(FakeShellRun::runs.size(), 1U);
    EXPECT_EQ(FakeShellRun::runs[0].file, L"C:\\Program Files\\Caf\u00E9 \u2615\\\u65E5\u672C.exe");
    EXPECT_EQ(FakeShellRun::runs[0].parameters, L"--name \"Zo\u00EB \u00FCn\u00EF\"");
}

TEST_F(WindowsRunProgramTest, DocumentsAddressesAndShellFoldersAreOpenedAsTyped)
{
    // %WINDIR% is the real call's to expand (SEE_MASK_DOENVSUBST), so it is handed on as typed.
    const std::vector<std::wstring> targets{
        LR"(C:\Users\Public\notes.txt)",
        L"https://example.com/?q=a",
        L"shell:startup",
        LR"(%WINDIR%\notepad.exe)",
    };
    for (const char* target : {R"(C:\Users\Public\notes.txt)", "https://example.com/?q=a", "shell:startup", R"(%WINDIR%\notepad.exe)"})
    {
        EXPECT_TRUE(Windows::runShellProgram(FakeShellRun::functions(), false, {.commandLine = target, .asAdministrator = false}).success);
    }
    ASSERT_EQ(FakeShellRun::runs.size(), targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        EXPECT_EQ(FakeShellRun::runs[i].verb, L"open");
        EXPECT_EQ(FakeShellRun::runs[i].file, targets[i]);
        EXPECT_TRUE(FakeShellRun::runs[i].parameters.empty());
    }
}

TEST_F(WindowsRunProgramTest, AsAdministratorUsesRunasUnlessAlreadyElevated)
{
    EXPECT_TRUE(Windows::runShellProgram(FakeShellRun::functions(), false, {.commandLine = "cmd /k ver", .asAdministrator = true}).success);
    EXPECT_TRUE(Windows::runShellProgram(FakeShellRun::functions(), true, {.commandLine = "cmd /k ver", .asAdministrator = true}).success);
    ASSERT_EQ(FakeShellRun::runs.size(), 2U);
    EXPECT_EQ(FakeShellRun::runs[0].verb, L"runas");
    EXPECT_EQ(FakeShellRun::runs[0].file, L"cmd");
    EXPECT_EQ(FakeShellRun::runs[0].parameters, L"/k ver");
    EXPECT_EQ(FakeShellRun::runs[1].verb, L"open");
}

TEST_F(WindowsRunProgramTest, ADismissedUacPromptIsCancelledNotAnError)
{
    FakeShellRun::error = ERROR_CANCELLED;
    const ProcessActionResult result =
        Windows::runShellProgram(FakeShellRun::functions(), false, {.commandLine = "notepad", .asAdministrator = true});
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(result.permissionDenied);
    EXPECT_EQ(result.errorMessage, "the administrator prompt was cancelled");
}

TEST_F(WindowsRunProgramTest, OtherFailuresAreErrorsSayingWhatWasWrong)
{
    const auto failure = [](DWORD error, const char* command)
    {
        FakeShellRun::error = error;
        return Windows::runShellProgram(FakeShellRun::functions(), false, {.commandLine = command, .asAdministrator = false});
    };
    const ProcessActionResult missing = failure(ERROR_FILE_NOT_FOUND, "notepadd --x");
    EXPECT_FALSE(missing.success);
    EXPECT_FALSE(missing.cancelled);
    EXPECT_EQ(missing.errorMessage, "Windows can't find notepadd. Check the name and try again");
    EXPECT_EQ(failure(ERROR_PATH_NOT_FOUND, R"("C:\No Such\x.exe")").errorMessage,
              R"(Windows can't find C:\No Such\x.exe. Check the name and try again)");
    EXPECT_EQ(failure(ERROR_NO_ASSOCIATION, "a.unknownext").errorMessage, "No app is set to open a.unknownext");
    EXPECT_EQ(failure(ERROR_BAD_EXE_FORMAT, "x.exe").errorMessage, "x.exe isn't a program Windows can run");
    EXPECT_EQ(failure(ERROR_ACCESS_DENIED, "x.exe").errorMessage, "Windows refused to open x.exe: access is denied");
    const ProcessActionResult other = failure(ERROR_NOT_ENOUGH_MEMORY, "x.exe");
    EXPECT_FALSE(other.success);
    EXPECT_FALSE(other.cancelled);
    EXPECT_FALSE(other.errorMessage.empty());
}

TEST_F(WindowsRunProgramTest, ABadCommandLineIsRefusedBeforeAnythingIsOpened)
{
    for (const char* command : {"", "   ", R"("C:\Program Files\app.exe --x)", R"("" --x)", "bad\xFFutf8", "ok \xC3"})
    {
        const ProcessActionResult result =
            Windows::runShellProgram(FakeShellRun::functions(), false, {.commandLine = command, .asAdministrator = false});
        EXPECT_FALSE(result.success) << command;
        EXPECT_FALSE(result.errorMessage.empty()) << command;
    }
    EXPECT_TRUE(FakeShellRun::runs.empty());
    EXPECT_EQ(
        Windows::runShellProgram(FakeShellRun::functions(), false, {.commandLine = R"("C:\x.exe)", .asAdministrator = false}).errorMessage,
        "A \" quote isn't closed");
}

TEST_F(WindowsRunProgramTest, WithoutTheShellCallNothingIsOffered)
{
    const Windows::RunProgramFunctions none{};
    EXPECT_FALSE(Windows::runShellProgram(none, false, {.commandLine = "notepad", .asAdministrator = false}).success);

    WindowsProcessActions actions{WindowsProcessFunctions{}};
    EXPECT_FALSE(actions.actionCapabilities().canRunProgram);
    EXPECT_FALSE(actions.actionCapabilities().canRunProgramAsAdministrator);
    EXPECT_FALSE(actions.runProgram({.commandLine = "notepad", .asAdministrator = false}).success);
    EXPECT_TRUE(FakeShellRun::runs.empty());
}

TEST_F(WindowsRunProgramTest, TheCapabilityIsSetByTheRealTable)
{
    // The real table is only asked what it offers here: runProgram() is never called on it.
    EXPECT_TRUE(WindowsProcessActions(realWindowsProcessFunctions()).actionCapabilities().canRunProgram);
}

TEST_F(WindowsRunProgramTest, TheAdministratorOptionIsOfferedOnlyWhenNotElevated)
{
    FakeShellRun::elevated = false;
    const WindowsProcessActions standard(FakeShellRun::processFunctions());
    EXPECT_TRUE(standard.actionCapabilities().canRunProgram);
    EXPECT_TRUE(standard.actionCapabilities().canRunProgramAsAdministrator);

    FakeShellRun::elevated = true;
    const WindowsProcessActions elevated(FakeShellRun::processFunctions());
    EXPECT_TRUE(elevated.actionCapabilities().canRunProgram);
    EXPECT_FALSE(elevated.actionCapabilities().canRunProgramAsAdministrator); // its tasks are elevated already
    static_cast<void>(elevated.actionCapabilities());
    EXPECT_EQ(FakeShellRun::elevationChecks, 2); // once per construction, never per capabilities call
}

TEST_F(WindowsRunProgramTest, RunProgramGoesThroughTheTableWithTheElevationReadAtConstruction)
{
    FakeShellRun::elevated = true;
    WindowsProcessActions elevated(FakeShellRun::processFunctions());
    EXPECT_TRUE(elevated.runProgram({.commandLine = "cmd /k whoami /groups", .asAdministrator = true}).success);

    FakeShellRun::elevated = false;
    WindowsProcessActions standard(FakeShellRun::processFunctions());
    FakeShellRun::error = ERROR_CANCELLED;
    const ProcessActionResult cancelled = standard.runProgram({.commandLine = "cmd", .asAdministrator = true});
    EXPECT_TRUE(cancelled.cancelled);

    ASSERT_EQ(FakeShellRun::runs.size(), 2U);
    EXPECT_EQ(FakeShellRun::runs[0].verb, L"open"); // elevated: inherits it, no prompt
    EXPECT_EQ(FakeShellRun::runs[0].file, L"cmd");
    EXPECT_EQ(FakeShellRun::runs[0].parameters, L"/k whoami /groups");
    EXPECT_EQ(FakeShellRun::runs[1].verb, L"runas");
    EXPECT_TRUE(FakeShellRun::runs[1].parameters.empty());
}

} // namespace
} // namespace Platform
