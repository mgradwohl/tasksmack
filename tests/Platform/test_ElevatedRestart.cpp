// Tests for ElevatedRestart (#2728): how a Standard TaskSmack asks ShellExecuteExW("runas") to start itself
// elevated, what it makes of the answers, and how the elevated copy waits for it. ShellExecuteExW, OpenProcess
// and WaitForSingleObject are fakes here, so no UAC prompt is ever raised, and nothing runs elevated. The folder
// check runs on a fake disk, and its real disk calls on folders of this machine's that nothing changes (#3676).

#include "Platform/IElevatedRestart.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/ElevatedRestart.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

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

// #3676: a quote in the folder ends its quoted argument early, so the new copy would read another folder, or more
// arguments, than the one meant. Nothing is started with a command line that doesn't read back as written.
TEST(ElevatedRestartTest, AConfigDirectoryTheNewCopyWouldReadDifferentlyStartsNothing)
{
    const std::filesystem::path quoted(LR"(C:\Users\pat\a" --config-dir "C:\Windows)");
    const auto line = parameters(7, 9, quoted);
    ASSERT_FALSE(line.has_value());
    EXPECT_TRUE(line.error().contains("wouldn't read its own command line")) << line.error();

    const FakeShell& shell = resetFakeShell();
    for (const std::filesystem::path& dir : {quoted, std::filesystem::path()})
    {
        const ElevatedRestartResult result = restart(dir, nullptr, FAKES, 1, TICKS);
        EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Failed) << dir.string();
        EXPECT_FALSE(result.detail.empty()) << dir.string();
    }
    EXPECT_EQ(shell.calls, 0) << "no prompt for a hand-off that couldn't work";
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
    // Whatever else stops ShellExecuteExW is a failure in Windows' own words: the notice shows them.
    for (const DWORD error : {DWORD{ERROR_ACCESS_DENIED}, DWORD{ERROR_FILE_NOT_FOUND}, DWORD{ERROR_NO_ASSOCIATION}})
    {
        resetFakeShell(error);
        const ElevatedRestartResult result = restart(CONFIG_DIR, nullptr, FAKES, 1, TICKS);
        EXPECT_EQ(result.outcome, ElevatedRestartResult::Outcome::Failed) << error;
        EXPECT_EQ(result.detail, std::system_category().message(static_cast<int>(error))) << error;
    }
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
    bool synchronizeOnly = false; ///< Open it without the right to read its start time
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
    return OpenProcess(fakeWait().synchronizeOnly ? DWORD{SYNCHRONIZE} : access, FALSE, GetCurrentProcessId());
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

    fakeWait() = FakeWait{.answer = WAIT_ABANDONED};
    EXPECT_EQ(waitForHandoff(4242, ownTicks(), 5, WAIT_FAKES), HandoffWait::Unopenable) << "any other answer: the lock decides";
}

// #3676: a process whose start time can't be read can't be told from one that took the PID, so it isn't waited for.
TEST(ElevatedRestartTest, AProcessWhoseStartCantBeReadIsNotWaitedFor)
{
    fakeWait() = FakeWait{.synchronizeOnly = true};
    EXPECT_EQ(waitForHandoff(4242, ownTicks(), 5, WAIT_FAKES), HandoffWait::NotThatProcess);
    EXPECT_EQ(fakeWait().timeout, 0U) << "nothing waited on";
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
    std::wstring fileAt{};       // NOLINT(readability-redundant-member-init) - a plain file where a folder should be
    bool lockExists = false;     ///< tasksmack.lock is there
    bool configIsFolder = false; ///< config.toml is a folder
    bool linksKnown = true;      ///< The hard-link count can be read
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
    const bool config = text.ends_with(L".toml");
    const bool lock = text.ends_with(L".lock");
    if ((config && !fakeDisk().configExists) || (lock && !fakeDisk().lockExists))
    {
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD attributes =
        (config && !fakeDisk().configIsFolder) || lock || text == fakeDisk().fileAt ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY;
    if (text == fakeDisk().reparseAt)
    {
        attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    }
    return attributes;
}

std::optional<DWORD> fakeLinkCount(const std::wstring& /*path*/)
{
    if (!fakeDisk().linksKnown)
    {
        return std::nullopt;
    }
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

/// Why checkConfigDir() refused @p dir under @p profiles with the fake disk, or "(taken)".
[[nodiscard]] std::string refusal(std::wstring_view dir, std::wstring_view profiles = PROFILES)
{
    const auto checked = checkConfigDir(std::filesystem::path(dir), std::filesystem::path(profiles), DISK_FAKES);
    return checked.has_value() ? std::string("(taken)") : checked.error();
}

// #3676: each refusal says which check failed, so the log says why the elevated copy fell back to its own folder.
TEST(ElevatedRestartConfigDirTest, EachRefusalOfThePathSaysWhy)
{
    fakeDisk() = FakeDisk{};
    for (const std::wstring_view dir : {
             std::wstring_view(LR"(Users\pat\AppData\Roaming\TaskSmack)"),
             std::wstring_view(LR"(C:Users\pat\AppData\Roaming\TaskSmack)"),
             std::wstring_view(LR"(C:\Users\pat\.\AppData\Roaming\TaskSmack)"),
             std::wstring_view(LR"(\\?\C:\Users\pat\AppData\Roaming\TaskSmack)"),
             std::wstring_view(LR"(\\server\share\Users\pat\TaskSmack)"),
         })
    {
        EXPECT_TRUE(refusal(dir).contains("not a plain local folder path")) << refusal(dir);
    }
    EXPECT_TRUE(refusal(LR"(D:\Users\pat\AppData\Roaming\TaskSmack)").contains("not under the user profiles folder")) << "another drive";
    EXPECT_TRUE(refusal(GOOD_DIR, L"Users").contains("not under the user profiles folder")) << "a profiles folder that isn't absolute";
    EXPECT_TRUE(refusal(LR"(C:\Users\pat)").contains("not inside a user's profile")) << "a profile itself";
    EXPECT_TRUE(refusal(LR"(C:\ProgramData\TaskSmack)").contains("not inside a user's profile"));
    EXPECT_TRUE(refusal(LR"(C:\Users2\pat\TaskSmack)").contains("not inside a user's profile")) << "a folder that only starts alike";
    EXPECT_EQ(fakeDisk().pins, 0) << "nothing held";
}

TEST(ElevatedRestartConfigDirTest, AFileWhereAFolderShouldBeIsRefused)
{
    fakeDisk() = FakeDisk{.fileAt = LR"(C:\Users\pat\AppData)"};
    EXPECT_TRUE(refusal(GOOD_DIR).contains("isn't there")) << refusal(GOOD_DIR);
    fakeDisk() = FakeDisk{.missing = LR"(C:\Users\pat)"};
    EXPECT_TRUE(refusal(GOOD_DIR).contains("isn't there")) << refusal(GOOD_DIR);
    EXPECT_EQ(fakeDisk().pins, 0);
}

// The lock file gets the checks config.toml gets: an elevated TaskSmack opens both.
TEST(ElevatedRestartConfigDirTest, ALinkedOrHardLinkedLockFileIsRefusedToo)
{
    fakeDisk() = FakeDisk{.lockExists = true};
    EXPECT_EQ(refusal(GOOD_DIR), "(taken)") << "a plain lock file";

    fakeDisk() = FakeDisk{.reparseAt = std::wstring(GOOD_DIR) + L"\\tasksmack.lock", .lockExists = true};
    EXPECT_TRUE(refusal(GOOD_DIR).contains("its tasksmack.lock is a link or a folder")) << refusal(GOOD_DIR);

    fakeDisk() = FakeDisk{.configExists = false, .configLinks = 2, .lockExists = true};
    EXPECT_TRUE(refusal(GOOD_DIR).contains("its tasksmack.lock is hard-linked")) << refusal(GOOD_DIR);
    EXPECT_EQ(fakeDisk().pins, 0);
}

TEST(ElevatedRestartConfigDirTest, AConfigThatIsAFolderOrWhoseLinksCantBeCountedIsRefused)
{
    fakeDisk() = FakeDisk{.configIsFolder = true};
    EXPECT_TRUE(refusal(GOOD_DIR).contains("its config.toml is a link or a folder")) << refusal(GOOD_DIR);

    fakeDisk() = FakeDisk{.linksKnown = false};
    EXPECT_TRUE(refusal(GOOD_DIR).contains("its config.toml is hard-linked")) << "a count that can't be read is no proof of one link";
    EXPECT_EQ(fakeDisk().pins, 0);
}

TEST(ElevatedRestartConfigDirTest, AFolderOutsideTheRealProfilesFolderIsRefused)
{
    // The one-argument check, against this machine's FOLDERID_UserProfiles: refused before the disk is read.
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    ASSERT_GT(length, 0U);
    const auto checked = checkConfigDir(std::filesystem::path(std::wstring(system.data(), length)));
    ASSERT_FALSE(checked.has_value());
    EXPECT_TRUE(checked.error().contains("profile")) << checked.error();
    EXPECT_FALSE(checkConfigDir(std::filesystem::path(L"TaskSmack")).has_value()) << "a relative path";
}

// The real disk calls (#3676): the owner read, the hard-link count and the hold, each against a folder of this machine's.

/// This machine's system folder (C:\Windows\System32), which TrustedInstaller owns.
[[nodiscard]] std::filesystem::path systemFolder()
{
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    EXPECT_GT(length, 0U);
    return {std::wstring(system.data(), length)};
}

TEST(ElevatedRestartConfigDirTest, TheRealOwnerCheckRefusesAFolderWindowsOwns)
{
    // System32, checked against its drive as the profiles folder, with every real call: TrustedInstaller owns it.
    const std::filesystem::path system = systemFolder();
    const auto checked = checkConfigDir(system, system.root_path());
    ASSERT_FALSE(checked.has_value());
    EXPECT_TRUE(checked.error().contains("owned by an administrator or system account")) << checked.error();
}

/// An owner check that says every folder is a user's: the real one would refuse a folder an elevated test run made.
std::optional<bool> userOwned(const std::wstring& /*path*/)
{
    return false;
}

/// Every path a folder, but config.toml and tasksmack.lock, which aren't there: a folder swapped for something else
/// between the attribute checks and the hold, as far as checkConfigDir() can tell.
DWORD WINAPI everythingAFolder(LPCWSTR path)
{
    const std::wstring_view text(path);
    return text.ends_with(L".toml") || text.ends_with(L".lock") ? INVALID_FILE_ATTRIBUTES : FILE_ATTRIBUTE_DIRECTORY;
}

/// As everythingAFolder(), but config.toml is always there, as a plain file.
DWORD WINAPI configAlwaysThere(LPCWSTR path)
{
    const std::wstring_view text(path);
    if (text.ends_with(L".toml"))
    {
        return FILE_ATTRIBUTE_NORMAL;
    }
    return everythingAFolder(path);
}

/// A folder of this test's own under the temp folder, removed at the end.
class ScratchFolder
{
  public:
    ScratchFolder()
        : m_Path(std::filesystem::temp_directory_path() / std::format(L"TaskSmack.ElevatedRestartTest.{}", GetCurrentProcessId()))
    {
        std::error_code error;
        std::filesystem::create_directories(configDir(), error);
        EXPECT_FALSE(error) << error.message();
    }
    // NOLINTNEXTLINE(bugprone-exception-escape) - a test fixture: what escapes it ends the run, as a failed test should
    ~ScratchFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(m_Path, error);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    ScratchFolder(ScratchFolder&&) = delete;
    ScratchFolder& operator=(ScratchFolder&&) = delete;

    /// The folder itself, which stands in for the profiles folder.
    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return m_Path;
    }
    /// A config folder inside it, two levels below root(): a profile folder's own, never the profile itself.
    [[nodiscard]] std::filesystem::path configDir() const
    {
        return m_Path / L"pat" / L"TaskSmack";
    }

  private:
    std::filesystem::path m_Path;
};

TEST(ElevatedRestartConfigDirTest, TheRealLinkCountSeesAHardLinkedConfig)
{
    const ScratchFolder scratch;
    const std::filesystem::path config = scratch.configDir() / L"config.toml";
    std::ofstream(config) << "theme = \"dark\"\n";
    const std::filesystem::path elsewhere = scratch.root() / L"elsewhere.toml";
    ASSERT_NE(CreateHardLinkW(elsewhere.c_str(), config.c_str(), nullptr), FALSE) << GetLastError();
    const ConfigDirFunctions realLinks{.getFileAttributes = &::GetFileAttributesW, .linkCount = nullptr, .ownerIsPrivileged = &userOwned};
    const auto linked = checkConfigDir(scratch.configDir(), scratch.root(), realLinks);
    ASSERT_FALSE(linked.has_value());
    EXPECT_TRUE(linked.error().contains("its config.toml is hard-linked")) << linked.error();

    // A config.toml that is there for the attribute check and gone for the count: refused, not taken as one link.
    std::filesystem::remove(elsewhere);
    std::filesystem::remove(config);
    const ConfigDirFunctions vanished{.getFileAttributes = &configAlwaysThere, .linkCount = nullptr, .ownerIsPrivileged = &userOwned};
    const auto gone = checkConfigDir(scratch.configDir(), scratch.root(), vanished);
    ASSERT_FALSE(gone.has_value());
    EXPECT_TRUE(gone.error().contains("hard-linked")) << gone.error();
}

TEST(ElevatedRestartConfigDirTest, TheRealHoldRefusesWhatIsNoLongerAFolder)
{
    const ScratchFolder scratch;
    // What passed as a folder is a file, or nothing at all, by the time it is held.
    const std::filesystem::path file = scratch.configDir() / L"settings";
    std::ofstream(file) << "not a folder";
    const ConfigDirFunctions swapped{.getFileAttributes = &everythingAFolder, .linkCount = nullptr, .ownerIsPrivileged = &userOwned};
    for (const std::filesystem::path& dir : {file, scratch.configDir() / L"gone"})
    {
        const auto checked = checkConfigDir(dir, scratch.root(), swapped);
        ASSERT_FALSE(checked.has_value()) << dir.string();
        EXPECT_TRUE(checked.error().contains("can't be held open")) << checked.error();
    }
}

TEST(ElevatedRestartConfigDirTest, TheRealHoldKeepsTheFolderFromBeingRemoved)
{
    // This test program's own folder, held for the rest of the run: it can't be removed while the program runs anyway.
    std::array<wchar_t, MAX_PATH> image{};
    const DWORD length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    ASSERT_GT(length, 0U);
    ASSERT_LT(length, image.size());
    const std::filesystem::path folder = std::filesystem::path(std::wstring(image.data(), length)).parent_path();
    ASSERT_TRUE(folder.has_relative_path() && folder.parent_path().has_relative_path()) << folder.string();
    const ConfigDirFunctions held{.getFileAttributes = &::GetFileAttributesW, .linkCount = nullptr, .ownerIsPrivileged = &userOwned};
    const auto checked = checkConfigDir(folder, folder.parent_path().parent_path(), held);
    ASSERT_TRUE(checked.has_value()) << checked.error();

    // Opening it to delete it is refused while it is held: nothing is deleted either way.
    const UniqueHandle forDelete(CreateFileW(folder.c_str(),
                                             DELETE,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS,
                                             nullptr));
    const DWORD error = GetLastError();
    EXPECT_FALSE(forDelete) << "the held folder opened for deletion";
    EXPECT_EQ(error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
}

} // namespace
} // namespace Platform::Windows::ElevatedRestart
