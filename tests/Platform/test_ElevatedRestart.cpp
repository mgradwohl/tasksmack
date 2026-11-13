// Tests for ElevatedRestart (#2728): how a Standard TaskSmack asks ShellExecuteExW("runas") to start itself
// elevated, what it makes of the answers, and how the elevated copy waits for it. ShellExecuteExW, OpenProcess
// and WaitForSingleObject are fakes here, so no UAC prompt is ever raised, and nothing runs elevated.

#include "Platform/IElevatedRestart.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/ElevatedRestart.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
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

constexpr std::uint64_t TICKS = 133'000'000'000'000'000ULL;

TEST(ElevatedRestartTest, ParametersNameThisProcessItsStartAndTheLaunchingUsersConfigDirectory)
{
    const auto line = parameters(4242, TICKS, CONFIG_DIR);
    ASSERT_TRUE(line.has_value()) << line.error();
    EXPECT_EQ(*line, LR"(--handoff 4242 133000000000000000 --config-dir "C:\Users\Pat Smith\AppData\Roaming\TaskSmack")");
}

TEST(ElevatedRestartTest, ADirectoryEndingInABackslashStillReadsBackExactly)
{
    // "C:\" in quotes would be an escaped quote to CommandLineToArgvW: the backslash is doubled.
    const auto line = parameters(7, 9, std::filesystem::path(LR"(D:\)"));
    ASSERT_TRUE(line.has_value()) << line.error();
    EXPECT_EQ(*line, LR"(--handoff 7 9 --config-dir "D:\\")");
}

TEST(ElevatedRestartTest, NoConfigDirectoryIsRefused)
{
    EXPECT_FALSE(parameters(7, 9, {}).has_value());
}

TEST(ElevatedRestartTest, RestartAsksRunasForThisExecutableWithTheHandOff)
{
    const FakeShell& shell = resetFakeShell();
    const ElevatedRestartResult result = restart(CONFIG_DIR, OWNER, FAKES, 4242, TICKS);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Started) << result.detail;
    EXPECT_EQ(shell.calls, 1);
    EXPECT_EQ(shell.verb, L"runas");
    EXPECT_EQ(shell.file, IMAGE) << "its own full path, never a search";
    EXPECT_EQ(shell.parameters, *parameters(4242, TICKS, CONFIG_DIR));
    EXPECT_EQ(shell.window, OWNER) << "the prompt in front of TaskSmack";
    EXPECT_NE(shell.mask & SEE_MASK_NOCLOSEPROCESS, 0U) << "Started only with the process back";
    EXPECT_NE(shell.mask & SEE_MASK_FLAG_NO_UI, 0U);
}

TEST(ElevatedRestartTest, ADeclinedPromptIsDeclinedNotAFailure)
{
    resetFakeShell(ERROR_CANCELLED);
    const ElevatedRestartResult result = restart(CONFIG_DIR, nullptr, FAKES, 1, TICKS);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Declined);
    EXPECT_TRUE(result.detail.empty());
}

TEST(ElevatedRestartTest, AnotherErrorIsAFailureThatSaysWhy)
{
    resetFakeShell(ERROR_ACCESS_DENIED);
    const ElevatedRestartResult result = restart(CONFIG_DIR, nullptr, FAKES, 1, TICKS);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Failed);
    EXPECT_FALSE(result.detail.empty());
}

TEST(ElevatedRestartTest, NoProcessBackIsAFailureSoThisCopyDoesNotClose)
{
    resetFakeShell().noProcess = true;
    const ElevatedRestartResult result = restart(CONFIG_DIR, nullptr, FAKES, 1, TICKS);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Failed);
}

TEST(ElevatedRestartTest, AnUnknownOwnPathStartsNothing)
{
    const FakeShell& shell = resetFakeShell();
    const ElevatedRestartResult result =
        restart(CONFIG_DIR, nullptr, {.shellExecuteEx = &fakeShellExecuteEx, .getModuleFileName = &failingModuleFileName}, 1, TICKS);
    EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Failed);
    EXPECT_EQ(shell.calls, 0);
}

TEST(ElevatedRestartTest, TheRestartObjectHandsOverThisProcessAndItsOwnStart)
{
    const FakeShell& shell = resetFakeShell(ERROR_CANCELLED);
    WindowsElevatedRestart restarter(OWNER, AdministratorPrompt::Credentials, FAKES);
    EXPECT_EQ(restarter.prompt(), AdministratorPrompt::Credentials);
    EXPECT_EQ(restarter.restart(CONFIG_DIR).outcome, ElevatedRestartResult::Outcome::Declined);
    EXPECT_EQ(shell.window, OWNER);
    const std::optional<std::uint64_t> ownTicks = BrokerHelper::startTicksOf(GetCurrentProcess());
    ASSERT_TRUE(ownTicks.has_value());
    EXPECT_EQ(shell.parameters, parameters(GetCurrentProcessId(), ownTicks.value_or(0), CONFIG_DIR).value_or(L""))
        << "on behalf of this process";
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
    DWORD openError = ERROR_SUCCESS; ///< ERROR_SUCCESS: open this test's own process
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
    if (fakeWait().openError != ERROR_SUCCESS)
    {
        SetLastError(fakeWait().openError);
        return nullptr;
    }
    return OpenProcess(access, FALSE, GetCurrentProcessId());
}

DWORD WINAPI fakeWaitForSingleObject(HANDLE /*handle*/, DWORD timeout)
{
    fakeWait().timeout = timeout;
    return fakeWait().answer;
}

const WaitFunctions WAIT_FAKES{.openProcess = &fakeOpenProcess, .waitForSingleObject = &fakeWaitForSingleObject};

/// This test process's creation time, which the fake open hands back the process of.
[[nodiscard]] std::uint64_t ownTicks()
{
    return BrokerHelper::startTicksOf(GetCurrentProcess()).value_or(0);
}

TEST(ElevatedRestartTest, TheElevatedCopyWaitsForTheStandardOneBounded)
{
    fakeWait() = FakeWait{};
    EXPECT_EQ(waitForHandoff(4242, ownTicks(), HANDOFF_WAIT_MS, WAIT_FAKES), HandoffWait::Exited);
    EXPECT_EQ(fakeWait().openedPid, 4242U);
    EXPECT_EQ(fakeWait().access, static_cast<DWORD>(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION)) << "only to check and wait on it";
    EXPECT_EQ(fakeWait().timeout, HANDOFF_WAIT_MS) << "never INFINITE";

    fakeWait() = FakeWait{.answer = WAIT_TIMEOUT};
    EXPECT_EQ(waitForHandoff(4242, ownTicks(), 5, WAIT_FAKES), HandoffWait::TimedOut);

    fakeWait() = FakeWait{.answer = WAIT_FAILED};
    EXPECT_EQ(waitForHandoff(4242, ownTicks(), 5, WAIT_FAKES), HandoffWait::Unopenable) << "the lock decides";
}

TEST(ElevatedRestartTest, OnlyANonexistentPidHasGone)
{
    fakeWait() = FakeWait{.openError = ERROR_INVALID_PARAMETER};
    EXPECT_EQ(waitForHandoff(4242, ownTicks(), 5, WAIT_FAKES), HandoffWait::NotRunning) << "it had already closed";
}

// The review's case (#3657): another account approved the prompt, and its process may not open the user's.
TEST(ElevatedRestartTest, AProcessThisAccountMayNotOpenIsUnopenableNotGone)
{
    fakeWait() = FakeWait{.openError = ERROR_ACCESS_DENIED};
    EXPECT_EQ(waitForHandoff(4242, ownTicks(), 5, WAIT_FAKES), HandoffWait::Unopenable);
    EXPECT_EQ(fakeWait().timeout, 0U) << "nothing waited on";
}

TEST(ElevatedRestartTest, AReusedPidIsNotWaitedFor)
{
    fakeWait() = FakeWait{};
    EXPECT_EQ(waitForHandoff(4242, ownTicks() + 1, 5, WAIT_FAKES), HandoffWait::NotThatProcess);
    EXPECT_EQ(fakeWait().timeout, 0U) << "nothing waited on";
}

TEST(ElevatedRestartTest, TheHandOffArgumentsAreTheOnesLaunchOptionsReads)
{
    EXPECT_EQ(HANDOFF_ARGUMENT, "--handoff");
    EXPECT_EQ(CONFIG_DIR_ARGUMENT, "--config-dir");
}

// checkConfigDir() through fakes: the folders below are never touched.

constexpr std::wstring_view PROFILES = LR"(C:\Users)";
constexpr std::wstring_view GOOD_DIR = LR"(C:\Users\pat\AppData\Roaming\TaskSmack)";

/// What the fake disk answers.
struct FakeDisk
{
    std::wstring reparseAt{}; // NOLINT(readability-redundant-member-init) - designated initializers need every member initialised
    std::wstring missing{};   // NOLINT(readability-redundant-member-init) - designated initializers need every member initialised
    bool configExists = true; ///< config.toml is there
    DWORD configLinks = 1;    ///< Its hard-link count
    std::optional<bool> privilegedOwner = false;
    bool pinned = true; ///< pin() succeeds
    int pins = 0;
};

[[nodiscard]] FakeDisk& fakeDisk()
{
    static FakeDisk fake;
    return fake;
}

DWORD WINAPI fakeAttributes(LPCWSTR path)
{
    const std::wstring_view text(path);
    if (text == fakeDisk().missing)
    {
        return INVALID_FILE_ATTRIBUTES;
    }
    const bool file = text.ends_with(L".toml") || text.ends_with(L".lock");
    if (file && (!fakeDisk().configExists || text.ends_with(L".lock")))
    {
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD attributes = file ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY;
    if (text == fakeDisk().reparseAt)
    {
        attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    }
    return attributes;
}

std::optional<DWORD> fakeLinkCount(const std::wstring& /*path*/)
{
    return fakeDisk().configLinks;
}

std::optional<bool> fakeOwnerIsPrivileged(const std::wstring& /*path*/)
{
    return fakeDisk().privilegedOwner;
}

bool fakePin(const std::wstring& /*path*/)
{
    ++fakeDisk().pins;
    return fakeDisk().pinned;
}

const ConfigDirFunctions DISK_FAKES{
    .getFileAttributes = &fakeAttributes,
    .linkCount = &fakeLinkCount,
    .ownerIsPrivileged = &fakeOwnerIsPrivileged,
    .pin = &fakePin,
};

[[nodiscard]] std::expected<void, std::string> check(std::wstring_view dir)
{
    return checkConfigDir(std::filesystem::path(dir), std::filesystem::path(PROFILES), DISK_FAKES);
}

TEST(ElevatedRestartConfigDirTest, AUsersOwnPlainFolderIsTakenAndHeldOpen)
{
    fakeDisk() = FakeDisk{};
    const auto checked = check(GOOD_DIR);
    EXPECT_TRUE(checked.has_value()) << checked.error();
    EXPECT_EQ(fakeDisk().pins, 1);
    EXPECT_TRUE(check(LR"(c:\users\PAT\AppData\Roaming\TaskSmack)").has_value()) << "case doesn't matter, as on NTFS";
}

TEST(ElevatedRestartConfigDirTest, AJunctionAnywhereOnThePathIsRefused)
{
    for (const std::wstring_view at : {GOOD_DIR, std::wstring_view(LR"(C:\Users\pat\AppData)"), std::wstring_view(LR"(C:\Users\pat)")})
    {
        fakeDisk() = FakeDisk{.reparseAt = std::wstring(at)};
        const auto checked = check(GOOD_DIR);
        ASSERT_FALSE(checked.has_value()) << "a reparse point at " << std::filesystem::path(at).string();
        EXPECT_TRUE(checked.error().contains("junction or link")) << checked.error();
        EXPECT_EQ(fakeDisk().pins, 0) << "nothing held";
    }
}

TEST(ElevatedRestartConfigDirTest, ALinkedOrHardLinkedConfigIsRefused)
{
    fakeDisk() = FakeDisk{.reparseAt = std::wstring(GOOD_DIR) + L"\\config.toml"};
    ASSERT_FALSE(check(GOOD_DIR).has_value());
    EXPECT_TRUE(check(GOOD_DIR).error().contains("config.toml")) << check(GOOD_DIR).error();

    fakeDisk() = FakeDisk{.configLinks = 2};
    ASSERT_FALSE(check(GOOD_DIR).has_value());
    EXPECT_TRUE(check(GOOD_DIR).error().contains("hard-linked")) << check(GOOD_DIR).error();

    fakeDisk() = FakeDisk{.configExists = false};
    EXPECT_TRUE(check(GOOD_DIR).has_value()) << "a config not saved yet is created as a plain file";
}

TEST(ElevatedRestartConfigDirTest, AFolderOutsideAUsersProfileIsRefused)
{
    fakeDisk() = FakeDisk{};
    for (const std::wstring_view dir : {
             std::wstring_view(LR"(C:\Windows\System32)"),
             std::wstring_view(LR"(C:\Users)"),
             std::wstring_view(LR"(C:\Users\pat)"),
             std::wstring_view(LR"(D:\Users\pat\AppData\Roaming\TaskSmack)"),
             std::wstring_view(LR"(C:\Users\pat\..\..\Windows\Temp\x)"),
             std::wstring_view(LR"(\\server\share\Users\pat\TaskSmack)"),
             std::wstring_view(LR"(\\?\C:\Users\pat\AppData\Roaming\TaskSmack)"),
             std::wstring_view(LR"(Users\pat\AppData\Roaming\TaskSmack)"),
         })
    {
        EXPECT_FALSE(check(dir).has_value()) << std::filesystem::path(dir).string();
    }
    EXPECT_EQ(fakeDisk().pins, 0);
}

TEST(ElevatedRestartConfigDirTest, AFolderAnAdministratorOrSystemOwnsIsRefused)
{
    fakeDisk() = FakeDisk{.privilegedOwner = true}; // C:\Users\Default, say
    ASSERT_FALSE(check(GOOD_DIR).has_value());
    EXPECT_TRUE(check(GOOD_DIR).error().contains("owned by")) << check(GOOD_DIR).error();

    fakeDisk() = FakeDisk{.privilegedOwner = std::nullopt};
    EXPECT_FALSE(check(GOOD_DIR).has_value()) << "an owner that can't be read";
}

TEST(ElevatedRestartConfigDirTest, AMissingFolderOrOneSwappedBeforeItIsHeldIsRefused)
{
    fakeDisk() = FakeDisk{.missing = std::wstring(GOOD_DIR)};
    EXPECT_FALSE(check(GOOD_DIR).has_value());

    fakeDisk() = FakeDisk{.pinned = false};
    EXPECT_FALSE(check(GOOD_DIR).has_value());
}

} // namespace
} // namespace Platform::Windows::ElevatedRestart
