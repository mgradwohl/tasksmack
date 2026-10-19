// Tests for HelperLauncher (#2549): how TaskSmack asks ShellExecuteExW("runas") to start
// TaskSmackHelper.exe, and what it makes of the answers. ShellExecuteExW is always a fake here, so no
// UAC prompt is ever raised and nothing runs elevated; test_BrokerHelper.cpp starts the real helper,
// unelevated, through the same launcher with CreateProcessW in its place.

#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/HelperLauncher.h"
#include "Platform/Windows/PipeBroker.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

namespace Platform::Windows
{
namespace
{

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds WAIT{10'000};

/// What the fake ShellExecuteExW was asked, and how it answers.
struct FakeShell
{
    int calls = 0;
    std::wstring verb;
    std::wstring file;
    std::wstring parameters;
    ULONG mask = 0;
    int show = -1;
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
    fake.show = info->nShow;
    fake.window = info->hwnd;
    if (fake.failWith != ERROR_SUCCESS)
    {
        SetLastError(fake.failWith);
        return FALSE;
    }
    info->hProcess = fake.noProcess ? nullptr : OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    return TRUE;
}

DWORD WINAPI relativeModuleFileName(HMODULE /*module*/, LPWSTR buffer, DWORD size)
{
    constexpr std::wstring_view NAME = L"TaskSmack.exe";
    const std::span<wchar_t> out(buffer, size);
    std::ranges::copy(NAME, out.begin());
    out[NAME.size()] = L'\0';
    return static_cast<DWORD>(NAME.size());
}

DWORD WINAPI failingModuleFileName(HMODULE /*module*/, LPWSTR /*buffer*/, DWORD /*size*/)
{
    SetLastError(ERROR_ACCESS_DENIED);
    return 0;
}

[[nodiscard]] HelperLauncher::Functions fakes()
{
    return {.shellExecuteEx = &fakeShellExecuteEx};
}

[[nodiscard]] std::wstring freshName()
{
    auto name = BrokerPipe::makePipeName();
    EXPECT_TRUE(name.has_value());
    return name.value_or(L"");
}

[[nodiscard]] std::uint64_t startTicksOf(HANDLE process)
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    EXPECT_NE(GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime), FALSE);
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
}

[[nodiscard]] std::wstring ownImage()
{
    std::array<wchar_t, 32'768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return {path.data(), length};
}

/// @p parameters as the helper reads them: split by CommandLineToArgvW after its path, then parsed.
[[nodiscard]] std::expected<BrokerHelper::Arguments, std::string> readAsTheHelper(const std::wstring& parameters)
{
    const std::wstring commandLine = L"\"C:\\Program Files\\TaskSmack\\TaskSmackHelper.exe\" " + parameters;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &count);
    if (argv == nullptr)
    {
        return std::unexpected(std::string("didn't split"));
    }
    const std::span<LPWSTR> all(argv, static_cast<std::size_t>(count));
    const std::vector<std::wstring> owned(all.begin() + 1, all.end());
    LocalFree(static_cast<HLOCAL>(argv));
    const std::vector<std::wstring_view> views(owned.begin(), owned.end());
    return BrokerHelper::parseArguments(views);
}

// --- The request --------------------------------------------------------------------------------------

TEST(HelperLauncherTest, TheHelperBesideThisExecutableIsStartedElevatedForThisProcess)
{
    const FakeShell& fake = resetFakeShell();
    const std::wstring name = freshName();
    const auto helper = HelperLauncher::launch(name, {}, fakes());
    ASSERT_TRUE(helper.has_value()) << helper.error();
    EXPECT_EQ(GetProcessId(helper->get()), GetCurrentProcessId()) << "the process ShellExecuteExW handed back";

    EXPECT_EQ(fake.calls, 1);
    EXPECT_EQ(fake.verb, L"runas");
    EXPECT_EQ(fake.mask & SEE_MASK_NOCLOSEPROCESS, static_cast<ULONG>(SEE_MASK_NOCLOSEPROCESS)) << "the process is needed";
    EXPECT_EQ(fake.mask & SEE_MASK_NOASYNC, static_cast<ULONG>(SEE_MASK_NOASYNC));
    EXPECT_EQ(fake.mask & SEE_MASK_FLAG_NO_UI, static_cast<ULONG>(SEE_MASK_FLAG_NO_UI)) << "the error is TaskSmack's to show";
    EXPECT_EQ(fake.show, SW_HIDE);

    // The full path of the helper beside this executable: never a bare name a search could resolve.
    EXPECT_TRUE(std::filesystem::path(fake.file).is_absolute()) << std::filesystem::path(fake.file).string();
    EXPECT_EQ(fake.file, BrokerHelper::besidePath(ownImage(), L"TaskSmackHelper.exe"));

    const auto read = readAsTheHelper(fake.parameters);
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->pipeName, name);
    EXPECT_EQ(read->parentPid, GetCurrentProcessId());
    EXPECT_EQ(read->parentStartTicks, startTicksOf(GetCurrentProcess())) << "this process's own creation time";
}

TEST(HelperLauncherTest, TheParametersAreTheHelpersCommandLineAndReadBackExactly)
{
    const BrokerHelper::Arguments arguments{.pipeName = freshName(), .parentPid = 4294967295U, .parentStartTicks = 18446744073709551615ULL};
    const auto parameters = HelperLauncher::parameters(arguments);
    ASSERT_TRUE(parameters.has_value()) << parameters.error();
    EXPECT_EQ(*parameters, BrokerHelper::commandLine(arguments));
    const auto read = readAsTheHelper(*parameters);
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->pipeName, arguments.pipeName);
    EXPECT_EQ(read->parentPid, arguments.parentPid);
    EXPECT_EQ(read->parentStartTicks, arguments.parentStartTicks);
}

TEST(HelperLauncherTest, ParametersTheHelperWouldReadDifferentlyAreRefused)
{
    // Spaces and quotes would split or join differently; neither is ever in a name makePipeName() makes.
    for (const std::wstring& pipeName : {std::wstring(L"\\\\.\\pipe\\TaskSmack.Broker.0123456789abcdef0123456789abcde f"),
                                         std::wstring(L"\\\\.\\pipe\\TaskSmack.Broker.0123456789abcdef0123456789abcde\""),
                                         std::wstring(L"\\\\.\\pipe\\Other")})
    {
        EXPECT_FALSE(HelperLauncher::parameters({.pipeName = pipeName, .parentPid = 1, .parentStartTicks = 1}).has_value());
    }
    EXPECT_FALSE(HelperLauncher::parameters({.pipeName = freshName(), .parentPid = 0, .parentStartTicks = 1}).has_value());
}

// --- The answers --------------------------------------------------------------------------------------

TEST(HelperLauncherTest, ADismissedPromptIsARefusal)
{
    const FakeShell& fake = resetFakeShell(ERROR_CANCELLED);
    const auto helper = HelperLauncher::launch(freshName(), {}, fakes());
    ASSERT_FALSE(helper.has_value());
    EXPECT_EQ(helper.error(), HelperLauncher::PROMPT_DISMISSED);
    EXPECT_EQ(fake.calls, 1);
}

TEST(HelperLauncherTest, AMissingHelperSaysSo)
{
    for (const DWORD error : {DWORD{ERROR_FILE_NOT_FOUND}, DWORD{ERROR_PATH_NOT_FOUND}})
    {
        resetFakeShell(error);
        const auto helper = HelperLauncher::launch(freshName(), {}, fakes());
        ASSERT_FALSE(helper.has_value());
        EXPECT_EQ(helper.error(), HelperLauncher::HELPER_MISSING);
    }
    resetFakeShell(ERROR_ACCESS_DENIED);
    const auto helper = HelperLauncher::launch(freshName(), {}, fakes());
    ASSERT_FALSE(helper.has_value());
    EXPECT_TRUE(helper.error().starts_with("couldn't start TaskSmackHelper.exe: ")) << helper.error();
}

TEST(HelperLauncherTest, AStartWithoutAProcessIsAFailure)
{
    resetFakeShell().noProcess = true;
    const auto helper = HelperLauncher::launch(freshName(), {}, fakes());
    ASSERT_FALSE(helper.has_value());
    EXPECT_EQ(helper.error(), "the helper started without a process to check");
}

TEST(HelperLauncherTest, AStoppedLaunchNeverAsks)
{
    const FakeShell& fake = resetFakeShell();
    std::stop_source stop;
    stop.request_stop();
    const auto helper = HelperLauncher::launch(freshName(), stop.get_token(), fakes());
    ASSERT_FALSE(helper.has_value());
    EXPECT_EQ(helper.error(), "cancelled");
    EXPECT_EQ(fake.calls, 0);
}

TEST(HelperLauncherTest, WithoutItsOwnFullPathNothingIsStarted)
{
    const FakeShell& fake = resetFakeShell();
    for (const auto getModuleFileName : {&relativeModuleFileName, &failingModuleFileName})
    {
        const auto helper =
            HelperLauncher::launch(freshName(), {}, {.shellExecuteEx = &fakeShellExecuteEx, .getModuleFileName = getModuleFileName});
        EXPECT_FALSE(helper.has_value());
    }
    EXPECT_EQ(fake.calls, 0) << "never a helper found relative to the current directory";
}

// --- The prompt's owner, and whether the helper is offered at all (#2550) -----------------------------

TEST(HelperLauncherTest, ThePromptBelongsToTheWindowItIsGiven)
{
    const FakeShell& fake = resetFakeShell();
    // Any window will do for the fake: the desktop's is always there, and nothing is shown.
    auto* const window = GetDesktopWindow();
    ASSERT_NE(window, nullptr);
    const auto helper = HelperLauncher::launch(freshName(), {}, fakes(), window);
    ASSERT_TRUE(helper.has_value()) << helper.error();
    EXPECT_EQ(fake.window, window) << "the UAC prompt comes up in front of TaskSmack";

    static_cast<void>(resetFakeShell());
    const auto unowned = HelperLauncher::launch(freshName(), {}, fakes());
    ASSERT_TRUE(unowned.has_value()) << unowned.error();
    EXPECT_EQ(fakeShell().window, nullptr);
}

TEST(HelperLauncherTest, TheBrokersLauncherPassesItsWindowOn)
{
    const FakeShell& fake = resetFakeShell(ERROR_CANCELLED);
    auto* const window = GetDesktopWindow();
    const PipeBroker::Launcher launcher = HelperLauncher::makeLauncher(fakes(), window);
    const auto helper = launcher(freshName(), {});
    EXPECT_FALSE(helper.has_value());
    EXPECT_EQ(fake.calls, 1);
    EXPECT_EQ(fake.window, window);
}

[[nodiscard]] std::expected<void, std::string> passingInstall(HANDLE /*process*/, std::span<const std::wstring> files)
{
    EXPECT_EQ(files.size(), 2U) << "TaskSmack.exe and the helper";
    return {};
}

[[nodiscard]] std::expected<void, std::string> changeableInstall(HANDLE /*process*/, std::span<const std::wstring> /*files*/)
{
    return std::unexpected(std::string("TaskSmack's user can change its folder"));
}

#ifdef TASKSMACK_WINDOWS_HELPER_PATH
/// A TaskSmack.exe beside the helper the build made: where availability() looks for it.
[[nodiscard]] std::wstring taskSmackBesideTheBuiltHelper()
{
    return BrokerHelper::besidePath(std::filesystem::path(TASKSMACK_WINDOWS_HELPER_PATH).wstring(), L"TaskSmack.exe");
}

TEST(HelperLauncherTest, TheHelperIsOfferedBesideAnInstallOnlyAnAdministratorCanChange)
{
    const auto available = HelperLauncher::availability(false, taskSmackBesideTheBuiltHelper(), &passingInstall);
    EXPECT_TRUE(available.has_value()) << available.error_or("");
}

TEST(HelperLauncherTest, AnElevatedTaskSmackNeedsNoHelper)
{
    const auto available = HelperLauncher::availability(true, taskSmackBesideTheBuiltHelper(), &passingInstall);
    ASSERT_FALSE(available.has_value());
    EXPECT_TRUE(available.error().contains("already runs as an administrator")) << available.error();
}

TEST(HelperLauncherTest, AnInstallItsUserCanChangeIsntOfferedTheHelper)
{
    const auto refused = HelperLauncher::availability(false, taskSmackBesideTheBuiltHelper(), &changeableInstall);
    ASSERT_FALSE(refused.has_value());
    EXPECT_TRUE(refused.error().contains("only an administrator can change")) << refused.error();

    // With the real check: the build's folder is this user's to change (and an administrator's, when
    // the tests run elevated), as a portable or per-user copy's would be.
    const auto real = HelperLauncher::availability(false, taskSmackBesideTheBuiltHelper(), &BrokerHelper::checkUnchangeableByParent);
    EXPECT_FALSE(real.has_value()) << "the build folder passed as one only an administrator can change";
}
#endif

// #2577 review, finding 1: only a split-token administrator's helper runs as TaskSmack's own user, and
// so can read other users' processes. A standard user's (Default, unelevated) runs as the administrator
// who typed their password, and reads only the user's own.
TEST(HelperLauncherTest, OnlyASplitTokenAdministratorsHelperReadsOtherUsers)
{
    EXPECT_TRUE(HelperLauncher::readsOtherUsers(TokenElevationTypeLimited));
    EXPECT_FALSE(HelperLauncher::readsOtherUsers(TokenElevationTypeDefault));
    EXPECT_FALSE(HelperLauncher::readsOtherUsers(TokenElevationTypeFull));
}

TEST(HelperLauncherTest, NoHelperBesideTaskSmackIsntOffered)
{
    // A folder of this test's own, with no helper in it.
    std::error_code error;
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path(error) / std::format(L"tasksmack-2550-{}", GetCurrentProcessId());
    std::filesystem::create_directories(folder, error);
    const auto missing = HelperLauncher::availability(false, (folder / L"TaskSmack.exe").wstring(), &passingInstall);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error(), HelperLauncher::HELPER_MISSING);
    EXPECT_FALSE(HelperLauncher::availability(false, L"TaskSmack.exe", &passingInstall).has_value()) << "not a full path";
    std::filesystem::remove_all(folder, error);
}
// --- Through PipeBroker -------------------------------------------------------------------------------

TEST(HelperLauncherTest, ADismissedPromptLeavesTheBrokerReadyToAskAgain)
{
    const FakeShell& fake = resetFakeShell(ERROR_CANCELLED);
    PipeBroker broker(HelperLauncher::makeLauncher(fakes()));
    const auto first = broker.connectWithin({}, WAIT);
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(first.error(), HelperLauncher::PROMPT_DISMISSED);
    EXPECT_FALSE(broker.connected());
    // The UI offers to try again: the next connect asks again.
    const auto second = broker.connectWithin({}, WAIT);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), HelperLauncher::PROMPT_DISMISSED);
    EXPECT_EQ(fake.calls, 2);
}

/// cmd.exe started suspended and ended at once with @p status: a helper that exited before it made its pipe.
[[nodiscard]] UniqueHandle exitedWith(DWORD status)
{
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    const std::wstring cmd = std::wstring(system.data(), length) + L"\\cmd.exe";
    std::wstring commandLine = L"cmd.exe /c exit 0";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (CreateProcessW(cmd.c_str(),
                       commandLine.data(),
                       nullptr,
                       nullptr,
                       FALSE,
                       CREATE_SUSPENDED | CREATE_NO_WINDOW,
                       nullptr,
                       nullptr,
                       &startup,
                       &info) == FALSE)
    {
        ADD_FAILURE() << "couldn't start a suspended cmd.exe: " << GetLastError();
        return {};
    }
    CloseHandle(info.hThread);
    UniqueHandle process(info.hProcess);
    EXPECT_NE(TerminateProcess(process.get(), status), FALSE);
    EXPECT_EQ(WaitForSingleObject(process.get(), static_cast<DWORD>(WAIT.count())), static_cast<DWORD>(WAIT_OBJECT_0));
    return process;
}

TEST(HelperLauncherTest, AHelperThatRefusedItsParentSaysSo)
{
    PipeBroker broker([](const std::wstring&, const std::stop_token&) -> std::expected<UniqueHandle, std::string>
                      { return exitedWith(static_cast<DWORD>(BrokerHelper::Exit::NoParent)); });
    const auto connected = broker.connectWithin({}, WAIT);
    ASSERT_FALSE(connected.has_value());
    EXPECT_TRUE(connected.error().contains(BrokerHelper::exitMeaning(static_cast<DWORD>(BrokerHelper::Exit::NoParent))))
        << connected.error();
    EXPECT_FALSE(broker.connected());
}

} // namespace
} // namespace Platform::Windows
