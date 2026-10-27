// Tests for HelperLauncher (#2549): how TaskSmack asks ShellExecuteExW("runas") to start
// TaskSmackHelper.exe, and what it makes of the answers. ShellExecuteExW is always a fake here, so no
// UAC prompt is ever raised and nothing runs elevated; test_BrokerHelper.cpp starts the real helper,
// unelevated, through the same launcher with CreateProcessW in its place. The helper's session is served
// on a thread of this process where a test needs its answers (BrokerHelper::serveParent(), #2769).

#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/HelperLauncher.h"
#include "Platform/Windows/PipeBroker.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
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
#include <sddl.h>
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

/// @p path in lower case: two spellings of one path compare equal, as NTFS compares names.
[[nodiscard]] std::wstring lowerCased(std::wstring path)
{
    std::ranges::transform(path, path.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return path;
}

// #2772: what TaskSmack says of itself is what the helper reads of it. Its image is the one
// BrokerHelper::imageOf() reads from its process, and its start time is BrokerHelper::startTicksOf()'s
// reading, the same number the helper checks the command line against.
TEST(HelperLauncherTest, TaskSmackNamesItselfAsTheHelperReadsIt)
{
    const FakeShell& fake = resetFakeShell();
    const auto helper = HelperLauncher::launch(freshName(), {}, fakes());
    ASSERT_TRUE(helper.has_value()) << helper.error();

    const auto image = BrokerHelper::imageOf(GetCurrentProcess());
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(lowerCased(fake.file), lowerCased(BrokerHelper::besidePath(*image, L"TaskSmackHelper.exe")));

    const auto read = readAsTheHelper(fake.parameters);
    ASSERT_TRUE(read.has_value()) << read.error();
    const auto ticks = BrokerHelper::startTicksOf(GetCurrentProcess());
    ASSERT_TRUE(ticks.has_value());
    EXPECT_EQ(read->parentStartTicks, *ticks);
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

// --- What the helper's answers teach (#2769) ---------------------------------------------------------

[[nodiscard]] UniqueHandle ownProcess()
{
    return UniqueHandle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId()));
}

/// This process, as the helper holds TaskSmack's: the handle the session checks its client against.
/// Open for the whole run.
[[nodiscard]] HANDLE self()
{
    static const UniqueHandle process = ownProcess();
    return process.get();
}

[[nodiscard]] BrokerPipe::Sid ownSid()
{
    UniqueHandle token;
    EXPECT_NE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()), FALSE);
    auto sid = BrokerPipe::userSidOfToken(token.get());
    EXPECT_TRUE(sid.has_value());
    return sid.value_or(BrokerPipe::Sid{});
}

/// Why the broker won't serve this test's own user, or nothing when it will: it serves only user
/// accounts (BrokerPipe::checkUserSid), which Windows Sandbox's WDAGUtilityAccount isn't (#2618).
[[nodiscard]] std::optional<std::string> brokerRefusesThisUser()
{
    UniqueHandle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        return std::nullopt;
    }
    const auto sid = BrokerPipe::userSidOfToken(token.get());
    if (!sid.has_value())
    {
        return std::nullopt;
    }
    const auto checked = BrokerPipe::checkUserSid(*sid);
    if (checked.has_value())
    {
        return std::nullopt;
    }
    return checked.error();
}

/// An administrator's account that isn't this test's user: what the helper runs as after a credentials
/// prompt answered with another account, or under Administrator Protection.
[[nodiscard]] BrokerPipe::Sid anotherAccount()
{
    PSID sid = nullptr;
    if (ConvertStringSidToSidW(L"S-1-5-21-1111111111-2222222222-3333333333-1001", &sid) == FALSE)
    {
        return {};
    }
    const auto* const first = static_cast<const std::byte*>(sid);
    BrokerPipe::Sid bytes(first,
                          first + GetLengthSid(sid)); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the SID's own length
    LocalFree(sid);
    return bytes;
}

[[nodiscard]] ProcessTarget ownTarget()
{
    return {.pid = static_cast<std::int32_t>(GetCurrentProcessId()), .startTimeTicks = startTicksOf(GetCurrentProcess())};
}

/// A process that isn't this user's, as far as the helper's session can tell: no process is.
constexpr ProcessTarget THEIRS{.pid = 4242, .startTimeTicks = 99};
/// A process whose read fails for another reason than whose it is.
constexpr ProcessTarget GONE{.pid = 4243, .startTimeTicks = 99};
/// The words of GONE's failure: not the helper's refusal.
constexpr std::string_view GONE_DETAIL = "the process has exited";

/// The helper's readers, as far as the session needs them: every read is Ok (GONE's Failed in its own
/// words), every action a success. Nothing is read; what reaches this has passed the session's policy.
[[nodiscard]] BrokerPipeServer::RequestHandler answeringReads()
{
    return [](const Broker::Request& request) -> Broker::Response
    {
        if (const auto* const modules = std::get_if<Broker::ModulesRequest>(&request))
        {
            if (modules->target == GONE)
            {
                return Broker::ModulesReply{
                    .result = {.status = ModulesReadStatus::Failed, .modules = {}, .detail = std::string(GONE_DETAIL)}};
            }
            return Broker::ModulesReply{.result = {.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}}};
        }
        if (std::holds_alternative<Broker::OpenFilesRequest>(request))
        {
            OpenFilesReadResult read;
            read.status = OpenFilesReadStatus::Ok;
            return Broker::OpenFilesReply{.result = std::move(read)};
        }
        if (std::holds_alternative<Broker::ConnectionsRequest>(request))
        {
            return Broker::ConnectionsReply{.result = {.status = ConnectionsReadStatus::Ok, .connections = {}, .detail = {}}};
        }
        if (std::holds_alternative<Broker::FirmwareFileRequest>(request))
        {
            return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Ok, .bytes = {0x5F, 0x53, 0x4D, 0x5F}};
        }
        return Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}};
    };
}

/// TaskSmackHelper.exe's session for this process, served on a thread of this one as the helper serves
/// TaskSmack's (BrokerHelper::serveParent()): for this user, from a helper running as @p helperUser,
/// so the session's policy is the real one, with @p handler behind it. launcher() is what a PipeBroker
/// starts it with; the pipe closes with the broker, which ends the session. Declare it before the broker.
class InProcessSession
{
  public:
    InProcessSession(BrokerPipe::Sid helperUser, BrokerPipeServer::RequestHandler handler)
        : m_User(ownSid()), m_HelperUser(std::move(helperUser)), m_Handler(std::move(handler))
    {}

    [[nodiscard]] PipeBroker::Launcher launcher()
    {
        return [this](const std::wstring& pipeName, const std::stop_token& /*stopToken*/) -> std::expected<UniqueHandle, std::string>
        {
            m_Thread = std::jthread(
                // NOLINTNEXTLINE(bugprone-exception-escape) - a test's helper thread: what escapes it ends the run, as a failed test should
                [this, pipeName, helperUser = m_HelperUser](const std::stop_token& stopToken)
                {
                    // How it ended isn't the tests' concern: the broker closing the pipe ends it.
                    static_cast<void>(BrokerHelper::serveParent(self(), m_User, helperUser, pipeName, m_Handler, WAIT, WAIT, stopToken));
                });
            return ownProcess();
        };
    }

    /// The account the next helper started runs as, as the next UAC prompt's answer decides (#2993).
    /// Each session keeps its own copy, so call it between sessions, before the connect that starts one.
    void setHelperUser(BrokerPipe::Sid helperUser)
    {
        m_HelperUser = std::move(helperUser);
    }

  private:
    BrokerPipe::Sid m_User;
    BrokerPipe::Sid m_HelperUser;
    BrokerPipeServer::RequestHandler m_Handler;
    std::jthread m_Thread; // last: joined first, while the rest is alive
};

/// A copy of the modules reply in @p response (which may be a temporary); when it isn't one, the
/// failure is noted and a Failed read in this test's own words stands in, so the test goes on.
[[nodiscard]] Broker::ModulesReply modulesReply(const std::expected<Broker::Response, std::string>& response)
{
    if (!response.has_value())
    {
        ADD_FAILURE() << response.error();
        return {.result = {.status = ModulesReadStatus::Failed, .modules = {}, .detail = "no reply: " + response.error()}};
    }
    if (const auto* const reply = std::get_if<Broker::ModulesReply>(&*response))
    {
        return *reply;
    }
    ADD_FAILURE() << "not a modules reply";
    return {.result = {.status = ModulesReadStatus::Failed, .modules = {}, .detail = "not a modules reply"}};
}

/// A launcher a test never expects to be called.
[[nodiscard]] PipeBroker::Launcher neverCalled()
{
    return [](const std::wstring&, const std::stop_token&) -> std::expected<UniqueHandle, std::string>
    {
        ADD_FAILURE() << "the helper was started";
        return std::unexpected(std::string("unused"));
    };
}

// A split-token administrator whose elevation runs as another account (Windows 11's Administrator
// Protection, or a credentials prompt answered with another administrator's account): the token says
// the helper reads other users' processes, the helper's first refusal says it doesn't, and that answer
// is kept for that helper, so "Read as administrator" isn't offered again while it runs.
TEST(HelperLauncherTest, AHelperApprovedForAnotherAccountTakesReadAsAdministratorBack)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    InProcessSession session(anotherAccount(), answeringReads());
    HelperLauncher::LearnedReadsBroker broker(
        std::make_unique<PipeBroker>(session.launcher(), HelperLauncher::readsOtherUsers(TokenElevationTypeLimited)));
    EXPECT_TRUE(broker.readsOtherUsers()) << "a split-token administrator's helper is expected to run as its user";
    EXPECT_FALSE(broker.lastSeenConnected());
    const auto connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_TRUE(broker.lastSeenConnected()) << "the inner broker's, as connect() left it (#3182)";
    EXPECT_TRUE(broker.connected());
    EXPECT_TRUE(broker.readsOtherUsers()) << "the handshake doesn't say which account the helper runs as";

    // The first read of a process that isn't this user's is refused, and the refusal the user sees
    // explains itself, in the helper's words' place.
    const Broker::ModulesReply refused = modulesReply(broker.call(Broker::ModulesRequest{.target = THEIRS}, {}));
    EXPECT_EQ(refused.result.status, ModulesReadStatus::Failed);
    EXPECT_EQ(refused.result.detail, HelperLauncher::OTHER_USERS_READS_UNAVAILABLE);
    EXPECT_FALSE(broker.readsOtherUsers()) << "learned: no more \"Read as administrator\" from this helper";

    // The helper is still there, and this user's own processes still read through it; that gives
    // nothing back.
    EXPECT_TRUE(broker.connected());
    const Broker::ModulesReply own = modulesReply(broker.call(Broker::ModulesRequest{.target = ownTarget()}, {}));
    EXPECT_EQ(own.result.status, ModulesReadStatus::Ok) << own.result.detail;
    EXPECT_FALSE(broker.readsOtherUsers());

    // Every read the helper refuses this way carries the explanation: the process reads that have
    // words, and the system-wide reads' ErrorReply, through callIfIdle() as through call().
    const auto files = broker.call(Broker::OpenFilesRequest{.target = THEIRS}, {});
    ASSERT_TRUE(files.has_value()) << files.error();
    ASSERT_TRUE(std::holds_alternative<Broker::OpenFilesReply>(*files));
    EXPECT_EQ(std::get<Broker::OpenFilesReply>(*files).result.status, OpenFilesReadStatus::Failed);
    EXPECT_EQ(std::get<Broker::OpenFilesReply>(*files).result.detail, HelperLauncher::OTHER_USERS_READS_UNAVAILABLE);
    const auto connections = broker.call(Broker::ConnectionsRequest{.target = THEIRS}, {});
    ASSERT_TRUE(connections.has_value()) << connections.error();
    ASSERT_TRUE(std::holds_alternative<Broker::ConnectionsReply>(*connections));
    EXPECT_EQ(std::get<Broker::ConnectionsReply>(*connections).result.status, ConnectionsReadStatus::Failed);
    EXPECT_EQ(std::get<Broker::ConnectionsReply>(*connections).result.detail, HelperLauncher::OTHER_USERS_READS_UNAVAILABLE);
    const auto firmware = broker.callIfIdle(Broker::FirmwareFileRequest{}, WAIT);
    ASSERT_TRUE(firmware.has_value()) << firmware.error();
    ASSERT_TRUE(std::holds_alternative<Broker::ErrorReply>(*firmware));
    EXPECT_EQ(std::get<Broker::ErrorReply>(*firmware).message, HelperLauncher::OTHER_USERS_READS_UNAVAILABLE);
    EXPECT_FALSE(broker.readsOtherUsers());

    // The lesson is this broker's: another TaskSmack's starts afresh.
    const HelperLauncher::LearnedReadsBroker fresh(
        std::make_unique<PipeBroker>(neverCalled(), HelperLauncher::readsOtherUsers(TokenElevationTypeLimited)));
    EXPECT_TRUE(fresh.readsOtherUsers());
}

// A credentials prompt is answered afresh each time (#2993): the refusal belongs to the helper that gave
// it. It holds while that helper runs and after it has gone, until PipeBroker starts another; that one
// is the token's again, keeps reading when its prompt was answered with the user's own account, and
// takes "Read as administrator" back again when it was another's.
TEST(HelperLauncherTest, ANewHelperIsExpectedToReadOtherUsersAgain)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    InProcessSession session(anotherAccount(), answeringReads());
    auto pipe = std::make_unique<PipeBroker>(session.launcher(), HelperLauncher::readsOtherUsers(TokenElevationTypeLimited));
    PipeBroker& helper = *pipe;
    HelperLauncher::LearnedReadsBroker broker(std::move(pipe));
    EXPECT_EQ(broker.sessionsStarted(), 0U);
    auto connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_EQ(broker.sessionsStarted(), 1U);
    EXPECT_EQ(modulesReply(broker.call(Broker::ModulesRequest{.target = THEIRS}, {})).result.detail,
              HelperLauncher::OTHER_USERS_READS_UNAVAILABLE);
    EXPECT_FALSE(broker.readsOtherUsers());

    // connect() while that helper runs is the same session, and keeps the lesson.
    connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_EQ(broker.sessionsStarted(), 1U);
    EXPECT_FALSE(broker.readsOtherUsers());

    // The helper is gone, and none has started since: still no.
    helper.disconnect();
    EXPECT_FALSE(broker.connected());
    EXPECT_FALSE(broker.readsOtherUsers());

    // The next prompt, answered with the user's own account: a new session, the token's again, and it reads.
    session.setHelperUser(ownSid());
    connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_EQ(broker.sessionsStarted(), 2U);
    EXPECT_TRUE(broker.readsOtherUsers());
    const Broker::ModulesReply theirs = modulesReply(broker.call(Broker::ModulesRequest{.target = THEIRS}, {}));
    EXPECT_EQ(theirs.result.status, ModulesReadStatus::Ok) << theirs.result.detail;
    EXPECT_TRUE(broker.readsOtherUsers());

    // And the one after, answered with another account again: learned again, from that helper.
    helper.disconnect();
    session.setHelperUser(anotherAccount());
    connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_EQ(broker.sessionsStarted(), 3U);
    EXPECT_TRUE(broker.readsOtherUsers());
    EXPECT_EQ(modulesReply(broker.call(Broker::ModulesRequest{.target = THEIRS}, {})).result.detail,
              HelperLauncher::OTHER_USERS_READS_UNAVAILABLE);
    EXPECT_FALSE(broker.readsOtherUsers());
}

// A split-token administrator whose elevation runs as them: the session is theirs, every process reads,
// and nothing the helper answers takes "Read as administrator" away.
TEST(HelperLauncherTest, AHelperRunningAsTaskSmacksUserKeepsReadAsAdministrator)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    InProcessSession session(ownSid(), answeringReads());
    HelperLauncher::LearnedReadsBroker broker(
        std::make_unique<PipeBroker>(session.launcher(), HelperLauncher::readsOtherUsers(TokenElevationTypeLimited)));
    const auto connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();

    const Broker::ModulesReply theirs = modulesReply(broker.call(Broker::ModulesRequest{.target = THEIRS}, {}));
    EXPECT_EQ(theirs.result.status, ModulesReadStatus::Ok) << "another account's process: read, as the session is this user's";
    EXPECT_TRUE(broker.readsOtherUsers());

    // A read that fails in other words, and a system-wide read that is answered, teach nothing.
    const Broker::ModulesReply gone = modulesReply(broker.call(Broker::ModulesRequest{.target = GONE}, {}));
    EXPECT_EQ(gone.result.status, ModulesReadStatus::Failed);
    EXPECT_EQ(gone.result.detail, GONE_DETAIL) << "left as the helper said it";
    EXPECT_TRUE(broker.readsOtherUsers());
    const auto firmware = broker.call(Broker::FirmwareFileRequest{}, {});
    ASSERT_TRUE(firmware.has_value()) << firmware.error();
    EXPECT_TRUE(std::holds_alternative<Broker::FirmwareFileReply>(*firmware));
    EXPECT_TRUE(broker.readsOtherUsers());
}

// Only the helper's own refusal teaches: not a transport failure, an action, a read refused without
// words (the environment's, the memory rollup's) or one that failed in other words; and nothing after
// gives the lesson back while no new helper starts.
TEST(HelperLauncherTest, OnlyTheHelpersOwnRefusalTeachesTheBroker)
{
    auto fake = std::make_unique<Mocks::FakeBroker>();
    Mocks::FakeBroker& inner = *fake;
    HelperLauncher::LearnedReadsBroker broker(std::move(fake));
    EXPECT_TRUE(broker.readsOtherUsers());
    EXPECT_TRUE(broker.connected());

    EXPECT_TRUE(broker.lastSeenConnected()); // the inner broker's (#3182)

    inner.setConnected(false);
    EXPECT_FALSE(broker.lastSeenConnected());
    EXPECT_FALSE(broker.connected());
    EXPECT_FALSE(broker.call(Broker::ModulesRequest{.target = THEIRS}, {}).has_value());
    EXPECT_TRUE(broker.readsOtherUsers());
    inner.setConnected(true);
    const std::array<Broker::Response, 4> silent{
        Broker::ActionResult{.success = false, .errorMessage = std::string(BrokerHelper::OTHER_USERS_READ_REFUSED), .successNote = {}},
        Broker::EnvironmentReply{.result = {.status = EnvironmentReadStatus::Failed, .variables = {}}},
        Broker::MemoryRollupReply{.result = std::unexpected(MemoryRollupFailure::Unavailable)},
        Broker::ModulesReply{.result = {.status = ModulesReadStatus::Failed, .modules = {}, .detail = "access is denied"}}};
    for (const Broker::Response& answer : silent) // `silent` outlives every answer given from it
    {
        inner.setAnswer([&answer](const Broker::Request&) { return answer; });
        EXPECT_TRUE(broker.call(Broker::ModulesRequest{.target = THEIRS}, {}).has_value());
        EXPECT_TRUE(broker.readsOtherUsers()) << "answer " << answer.index();
    }

    // The helper's refusal of a system-wide read, through callIfIdle(): learned, and reworded.
    inner.setAnswer([](const Broker::Request&)
                    { return Broker::Response{Broker::ErrorReply{.message = std::string(BrokerHelper::OTHER_USERS_READ_REFUSED)}}; });
    const auto refused = broker.callIfIdle(Broker::CrashesRequest{}, WAIT);
    ASSERT_TRUE(refused.has_value()) << refused.error();
    ASSERT_TRUE(std::holds_alternative<Broker::ErrorReply>(*refused));
    EXPECT_EQ(std::get<Broker::ErrorReply>(*refused).message, HelperLauncher::OTHER_USERS_READS_UNAVAILABLE);
    EXPECT_FALSE(broker.readsOtherUsers());
    EXPECT_EQ(inner.idleCallCount(), 1);

    inner.setAnswer(
        [](const Broker::Request&)
        { return Broker::Response{Broker::ModulesReply{.result = {.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}}}}; });
    EXPECT_TRUE(broker.call(Broker::ModulesRequest{.target = THEIRS}, {}).has_value());
    EXPECT_FALSE(broker.readsOtherUsers()) << "a read that then succeeds gives nothing back";
    EXPECT_EQ(inner.requests().size(), 7U) << "every call reached the helper";
}

/// The helper's refusal of another account's process: what a helper that runs as another account
/// answers a modules read with (BrokerHelper::OTHER_USERS_READ_REFUSED).
[[nodiscard]] Broker::Response refusedRead()
{
    return Broker::ModulesReply{
        .result = {.status = ModulesReadStatus::Failed, .modules = {}, .detail = std::string(BrokerHelper::OTHER_USERS_READ_REFUSED)}};
}

/// A modules read that succeeds.
[[nodiscard]] Broker::Response answeredRead()
{
    return Broker::ModulesReply{.result = {.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}}};
}

// The refusal belongs to the helper session that gave it (#2993): within that session it holds, through
// reads that then succeed, a connect() to the helper that is still running, the helper exiting and a
// prompt dismissed after; the next helper started is the token's again, and its own refusal is learned
// again, for it.
TEST(HelperLauncherTest, ARefusalLastsUntilANewHelperStarts)
{
    auto fake = std::make_unique<Mocks::FakeBroker>();
    Mocks::FakeBroker& inner = *fake;
    HelperLauncher::LearnedReadsBroker broker(std::move(fake));
    ASSERT_TRUE(broker.readsOtherUsers());
    EXPECT_EQ(broker.sessionsStarted(), 0U);

    inner.setAnswer([](const Broker::Request&) { return refusedRead(); });
    EXPECT_EQ(modulesReply(broker.call(Broker::ModulesRequest{.target = THEIRS}, {})).result.detail,
              HelperLauncher::OTHER_USERS_READS_UNAVAILABLE);
    EXPECT_FALSE(broker.readsOtherUsers());

    inner.setAnswer([](const Broker::Request&) { return answeredRead(); });
    EXPECT_TRUE(broker.call(Broker::ModulesRequest{.target = THEIRS}, {}).has_value());
    EXPECT_FALSE(broker.readsOtherUsers()) << "a read that then succeeds gives nothing back";
    auto connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_EQ(broker.sessionsStarted(), 0U) << "connected already: the same helper";
    EXPECT_FALSE(broker.readsOtherUsers());

    // The helper exits, and no new one starts: a dismissed prompt is no new session.
    inner.setConnected(false);
    EXPECT_FALSE(broker.readsOtherUsers());
    inner.setConnectFailure(std::string(HelperLauncher::PROMPT_DISMISSED));
    EXPECT_FALSE(broker.connect({}).has_value());
    EXPECT_EQ(broker.sessionsStarted(), 0U);
    EXPECT_FALSE(broker.readsOtherUsers());

    // A new helper: its prompt was answered afresh, so it is expected to read other users' processes.
    inner.setConnectFailure({});
    connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_EQ(broker.sessionsStarted(), 1U);
    EXPECT_TRUE(broker.readsOtherUsers());
    EXPECT_TRUE(broker.call(Broker::ModulesRequest{.target = THEIRS}, {}).has_value());
    EXPECT_TRUE(broker.readsOtherUsers());

    // That one refuses too, through callIfIdle(): learned again, for it.
    inner.setAnswer([](const Broker::Request&) { return refusedRead(); });
    EXPECT_EQ(modulesReply(broker.callIfIdle(Broker::ModulesRequest{.target = THEIRS}, WAIT)).result.detail,
              HelperLauncher::OTHER_USERS_READS_UNAVAILABLE);
    EXPECT_FALSE(broker.readsOtherUsers());
    inner.setConnected(false);
    EXPECT_FALSE(broker.readsOtherUsers());

    connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_EQ(broker.sessionsStarted(), 2U);
    EXPECT_TRUE(broker.readsOtherUsers());
}

// Under Administrator Protection every elevation runs as another account (#2993), so the first refusal
// holds while TaskSmack runs, through new helpers too, and says so.
TEST(HelperLauncherTest, UnderAdministratorProtectionARefusalLastsWhileTaskSmackRuns)
{
    auto fake = std::make_unique<Mocks::FakeBroker>();
    Mocks::FakeBroker& inner = *fake;
    HelperLauncher::LearnedReadsBroker broker(std::move(fake), HelperLauncher::RefusalLasts::ForTheProcess);
    ASSERT_TRUE(broker.readsOtherUsers()) << "told from the token until the helper says otherwise";

    inner.setAnswer([](const Broker::Request&) { return refusedRead(); });
    EXPECT_EQ(modulesReply(broker.call(Broker::ModulesRequest{.target = THEIRS}, {})).result.detail,
              HelperLauncher::ADMINISTRATOR_PROTECTION_READS_UNAVAILABLE);
    EXPECT_FALSE(broker.readsOtherUsers());

    inner.setConnected(false);
    const auto connected = broker.connect({});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_EQ(broker.sessionsStarted(), 1U);
    EXPECT_FALSE(broker.readsOtherUsers()) << "a new helper runs as another account too";
    EXPECT_EQ(modulesReply(broker.call(Broker::ModulesRequest{.target = THEIRS}, {})).result.detail,
              HelperLauncher::ADMINISTRATOR_PROTECTION_READS_UNAVAILABLE);
    EXPECT_FALSE(broker.readsOtherUsers());
}

// A standard user's helper never offered reads (#2550): the broker starts as told, and stays so, through
// a refusal and the new helper after it.
TEST(HelperLauncherTest, AStandardUsersBrokerNeverOffersReads)
{
    auto fake = std::make_unique<Mocks::FakeBroker>();
    Mocks::FakeBroker& inner = *fake;
    fake->setReadsOtherUsers(HelperLauncher::readsOtherUsers(TokenElevationTypeDefault));
    HelperLauncher::LearnedReadsBroker broker(std::move(fake));
    EXPECT_FALSE(broker.readsOtherUsers());

    inner.setAnswer([](const Broker::Request&) { return refusedRead(); });
    EXPECT_TRUE(broker.call(Broker::ModulesRequest{.target = THEIRS}, {}).has_value());
    inner.setConnected(false);
    ASSERT_TRUE(broker.connect({}).has_value());
    EXPECT_EQ(broker.sessionsStarted(), 1U);
    EXPECT_FALSE(broker.readsOtherUsers());
}

} // namespace
} // namespace Platform::Windows
