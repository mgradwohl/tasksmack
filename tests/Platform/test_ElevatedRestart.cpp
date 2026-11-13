// Tests for ElevatedRestart (#2728): how a Standard TaskSmack asks ShellExecuteExW("runas") to start itself
// elevated, what it makes of the answers, and how the elevated copy waits for it. ShellExecuteExW, OpenProcess
// and WaitForSingleObject are fakes here, so no UAC prompt is ever raised, and nothing runs elevated.

#include "Platform/IElevatedRestart.h"
#include "Platform/Windows/ElevatedRestart.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
// clang-format on

namespace Platform::Windows::ElevatedRestart
{
namespace
{

/// What the fake ShellExecuteExW was asked, and how it answers.
struct FakeShell
{
    int calls = 0;
    std::wstring verb;
    std::wstring file;
    std::wstring parameters;
    ULONG mask = 0;
    HWND window = nullptr;
    DWORD failWith = ERROR_SUCCESS; ///< ERROR_SUCCESS: succeed, handing back this process
    bool noProcess = false;         ///< succeed without a process
};

[[nodiscard]] FakeShell& fakeShell()
{
    static FakeShell fake;
    return fake;
}

FakeShell& resetFakeShell(DWORD failWith = ERROR_SUCCESS)
{
    fakeShell() = FakeShell{};
    fakeShell().failWith = failWith;
    return fakeShell();
}

BOOL WINAPI fakeShellExecuteEx(SHELLEXECUTEINFOW* info)
{
    FakeShell& fake = fakeShell();
    ++fake.calls;
    fake.verb = info->lpVerb != nullptr ? info->lpVerb : L"";
    fake.file = info->lpFile != nullptr ? info->lpFile : L"";
    fake.parameters = info->lpParameters != nullptr ? info->lpParameters : L"";
    fake.mask = info->fMask;
    fake.window = info->hwnd;
    if (fake.failWith != ERROR_SUCCESS)
    {
        SetLastError(fake.failWith);
        return FALSE;
    }
    info->hProcess = fake.noProcess ? nullptr : OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    return TRUE;
}

constexpr std::wstring_view IMAGE = LR"(C:\Program Files\TaskSmack\TaskSmack.exe)";

DWORD WINAPI fakeModuleFileName(HMODULE /*module*/, LPWSTR buffer, DWORD size)
{
    const std::span<wchar_t> out(buffer, size);
    std::ranges::copy(IMAGE, out.begin());
    out[IMAGE.size()] = L'\0';
    return static_cast<DWORD>(IMAGE.size());
}

DWORD WINAPI failingModuleFileName(HMODULE /*module*/, LPWSTR /*buffer*/, DWORD /*size*/)
{
    SetLastError(ERROR_ACCESS_DENIED);
    return 0;
}

const Functions FAKES{.shellExecuteEx = &fakeShellExecuteEx, .getModuleFileName = &fakeModuleFileName};
constexpr std::wstring_view CONFIG_DIR = LR"(C:\Users\Pat Smith\AppData\Roaming\TaskSmack)";
const auto OWNER = reinterpret_cast<HWND>(0x1234); // NOLINT(performance-no-int-to-ptr) - a stand-in handle, never used

TEST(ElevatedRestartTest, ParametersNameThisProcessAndTheLaunchingUsersConfigDirectory)
{
    const auto line = parameters(4242, CONFIG_DIR);
    ASSERT_TRUE(line.has_value()) << line.error();
    EXPECT_EQ(*line, LR"(--handoff 4242 --config-dir "C:\Users\Pat Smith\AppData\Roaming\TaskSmack")");
}

TEST(ElevatedRestartTest, ADirectoryEndingInABackslashStillReadsBackExactly)
{
    // "C:\" in quotes would be an escaped quote to CommandLineToArgvW: the backslash is doubled.
    const auto line = parameters(7, std::filesystem::path(LR"(D:\)"));
    ASSERT_TRUE(line.has_value()) << line.error();
    EXPECT_EQ(*line, LR"(--handoff 7 --config-dir "D:\\")");
}

TEST(ElevatedRestartTest, NoConfigDirectoryIsRefused)
{
    EXPECT_FALSE(parameters(7, {}).has_value());
}

TEST(ElevatedRestartTest, RestartAsksRunasForThisExecutableWithTheHandOff)
{
    const FakeShell& shell = resetFakeShell();
    const ElevatedRestartResult result = restart(CONFIG_DIR, OWNER, FAKES, 4242);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Started) << result.detail;
    EXPECT_EQ(shell.calls, 1);
    EXPECT_EQ(shell.verb, L"runas");
    EXPECT_EQ(shell.file, IMAGE) << "its own full path, never a search";
    EXPECT_EQ(shell.parameters, *parameters(4242, CONFIG_DIR));
    EXPECT_EQ(shell.window, OWNER) << "the prompt in front of TaskSmack";
    EXPECT_NE(shell.mask & SEE_MASK_NOCLOSEPROCESS, 0U) << "Started only with the process back";
    EXPECT_NE(shell.mask & SEE_MASK_FLAG_NO_UI, 0U);
}

TEST(ElevatedRestartTest, ADeclinedPromptIsDeclinedNotAFailure)
{
    resetFakeShell(ERROR_CANCELLED);
    const ElevatedRestartResult result = restart(CONFIG_DIR, nullptr, FAKES, 1);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Declined);
    EXPECT_TRUE(result.detail.empty());
}

TEST(ElevatedRestartTest, AnotherErrorIsAFailureThatSaysWhy)
{
    resetFakeShell(ERROR_ACCESS_DENIED);
    const ElevatedRestartResult result = restart(CONFIG_DIR, nullptr, FAKES, 1);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Failed);
    EXPECT_FALSE(result.detail.empty());
}

TEST(ElevatedRestartTest, NoProcessBackIsAFailureSoThisCopyDoesNotClose)
{
    resetFakeShell().noProcess = true;
    const ElevatedRestartResult result = restart(CONFIG_DIR, nullptr, FAKES, 1);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Failed);
}

TEST(ElevatedRestartTest, AnUnknownOwnPathStartsNothing)
{
    const FakeShell& shell = resetFakeShell();
    const ElevatedRestartResult result =
        restart(CONFIG_DIR, nullptr, {.shellExecuteEx = &fakeShellExecuteEx, .getModuleFileName = &failingModuleFileName}, 1);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Failed);
    EXPECT_EQ(shell.calls, 0);
}

TEST(ElevatedRestartTest, TheRestartObjectUsesItsOwnerAndSaysWhatThePromptAsks)
{
    const FakeShell& shell = resetFakeShell(ERROR_CANCELLED);
    WindowsElevatedRestart restarter(OWNER, AdministratorPrompt::Credentials, FAKES);
    EXPECT_EQ(restarter.prompt(), AdministratorPrompt::Credentials);
    EXPECT_EQ(restarter.restart(CONFIG_DIR).outcome, ElevatedRestartResult::Outcome::Declined);
    EXPECT_EQ(shell.window, OWNER);
    EXPECT_TRUE(shell.parameters.contains(std::to_wstring(GetCurrentProcessId()))) << "on behalf of this process";
}

TEST(ElevatedRestartTest, OnlyASplitTokenAdministratorIsAskedForConsentAlone)
{
    EXPECT_EQ(promptFor(TokenElevationTypeLimited), AdministratorPrompt::Consent);
    EXPECT_EQ(promptFor(TokenElevationTypeDefault), AdministratorPrompt::Credentials) << "a standard user";
    EXPECT_EQ(promptFor(TokenElevationTypeFull), AdministratorPrompt::Credentials);
}

/// The fake wait's answers.
struct FakeWait
{
    DWORD openedPid = 0;
    DWORD access = 0;
    DWORD timeout = 0;
    bool open = true;
    DWORD answer = WAIT_OBJECT_0;
};

[[nodiscard]] FakeWait& fakeWait()
{
    static FakeWait fake;
    return fake;
}

HANDLE WINAPI fakeOpenProcess(DWORD access, BOOL /*inherit*/, DWORD pid)
{
    fakeWait().openedPid = pid;
    fakeWait().access = access;
    return fakeWait().open ? OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId()) : nullptr;
}

DWORD WINAPI fakeWaitForSingleObject(HANDLE /*handle*/, DWORD timeout)
{
    fakeWait().timeout = timeout;
    return fakeWait().answer;
}

const WaitFunctions WAIT_FAKES{.openProcess = &fakeOpenProcess, .waitForSingleObject = &fakeWaitForSingleObject};

TEST(ElevatedRestartTest, TheElevatedCopyWaitsForTheStandardOneBounded)
{
    fakeWait() = FakeWait{};
    EXPECT_EQ(waitForHandoff(4242, HANDOFF_WAIT_MS, WAIT_FAKES), HandoffWait::Exited);
    EXPECT_EQ(fakeWait().openedPid, 4242U);
    EXPECT_EQ(fakeWait().access, static_cast<DWORD>(SYNCHRONIZE)) << "only to wait on it";
    EXPECT_EQ(fakeWait().timeout, HANDOFF_WAIT_MS) << "never INFINITE";

    fakeWait() = FakeWait{.answer = WAIT_TIMEOUT};
    EXPECT_EQ(waitForHandoff(4242, 5, WAIT_FAKES), HandoffWait::TimedOut);

    fakeWait() = FakeWait{.open = false};
    EXPECT_EQ(waitForHandoff(4242, 5, WAIT_FAKES), HandoffWait::NotRunning) << "it had already closed";

    fakeWait() = FakeWait{.answer = WAIT_FAILED};
    EXPECT_EQ(waitForHandoff(4242, 5, WAIT_FAKES), HandoffWait::NotRunning);
}

TEST(ElevatedRestartTest, TheHandOffArgumentsAreTheOnesLaunchOptionsReads)
{
    EXPECT_EQ(HANDOFF_ARGUMENT, "--handoff");
    EXPECT_EQ(CONFIG_DIR_ARGUMENT, "--config-dir");
}

} // namespace
} // namespace Platform::Windows::ElevatedRestart
