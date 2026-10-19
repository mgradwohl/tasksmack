// Tests for TaskSmackHelper.exe (#2517): its command line, the parent checks (the parent's handle, start
// time and user, and that it is the TaskSmack beside the helper that started it, #2549), the watch that
// ends a session when TaskSmack exits, the whole session run in this process, and the executable itself,
// started directly as this user, never elevated: no UAC prompt is ever raised. Where "runas" would make
// the helper TaskSmack's child, the tests make it so with PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, as
// AppInfo does, or have a stand-in TaskSmack.exe start it (HelperTestParent.cpp). The processes the tests
// act on are their own suspended children, which never run. Every wait is bounded.

#include "Mocks/MockProbes.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerRequestMapping.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <sddl.h>
#include <winver.h>

namespace Platform::Windows
{
namespace
{

using namespace std::chrono_literals;

/// The bound on anything that should happen at once: a test that waits this long has failed.
constexpr std::chrono::milliseconds WAIT{10'000};
/// How long a test gives a freshly started TaskSmackHelper.exe to make its pipe: a cold runner's
/// antivirus may scan the new image first. ACCEPT_TIMEOUT, as the helper gives TaskSmack.
constexpr std::chrono::milliseconds STARTUP_WAIT{BrokerPipeServer::ACCEPT_TIMEOUT};

[[nodiscard]] std::wstring freshName()
{
    auto name = BrokerPipe::makePipeName();
    EXPECT_TRUE(name.has_value());
    return name.value_or(L"");
}

/// @p process's creation time, in a FILETIME's ticks: what TaskSmack passes as its start time.
[[nodiscard]] std::uint64_t startTicksOf(HANDLE process)
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    EXPECT_NE(GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime), FALSE);
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
}

[[nodiscard]] UniqueHandle ownProcess()
{
    return UniqueHandle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId()));
}

[[nodiscard]] BrokerHelper::Arguments argumentsForThisProcess(std::wstring pipeName)
{
    return {.pipeName = std::move(pipeName), .parentPid = GetCurrentProcessId(), .parentStartTicks = startTicksOf(GetCurrentProcess())};
}

/// @p process's image, as a Win32 path.
[[nodiscard]] std::wstring imageOf(HANDLE process)
{
    std::array<wchar_t, 32'768> path{};
    auto size = static_cast<DWORD>(path.size());
    EXPECT_NE(QueryFullProcessImageNameW(process, 0, path.data(), &size), FALSE);
    return {path.data(), size};
}

/// Now, in a FILETIME's ticks: later than any process the tests name started. The precise clock, which
/// process creation times are read from: the coarse one can lag a child created a moment ago.
[[nodiscard]] std::uint64_t nowTicks()
{
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return (static_cast<std::uint64_t>(now.dwHighDateTime) << 32U) | now.dwLowDateTime;
}

/// Install checks for in-process helpers: one that passes any install, one that passes none.
[[nodiscard]] std::expected<void, std::string> anyInstall(HANDLE /*parent*/, std::span<const std::wstring> /*files*/)
{
    return {};
}

[[nodiscard]] std::expected<void, std::string> noInstall(HANDLE /*parent*/, std::span<const std::wstring> /*files*/)
{
    return std::unexpected(std::string("TaskSmack's user can change the install"));
}

/// What an in-process helper "started" by @p parent knows of itself: @p parent's image is its
/// TaskSmack, @p parent created it, it started now, and its install passes (the build's folder, which
/// this user can write, couldn't: checkUnchangeableBy() has tests of its own).
[[nodiscard]] BrokerHelper::Self selfStartedBy(HANDLE parent)
{
    return {.taskSmackPath = imageOf(parent),
            .creatorPid = GetProcessId(parent),
            .startTicks = nowTicks(),
            .helperPath = imageOf(parent),
            .installCheck = &anyInstall};
}

[[nodiscard]] std::chrono::milliseconds since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

/// Waits up to STARTUP_WAIT for pipe @p name to exist: the helper has opened its parent by then.
[[nodiscard]] bool pipeExists(const std::wstring& name)
{
    const auto deadline = std::chrono::steady_clock::now() + STARTUP_WAIT;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (WaitNamedPipeW(name.c_str(), NMPWAIT_USE_DEFAULT_WAIT) != FALSE || GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

/// A child process created suspended, so it never runs: cmd.exe, which every Windows has. Terminated
/// and closed on scope exit, if a test hasn't ended it.
class SuspendedChild
{
  public:
    SuspendedChild()
    {
        std::array<wchar_t, MAX_PATH> system{};
        const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        EXPECT_GT(length, 0U);
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
            return;
        }
        m_Process.reset(info.hProcess);
        m_Thread.reset(info.hThread);
        m_Pid = info.dwProcessId;
    }
    ~SuspendedChild()
    {
        if (m_Process)
        {
            TerminateProcess(m_Process.get(), 1);
            WaitForSingleObject(m_Process.get(), static_cast<DWORD>(WAIT.count()));
        }
    }
    SuspendedChild(const SuspendedChild&) = delete;
    SuspendedChild& operator=(const SuspendedChild&) = delete;
    SuspendedChild(SuspendedChild&&) = delete;
    SuspendedChild& operator=(SuspendedChild&&) = delete;

    [[nodiscard]] HANDLE process() const noexcept
    {
        return m_Process.get();
    }
    [[nodiscard]] DWORD pid() const noexcept
    {
        return m_Pid;
    }
    [[nodiscard]] ProcessTarget target() const
    {
        return {.pid = static_cast<std::int32_t>(m_Pid), .startTimeTicks = startTicksOf(m_Process.get())};
    }
    /// Whether it has exited, waiting up to @p timeout for it to.
    [[nodiscard]] bool exited(std::chrono::milliseconds timeout = WAIT) const
    {
        return WaitForSingleObject(m_Process.get(), static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
    }

  private:
    UniqueHandle m_Process;
    UniqueHandle m_Thread;
    DWORD m_Pid = 0;
};

// --- The command line --------------------------------------------------------------------------------

[[nodiscard]] std::expected<BrokerHelper::Arguments, std::string> parse(std::initializer_list<std::wstring_view> arguments)
{
    const std::vector<std::wstring_view> list(arguments);
    return BrokerHelper::parseArguments(list);
}

TEST(BrokerHelperTest, TheCommandLineTaskSmackMakesIsParsed)
{
    const std::wstring name = freshName();
    const auto parsed = parse({L"--broker", name, L"1234", L"133000000000000000"});
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->pipeName, name);
    EXPECT_EQ(parsed->parentPid, 1234U);
    EXPECT_EQ(parsed->parentStartTicks, 133000000000000000ULL);
}

TEST(BrokerHelperTest, TheCommandLineRoundTrips)
{
    const BrokerHelper::Arguments arguments{.pipeName = freshName(), .parentPid = 4294967295U, .parentStartTicks = 18446744073709551615ULL};
    const std::wstring line = BrokerHelper::commandLine(arguments);
    std::vector<std::wstring_view> split;
    for (const auto part : std::views::split(std::wstring_view(line), L' '))
    {
        split.emplace_back(part.begin(), part.end());
    }
    const auto parsed = BrokerHelper::parseArguments(split);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->pipeName, arguments.pipeName);
    EXPECT_EQ(parsed->parentPid, arguments.parentPid);
    EXPECT_EQ(parsed->parentStartTicks, arguments.parentStartTicks);
}

TEST(BrokerHelperTest, AnythingElseOnTheCommandLineIsRefused)
{
    const std::wstring name = freshName();
    const std::wstring upper = [&name]
    {
        std::wstring text = name;
        text.back() = L'A';
        return text;
    }();
    const std::vector<std::vector<std::wstring_view>> refused{
        {},
        {L"--broker"},
        {L"--broker", name, L"1234"},
        {L"--broker", name, L"1234", L"99", L"extra"},
        {L"--other", name, L"1234", L"99"},
        {L"--broker", L"\\\\.\\pipe\\SomethingElse", L"1234", L"99"},
        {L"--broker", L"\\\\.\\pipe\\TaskSmack.Broker.00", L"1234", L"99"},
        {L"--broker", upper, L"1234", L"99"},
        {L"--broker", name + L"0", L"1234", L"99"},
        {L"--broker", L"\\\\server\\pipe\\TaskSmack.Broker.0123456789abcdef0123456789abcdef", L"1234", L"99"},
        {L"--broker", name, L"0", L"99"},
        {L"--broker", name, L"01234", L"99"},
        {L"--broker", name, L"-1", L"99"},
        {L"--broker", name, L"+1", L"99"},
        {L"--broker", name, L" 1", L"99"},
        {L"--broker", name, L"4294967296", L"99"},
        {L"--broker", name, L"12a", L"99"},
        {L"--broker", name, L"", L"99"},
        {L"--broker", name, L"1234", L"0"},
        {L"--broker", name, L"1234", L"18446744073709551616"},
        {L"--broker", name, L"1234", L"S-1-5-21-1-2-3-1001"},
    };
    for (const auto& arguments : refused)
    {
        EXPECT_FALSE(BrokerHelper::parseArguments(arguments).has_value()) << arguments.size();
    }
}

TEST(BrokerHelperTest, OnlyNamesMakePipeNameMakesAreBrokerPipes)
{
    EXPECT_TRUE(BrokerPipe::isPipeName(freshName()));
    EXPECT_TRUE(BrokerPipe::isPipeName(L"\\\\.\\pipe\\TaskSmack.Broker.0123456789abcdef0123456789abcdef"));
    EXPECT_FALSE(BrokerPipe::isPipeName(L"\\\\.\\pipe\\TaskSmack.Broker.0123456789abcdef0123456789abcdeg"));
    EXPECT_FALSE(BrokerPipe::isPipeName(L"\\\\.\\pipe\\TaskSmack.Broker.0123456789ABCDEF0123456789abcdef"));
    EXPECT_FALSE(BrokerPipe::isPipeName(L"\\\\.\\pipe\\TaskSmack.Broker.0123456789abcdef0123456789abcde"));
    EXPECT_FALSE(BrokerPipe::isPipeName(L"\\\\.\\pipe\\TaskSmack.Broker."));
    EXPECT_FALSE(BrokerPipe::isPipeName(L"\\\\.\\pipe\\TaskSmack.Broker.0123456789abcdef0123456789abcdef\\..\\x"));
    EXPECT_FALSE(BrokerPipe::isPipeName(L""));
}

// --- The parent --------------------------------------------------------------------------------------

TEST(BrokerHelperTest, TheParentIsOpenedWhenItsStartTimeMatches)
{
    const auto parent = BrokerHelper::openParent(GetCurrentProcessId(), startTicksOf(GetCurrentProcess()));
    ASSERT_TRUE(parent.has_value()) << parent.error();
    EXPECT_EQ(GetProcessId(parent->get()), GetCurrentProcessId());
    EXPECT_EQ(WaitForSingleObject(parent->get(), 0), static_cast<DWORD>(WAIT_TIMEOUT)) << "opened with SYNCHRONIZE";
}

TEST(BrokerHelperTest, AParentOfAnotherStartTimeIsRefused)
{
    const auto parent = BrokerHelper::openParent(GetCurrentProcessId(), startTicksOf(GetCurrentProcess()) + 1);
    ASSERT_FALSE(parent.has_value());
    EXPECT_TRUE(parent.error().contains("isn't the TaskSmack")) << parent.error();
    EXPECT_FALSE(BrokerHelper::openParent(0, 1).has_value());
    EXPECT_FALSE(BrokerHelper::openParent(GetCurrentProcessId(), 0).has_value());
}

TEST(BrokerHelperTest, AParentThatHasExitedIsRefused)
{
    std::optional<ProcessTarget> target;
    {
        const SuspendedChild child;
        ASSERT_TRUE(child.process() != nullptr);
        target = child.target();
        ASSERT_NE(TerminateProcess(child.process(), 1), FALSE);
        ASSERT_TRUE(child.exited());
        // Still held here, so its PID can't be reused: the check sees an exited process, not another.
        const auto parent = BrokerHelper::openParent(static_cast<DWORD>(target->pid), target->startTimeTicks);
        EXPECT_FALSE(parent.has_value());
    }
}

// --- That the parent is the TaskSmack that started the helper (#2548 review, finding 3) -------------

TEST(BrokerHelperTest, BesidePathNamesTheFileInTheSameFolder)
{
    EXPECT_EQ(BrokerHelper::besidePath(L"C:\\Program Files\\TaskSmack\\TaskSmack.exe", L"TaskSmackHelper.exe"),
              L"C:\\Program Files\\TaskSmack\\TaskSmackHelper.exe");
    EXPECT_EQ(BrokerHelper::besidePath(L"C:\\TaskSmack.exe", L"TaskSmackHelper.exe"), L"C:\\TaskSmackHelper.exe");
    EXPECT_EQ(BrokerHelper::besidePath(L"TaskSmack.exe", L"TaskSmackHelper.exe"), L"") << "relative: the current directory's";
    EXPECT_EQ(BrokerHelper::besidePath(L"Tools\\TaskSmack.exe", L"TaskSmackHelper.exe"), L"");
    EXPECT_EQ(BrokerHelper::besidePath(L"C:TaskSmack.exe", L"TaskSmackHelper.exe"), L"") << "the drive's current directory";
    EXPECT_EQ(BrokerHelper::besidePath(L"C:\\", L"TaskSmackHelper.exe"), L"");
    EXPECT_EQ(BrokerHelper::besidePath(L"", L"TaskSmackHelper.exe"), L"");
}

TEST(BrokerHelperTest, SameFileComparesWhatPathsNameNotHowTheyAreSpelled)
{
    const std::wstring own = imageOf(GetCurrentProcess());
    const std::filesystem::path path(own);
    EXPECT_TRUE(BrokerHelper::sameFile(own, own));
    std::wstring upper = own;
    std::ranges::transform(upper, upper.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    EXPECT_TRUE(BrokerHelper::sameFile(own, upper)) << "case";
    EXPECT_TRUE(BrokerHelper::sameFile(own, (path.parent_path() / L"." / path.filename()).wstring()));
    EXPECT_TRUE(BrokerHelper::sameFile(own, (path.parent_path() / L"NoSuchFolder" / L".." / path.filename()).wstring()));
    std::array<wchar_t, 32'768> shortPath{};
    const DWORD shortLength = GetShortPathNameW(own.c_str(), shortPath.data(), static_cast<DWORD>(shortPath.size()));
    if (shortLength > 0 && shortLength < shortPath.size())
    {
        EXPECT_TRUE(BrokerHelper::sameFile(own, std::wstring(shortPath.data(), shortLength))) << "an 8.3 name, if the volume has them";
    }

    EXPECT_FALSE(BrokerHelper::sameFile(own, imageOf(SuspendedChild().process()))) << "another executable";
    EXPECT_FALSE(BrokerHelper::sameFile(own, (path.parent_path() / L"TaskSmack.exe.missing").wstring()));
    EXPECT_FALSE(BrokerHelper::sameFile(own, L""));
}

TEST(BrokerHelperTest, TheTaskSmackThatStartedTheHelperIsItsParent)
{
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const auto parent = BrokerHelper::openParent(arguments.parentPid, arguments.parentStartTicks);
    ASSERT_TRUE(parent.has_value()) << parent.error();
    const auto checked = BrokerHelper::checkParentIsTaskSmack(parent->get(), arguments, selfStartedBy(GetCurrentProcess()));
    EXPECT_TRUE(checked.has_value()) << checked.error();
}

TEST(BrokerHelperTest, AParentThatDidntStartTheHelperIsRefused)
{
    // Another program starting the helper through the UAC prompt and naming a TaskSmack: the helper's
    // creator isn't the process named.
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const auto parent = BrokerHelper::openParent(arguments.parentPid, arguments.parentStartTicks);
    ASSERT_TRUE(parent.has_value()) << parent.error();
    const SuspendedChild other;
    ASSERT_TRUE(other.process() != nullptr);
    BrokerHelper::Self self = selfStartedBy(GetCurrentProcess());
    self.creatorPid = other.pid();
    const auto checked = BrokerHelper::checkParentIsTaskSmack(parent->get(), arguments, self);
    ASSERT_FALSE(checked.has_value());
    EXPECT_TRUE(checked.error().contains("started the helper")) << checked.error();
}

TEST(BrokerHelperTest, AParentThatStartedAfterTheHelperIsRefused)
{
    // Its creator exited, and its PID went to a later process: that one can't have started the helper.
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const auto parent = BrokerHelper::openParent(arguments.parentPid, arguments.parentStartTicks);
    ASSERT_TRUE(parent.has_value()) << parent.error();
    BrokerHelper::Self self = selfStartedBy(GetCurrentProcess());
    self.startTicks = arguments.parentStartTicks - 1;
    const auto checked = BrokerHelper::checkParentIsTaskSmack(parent->get(), arguments, self);
    ASSERT_FALSE(checked.has_value());
    EXPECT_TRUE(checked.error().contains("started after")) << checked.error();
}

TEST(BrokerHelperTest, AParentRunningAnotherExecutableIsRefused)
{
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const auto parent = BrokerHelper::openParent(arguments.parentPid, arguments.parentStartTicks);
    ASSERT_TRUE(parent.has_value()) << parent.error();
    const std::filesystem::path own(imageOf(GetCurrentProcess()));
    const SuspendedChild cmd;
    ASSERT_TRUE(cmd.process() != nullptr);
    for (const std::wstring& taskSmack : {imageOf(cmd.process()),
                                          (own.parent_path() / L"TaskSmack.exe").wstring(), // the TaskSmack beside this, which isn't it
                                          std::wstring()})
    {
        BrokerHelper::Self self = selfStartedBy(GetCurrentProcess());
        self.taskSmackPath = taskSmack;
        const auto checked = BrokerHelper::checkParentIsTaskSmack(parent->get(), arguments, self);
        ASSERT_FALSE(checked.has_value());
        EXPECT_TRUE(checked.error().contains("isn't the TaskSmack.exe beside the helper")) << checked.error();
    }
}

// #2572 review, finding 1: the TaskSmack.exe beside the helper counts only in an install its user can't
// change, or a program could copy the helper into a folder of its own, beside itself.

TEST(BrokerHelperTest, ATaskSmackInAnInstallItsUserCanChangeIsRefused)
{
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const auto parent = BrokerHelper::openParent(arguments.parentPid, arguments.parentStartTicks);
    ASSERT_TRUE(parent.has_value()) << parent.error();
    BrokerHelper::Self self = selfStartedBy(GetCurrentProcess());
    self.installCheck = &noInstall;
    auto checked = BrokerHelper::checkParentIsTaskSmack(parent->get(), arguments, self);
    ASSERT_FALSE(checked.has_value());
    EXPECT_TRUE(checked.error().contains("can change")) << checked.error();
    self.installCheck = nullptr;
    EXPECT_FALSE(BrokerHelper::checkParentIsTaskSmack(parent->get(), arguments, self).has_value()) << "no check is a refusal";
    // The real check, with this process as TaskSmack: the build's folder is this user's to change.
    self.installCheck = &BrokerHelper::checkUnchangeableByParent;
    checked = BrokerHelper::checkParentIsTaskSmack(parent->get(), arguments, self);
    ASSERT_FALSE(checked.has_value());
    EXPECT_TRUE(checked.error().contains("can change")) << checked.error();
}

/// This process's token for identification, with Administrators deny-only (CreateRestrictedToken): a
/// standard user's rights, even on a runner that runs its tests as an administrator.
[[nodiscard]] UniqueHandle standardUserToken()
{
    HANDLE own = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &own) == FALSE)
    {
        ADD_FAILURE() << "couldn't open this process's token: " << GetLastError();
        return {};
    }
    const UniqueHandle ownToken(own);
    std::array<std::byte, SECURITY_MAX_SID_SIZE> administrators{};
    auto size = static_cast<DWORD>(administrators.size());
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size) == FALSE)
    {
        ADD_FAILURE() << "couldn't make the Administrators SID: " << GetLastError();
        return {};
    }
    SID_AND_ATTRIBUTES disable{.Sid = administrators.data(), .Attributes = 0};
    HANDLE restricted = nullptr;
    if (CreateRestrictedToken(ownToken.get(), DISABLE_MAX_PRIVILEGE, 1, &disable, 0, nullptr, 0, nullptr, &restricted) == FALSE)
    {
        ADD_FAILURE() << "couldn't restrict this process's token: " << GetLastError();
        return {};
    }
    const UniqueHandle restrictedToken(restricted);
    HANDLE identification = nullptr;
    if (DuplicateToken(restrictedToken.get(), SecurityIdentification, &identification) == FALSE)
    {
        ADD_FAILURE() << "couldn't copy the restricted token: " << GetLastError();
        return {};
    }
    return UniqueHandle(identification);
}

[[nodiscard]] std::wstring systemFile(std::wstring_view name)
{
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    return (std::filesystem::path(std::wstring(system.data(), length)) / name).wstring();
}

TEST(BrokerHelperTest, AnInstallInAFolderOnlyAdministratorsChangePasses)
{
    // System32 stands in for Program Files\TaskSmack: a standard user may change neither exe, the folder
    // nor any folder above it.
    const UniqueHandle token = standardUserToken();
    ASSERT_TRUE(token);
    const std::array<std::wstring, 2> files{systemFile(L"cmd.exe"), systemFile(L"kernel32.dll")};
    const auto checked = BrokerHelper::checkUnchangeableBy(token.get(), files);
    EXPECT_TRUE(checked.has_value()) << checked.error();
}

TEST(BrokerHelperTest, AnInstallInAFolderItsUserCanChangeIsRefused)
{
    const UniqueHandle token = standardUserToken();
    ASSERT_TRUE(token);
    const std::wstring name = freshName();
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / (L"TaskSmackInstallTest." + name.substr(name.rfind(L'.') + 1));
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    ASSERT_FALSE(error) << error.message();
    const std::filesystem::path file = folder / L"TaskSmack.exe";
    std::ofstream(file) << "not TaskSmack";
    const std::array<std::wstring, 1> mine{file.wstring()};
    const auto checked = BrokerHelper::checkUnchangeableBy(token.get(), mine);
    ASSERT_FALSE(checked.has_value());
    EXPECT_TRUE(checked.error().contains("can change")) << checked.error();
    // A protected file in a folder its user can change still isn't protected: here, its own copy.
    const std::array<std::wstring, 2> mixed{systemFile(L"cmd.exe"), file.wstring()};
    EXPECT_FALSE(BrokerHelper::checkUnchangeableBy(token.get(), mixed).has_value());
    std::filesystem::remove_all(folder, error);

    const std::array<std::wstring, 1> missing{(folder / L"TaskSmack.exe").wstring()};
    EXPECT_FALSE(BrokerHelper::checkUnchangeableBy(token.get(), missing).has_value()) << "a file that isn't there";
    EXPECT_FALSE(BrokerHelper::checkUnchangeableBy(token.get(), {}).has_value()) << "nothing to check";
}

TEST(BrokerHelperTest, ASessionForAParentThatIsntTaskSmackNeverMakesItsPipe)
{
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const BrokerPipeServer::RequestHandler handler = [](const Broker::Request&) -> Broker::Response
    {
        return Broker::ErrorReply{};
    };
    const SuspendedChild cmd;
    ASSERT_TRUE(cmd.process() != nullptr);
    BrokerHelper::Self self = selfStartedBy(GetCurrentProcess());
    self.taskSmackPath = imageOf(cmd.process());
    EXPECT_EQ(BrokerHelper::run(arguments, self, handler, WAIT, WAIT, {}), BrokerHelper::Exit::NoParent);
    self = selfStartedBy(cmd.process());
    self.taskSmackPath = imageOf(GetCurrentProcess());
    EXPECT_EQ(BrokerHelper::run(arguments, self, handler, WAIT, WAIT, {}), BrokerHelper::Exit::NoParent);
    const UniqueHandle own = ownProcess();
    EXPECT_FALSE(BrokerPipe::connectToServer(arguments.pipeName, GetCurrentProcessId(), own.get(), 100ms, {}).has_value());
}

TEST(BrokerHelperTest, TheHelpersOwnIdentityIsReadFromItsProcess)
{
    const auto self = BrokerHelper::readSelf();
    ASSERT_TRUE(self.has_value()) << self.error();
    EXPECT_EQ(self->taskSmackPath, BrokerHelper::besidePath(imageOf(GetCurrentProcess()), L"TaskSmack.exe"));
    EXPECT_NE(self->creatorPid, 0U);
    EXPECT_NE(self->creatorPid, GetCurrentProcessId());
    EXPECT_EQ(self->startTicks, startTicksOf(GetCurrentProcess()));
    EXPECT_EQ(self->helperPath, imageOf(GetCurrentProcess()));
    EXPECT_EQ(self->installCheck, &BrokerHelper::checkUnchangeableByParent) << "the real check, never a test's";
}

TEST(BrokerHelperTest, ExitStatusesAreSaidInWords)
{
    EXPECT_TRUE(BrokerHelper::exitMeaning(static_cast<DWORD>(BrokerHelper::Exit::NoParent)).contains("didn't accept TaskSmack"));
    EXPECT_TRUE(BrokerHelper::exitMeaning(static_cast<DWORD>(BrokerHelper::Exit::Usage)).contains("command line"));
    EXPECT_EQ(BrokerHelper::exitMeaning(77), "the helper exited with status 77");
}

// --- The watch on the parent ------------------------------------------------------------------------

TEST(BrokerHelperTest, TheWatchStopsWhenTheProcessEnds)
{
    // A manual-reset event stands in for the parent's process handle: set, it has exited.
    const UniqueHandle parent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(parent);
    const BrokerHelper::ProcessWatch watch(parent.get(), {});
    EXPECT_FALSE(watch.token().stop_requested());
    std::promise<void> stopped;
    const std::stop_callback onStop(watch.token(), [&stopped] { stopped.set_value(); });
    ASSERT_NE(SetEvent(parent.get()), FALSE);
    EXPECT_EQ(stopped.get_future().wait_for(WAIT), std::future_status::ready);
}

TEST(BrokerHelperTest, TheWatchStopsWithItsOuterToken)
{
    const UniqueHandle parent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(parent);
    std::stop_source outer;
    const BrokerHelper::ProcessWatch watch(parent.get(), outer.get_token());
    outer.request_stop();
    EXPECT_TRUE(watch.token().stop_requested());
}

TEST(BrokerHelperTest, AWatchOnARunningProcessEndsAtOnceWhenDestroyed)
{
    const UniqueHandle parent = ownProcess();
    const auto start = std::chrono::steady_clock::now();
    {
        const BrokerHelper::ProcessWatch watch(parent.get(), {});
        EXPECT_FALSE(watch.token().stop_requested());
    }
    EXPECT_LT(since(start), WAIT);
}

// --- acceptClient() and the parent's handle (#2520 review, finding 2) -------------------------------

TEST(BrokerHelperTest, AClientThatIsntTheParentsProcessIsRefused)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.process() != nullptr);
    const std::wstring name = freshName();
    const auto sid = BrokerPipe::userSidOfProcess(GetCurrentProcess());
    ASSERT_TRUE(sid.has_value()) << sid.error();
    const auto server = BrokerPipe::createServerPipe(name, *sid);
    ASSERT_TRUE(server.has_value()) << server.error();
    auto accepted = std::async(std::launch::async,
                               [pipe = server->get(), &child] { return BrokerPipe::acceptClient(pipe, child.process(), WAIT, {}); });
    const UniqueHandle self = ownProcess();
    const auto client = BrokerPipe::connectToServer(name, GetCurrentProcessId(), self.get(), WAIT, {});
    ASSERT_EQ(accepted.wait_for(WAIT), std::future_status::ready);
    const auto result = accepted.get();
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().contains("not TaskSmack")) << result.error();
    if (client.has_value())
    {
        const auto frame = BrokerPipe::receiveFrame(client->get(), WAIT, {});
        ASSERT_FALSE(frame.has_value());
        EXPECT_NE(frame.error(), BrokerPipe::ReceiveEnd::TimedOut) << "the refused client is cut off, not left waiting";
    }
}

TEST(BrokerHelperTest, AcceptGivesUpWhenTheParentExits)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.process() != nullptr);
    const auto sid = BrokerPipe::userSidOfProcess(GetCurrentProcess());
    ASSERT_TRUE(sid.has_value()) << sid.error();
    const auto server = BrokerPipe::createServerPipe(freshName(), *sid);
    ASSERT_TRUE(server.has_value()) << server.error();
    const auto start = std::chrono::steady_clock::now();
    // A minute's accept timeout: only the parent's exit can end the wait within WAIT.
    auto accepted =
        std::async(std::launch::async, [pipe = server->get(), &child] { return BrokerPipe::acceptClient(pipe, child.process(), 60s, {}); });
    ASSERT_NE(TerminateProcess(child.process(), 1), FALSE);
    ASSERT_EQ(accepted.wait_for(WAIT), std::future_status::ready);
    const auto result = accepted.get();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "TaskSmack exited before it opened the pipe");
    EXPECT_LT(since(start), WAIT);
}

TEST(BrokerHelperTest, AcceptRefusesAParentAlreadyGone)
{
    const UniqueHandle gone(CreateEventW(nullptr, TRUE, TRUE, nullptr)); // GetProcessId() can't name it
    ASSERT_TRUE(gone);
    const auto sid = BrokerPipe::userSidOfProcess(GetCurrentProcess());
    ASSERT_TRUE(sid.has_value()) << sid.error();
    const auto server = BrokerPipe::createServerPipe(freshName(), *sid);
    ASSERT_TRUE(server.has_value()) << server.error();
    EXPECT_FALSE(BrokerPipe::acceptClient(server->get(), gone.get(), WAIT, {}).has_value());

    const SuspendedChild child;
    ASSERT_TRUE(child.process() != nullptr);
    ASSERT_NE(TerminateProcess(child.process(), 1), FALSE);
    ASSERT_TRUE(child.exited());
    const auto result = BrokerPipe::acceptClient(server->get(), child.process(), WAIT, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "TaskSmack exited before it opened the pipe");
}

// --- The session, in this process -------------------------------------------------------------------

/// Sends @p request on @p pipe and returns the reply; an ErrorReply when there is none.
[[nodiscard]] Broker::Decoded<Broker::Response> roundTrip(HANDLE pipe, std::uint32_t id, const Broker::Request& request)
{
    std::string error;
    if (!BrokerPipe::sendFrame(pipe, Broker::encodeRequest(id, request), WAIT, {}, error))
    {
        ADD_FAILURE() << "send: " << error;
        return {.requestId = 0, .message = Broker::ErrorReply{.message = error}};
    }
    const auto frame = BrokerPipe::receiveFrame(pipe, WAIT, {});
    if (!frame.has_value())
    {
        ADD_FAILURE() << "no reply";
        return {.requestId = 0, .message = Broker::ErrorReply{.message = "no reply"}};
    }
    auto decoded = Broker::decodeResponse(*frame);
    if (!decoded.has_value())
    {
        ADD_FAILURE() << decoded.error();
        return {.requestId = 0, .message = Broker::ErrorReply{.message = decoded.error()}};
    }
    return std::move(*decoded);
}

TEST(BrokerHelperTest, ASessionServesTheParentAndEndsWhenItClosesThePipe)
{
    TestMocks::MockProcessActions actions;
    const BrokerPipeServer::RequestHandler handler = [&actions](const Broker::Request& request)
    {
        return BrokerServer::handleRequest(request, actions);
    };
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    auto session = std::async(std::launch::async,
                              [&arguments, &handler, self = selfStartedBy(GetCurrentProcess())]
                              { return BrokerHelper::run(arguments, self, handler, WAIT, WAIT, std::stop_token{}); });

    const UniqueHandle self = ownProcess();
    auto client = BrokerPipe::connectToServer(arguments.pipeName, GetCurrentProcessId(), self.get(), WAIT, {});
    ASSERT_TRUE(client.has_value()) << client.error();
    const auto hello = roundTrip(client->get(), 1, Broker::Hello{});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));
    EXPECT_TRUE(std::get<Broker::HelloReply>(hello.message).accepted);
    const ProcessTarget target{.pid = 4242, .startTimeTicks = 99};
    const auto kill = roundTrip(client->get(), 2, Broker::SignalRequest{.target = target, .signal = Broker::Signal::Kill});
    ASSERT_TRUE(std::holds_alternative<Broker::ActionResult>(kill.message));
    EXPECT_TRUE(std::get<Broker::ActionResult>(kill.message).success);
    EXPECT_EQ(actions.killCount(), 1);
    EXPECT_EQ(actions.lastTarget(), target);
    const auto environment = roundTrip(client->get(), 3, Broker::EnvironmentRequest{.target = target});
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(environment.message)) << "no reader, no read";

    client->reset();
    ASSERT_EQ(session.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(session.get(), BrokerHelper::Exit::Done);
}

TEST(BrokerHelperTest, ASessionForAParentOfAnotherStartTimeNeverMakesItsPipe)
{
    BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    ++arguments.parentStartTicks;
    const BrokerPipeServer::RequestHandler handler = [](const Broker::Request&) -> Broker::Response
    {
        return Broker::ErrorReply{};
    };
    EXPECT_EQ(BrokerHelper::run(arguments, selfStartedBy(GetCurrentProcess()), handler, WAIT, WAIT, {}), BrokerHelper::Exit::NoParent);
    const UniqueHandle self = ownProcess();
    EXPECT_FALSE(BrokerPipe::connectToServer(arguments.pipeName, GetCurrentProcessId(), self.get(), 100ms, {}).has_value());
}

TEST(BrokerHelperTest, ASessionWhoseParentExitsBeforeConnectingEnds)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.process() != nullptr);
    const ProcessTarget parent = child.target();
    const BrokerHelper::Arguments arguments{
        .pipeName = freshName(), .parentPid = static_cast<DWORD>(parent.pid), .parentStartTicks = parent.startTimeTicks};
    const BrokerPipeServer::RequestHandler handler = [](const Broker::Request&) -> Broker::Response
    {
        return Broker::ErrorReply{};
    };
    auto session = std::async(std::launch::async,
                              [&arguments, &handler, self = selfStartedBy(child.process())]
                              { return BrokerHelper::run(arguments, self, handler, WAIT, 60s, std::stop_token{}); });
    // Once the pipe is there, the parent goes.
    ASSERT_TRUE(pipeExists(arguments.pipeName));
    ASSERT_NE(TerminateProcess(child.process(), 1), FALSE);
    ASSERT_EQ(session.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(session.get(), BrokerHelper::Exit::NoClient);
}

TEST(BrokerHelperTest, ASessionRefusesAClientThatIsntTheParent)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.process() != nullptr);
    const ProcessTarget parent = child.target();
    const BrokerHelper::Arguments arguments{
        .pipeName = freshName(), .parentPid = static_cast<DWORD>(parent.pid), .parentStartTicks = parent.startTimeTicks};
    const BrokerPipeServer::RequestHandler handler = [](const Broker::Request&) -> Broker::Response
    {
        return Broker::ErrorReply{};
    };
    auto session = std::async(std::launch::async,
                              [&arguments, &handler, self = selfStartedBy(child.process())]
                              { return BrokerHelper::run(arguments, self, handler, WAIT, WAIT, std::stop_token{}); });
    // This process opens the pipe, which was made for its child: it is refused and cut off.
    const UniqueHandle self = ownProcess();
    const auto client = BrokerPipe::connectToServer(arguments.pipeName, GetCurrentProcessId(), self.get(), WAIT, {});
    ASSERT_EQ(session.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(session.get(), BrokerHelper::Exit::NoClient);
    if (client.has_value())
    {
        const auto frame = BrokerPipe::receiveFrame(client->get(), WAIT, {});
        ASSERT_FALSE(frame.has_value());
        EXPECT_NE(frame.error(), BrokerPipe::ReceiveEnd::TimedOut);
    }
}

// #2548 review, finding 5: a parent whose token user isn't one user account (a service, LocalSystem,
// a group) is refused as the parent it is, before any pipe is made.
TEST(BrokerHelperTest, AParentRunningAsAServiceOrSystemIsRefusedAsAParent)
{
    const UniqueHandle self = ownProcess();
    const BrokerPipeServer::RequestHandler handler = [](const Broker::Request&) -> Broker::Response
    {
        return Broker::ErrorReply{};
    };
    for (const wchar_t* text : {L"S-1-5-18", L"S-1-5-19", L"S-1-5-20", L"S-1-1-0", L"S-1-5-32-544"})
    {
        PSID sid = nullptr;
        ASSERT_NE(ConvertStringSidToSidW(text, &sid), FALSE);
        const auto* const first = static_cast<const std::byte*>(sid);
        const BrokerPipe::Sid bytes(
            first, first + GetLengthSid(sid)); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the SID's own length
        LocalFree(sid);
        const std::wstring name = freshName();
        EXPECT_EQ(BrokerHelper::serveParent(self.get(), bytes, bytes, name, handler, WAIT, WAIT, {}), BrokerHelper::Exit::NoParent);
        EXPECT_FALSE(BrokerPipe::connectToServer(name, GetCurrentProcessId(), self.get(), 100ms, {}).has_value()) << "no pipe was made";
    }
}

// #2548 review, finding 6: a session already being served ends when its parent dies, even though the
// pipe's client end stays open (a handle duplicated elsewhere, a pipe that outlives its opener).
TEST(BrokerHelperTest, AServedSessionEndsWhenTheParentDiesWithThePipeStillOpen)
{
    const SuspendedChild parent;
    ASSERT_TRUE(parent.process() != nullptr);
    const std::wstring name = freshName();
    const auto sid = BrokerPipe::userSidOfProcess(GetCurrentProcess());
    ASSERT_TRUE(sid.has_value()) << sid.error();
    auto server = BrokerPipe::createServerPipe(name, *sid);
    ASSERT_TRUE(server.has_value()) << server.error();
    const UniqueHandle self = ownProcess();
    auto accepted = std::async(std::launch::async,
                               [pipe = server->get(), process = self.get()] { return BrokerPipe::acceptClient(pipe, process, WAIT, {}); });
    auto client = BrokerPipe::connectToServer(name, GetCurrentProcessId(), self.get(), WAIT, {});
    ASSERT_TRUE(client.has_value()) << client.error();
    ASSERT_TRUE(accepted.get().has_value());

    const BrokerHelper::ProcessWatch watch(parent.process(), {});
    const BrokerPipeServer::RequestHandler handler = [](const Broker::Request&) -> Broker::Response
    {
        return Broker::ErrorReply{};
    };
    auto session =
        std::async(std::launch::async,
                   [pipe = server->get(), &handler, token = watch.token()] { return BrokerPipeServer::serve(pipe, handler, 60s, token); });
    const auto hello = roundTrip(client->get(), 1, Broker::Hello{});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));

    ASSERT_NE(TerminateProcess(parent.process(), 1), FALSE);
    ASSERT_EQ(session.wait_for(WAIT), std::future_status::ready) << "a minute's idle timeout: only the parent's exit ends it this soon";
    EXPECT_EQ(session.get(), BrokerPipeServer::SessionEnd::Stopped);
    EXPECT_TRUE(client->valid()) << "the client end was open throughout";
}

TEST(BrokerHelperTest, AStoppedSessionEnds)
{
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const BrokerPipeServer::RequestHandler handler = [](const Broker::Request&) -> Broker::Response
    {
        return Broker::ErrorReply{};
    };
    std::stop_source stop;
    auto session = std::async(std::launch::async,
                              [&arguments, &handler, token = stop.get_token(), self = selfStartedBy(GetCurrentProcess())]
                              { return BrokerHelper::run(arguments, self, handler, 60s, 60s, token); });
    stop.request_stop();
    ASSERT_EQ(session.wait_for(WAIT), std::future_status::ready);
    const BrokerHelper::Exit exit = session.get();
    EXPECT_TRUE(exit == BrokerHelper::Exit::NoClient || exit == BrokerHelper::Exit::Done) << static_cast<int>(exit);
}

// --- An over-the-shoulder elevation (#2550) ------------------------------------------------------------
// The helper running as another account than TaskSmack's user: an administrator typed their password
// for a standard user's TaskSmack. One action, a minute at most, and reads of the user's own processes.

/// The SID @p text names, as bytes.
[[nodiscard]] BrokerPipe::Sid sidFrom(const wchar_t* text)
{
    PSID sid = nullptr;
    if (ConvertStringSidToSidW(text, &sid) == FALSE)
    {
        ADD_FAILURE() << "not a SID";
        return {};
    }
    const auto* const first = static_cast<const std::byte*>(sid);
    BrokerPipe::Sid bytes(first,
                          first + GetLengthSid(sid)); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the SID's own length
    LocalFree(sid);
    return bytes;
}

[[nodiscard]] BrokerPipe::Sid ownSid()
{
    auto sid = BrokerPipe::userSidOfProcess(GetCurrentProcess());
    EXPECT_TRUE(sid.has_value()) << sid.error_or("");
    return sid.value_or(BrokerPipe::Sid{});
}

/// An administrator's account that isn't this test's user.
[[nodiscard]] BrokerPipe::Sid anotherAccount()
{
    return sidFrom(L"S-1-5-21-1111111111-2222222222-3333333333-1001");
}

[[nodiscard]] ProcessTarget ownTarget()
{
    return {.pid = static_cast<std::int32_t>(GetCurrentProcessId()), .startTimeTicks = startTicksOf(GetCurrentProcess())};
}

TEST(BrokerHelperTest, AnElevationOfTaskSmacksOwnUserIsTheSessionItWas)
{
    const BrokerPipe::Sid me = ownSid();
    const BrokerHelper::SessionPolicy policy = BrokerHelper::sessionPolicy(me, me, BrokerHelper::IDLE_TIMEOUT);
    EXPECT_EQ(policy.grant, BrokerPipeServer::Grant::Session);
    EXPECT_EQ(policy.idleTimeout, BrokerHelper::IDLE_TIMEOUT);
    EXPECT_FALSE(policy.readsOwnProcessesOnly);
}

TEST(BrokerHelperTest, AnOverTheShoulderElevationIsOneActionWithinAMinuteOnTheUsersOwnProcesses)
{
    const BrokerHelper::SessionPolicy policy = BrokerHelper::sessionPolicy(ownSid(), anotherAccount(), BrokerHelper::IDLE_TIMEOUT);
    EXPECT_EQ(policy.grant, BrokerPipeServer::Grant::OneAction);
    EXPECT_EQ(policy.idleTimeout, std::chrono::milliseconds{BrokerHelper::OVER_THE_SHOULDER_IDLE_TIMEOUT});
    EXPECT_EQ(BrokerHelper::OVER_THE_SHOULDER_IDLE_TIMEOUT, 1min);
    EXPECT_TRUE(policy.readsOwnProcessesOnly);
    EXPECT_EQ(BrokerHelper::sessionPolicy(ownSid(), anotherAccount(), 100ms).idleTimeout, 100ms) << "never longer than asked";
}

TEST(BrokerHelperTest, AProcessRunsAsTheUserItsTokenNames)
{
    EXPECT_TRUE(BrokerHelper::runsAs(ownTarget(), ownSid()));
    EXPECT_FALSE(BrokerHelper::runsAs(ownTarget(), anotherAccount()));
    const SuspendedChild child;
    ASSERT_TRUE(child.process() != nullptr);
    EXPECT_TRUE(BrokerHelper::runsAs(child.target(), ownSid())) << "a child of this user's";
}

TEST(BrokerHelperTest, AProcessWhoseUserCantBeToldIsNoOnesOwn)
{
    ProcessTarget other = ownTarget();
    ++other.startTimeTicks;
    EXPECT_FALSE(BrokerHelper::runsAs(other, ownSid())) << "another start time: not the process named";
    EXPECT_FALSE(BrokerHelper::runsAs({.pid = 0, .startTimeTicks = 1}, ownSid()));

    const SuspendedChild child;
    ASSERT_TRUE(child.process() != nullptr);
    const ProcessTarget gone = child.target();
    ASSERT_NE(TerminateProcess(child.process(), 1), FALSE);
    ASSERT_TRUE(child.exited());
    EXPECT_FALSE(BrokerHelper::runsAs(gone, ownSid())) << "an exited process";
}

/// Answers every read Ok and every action with success, counting what reaches it.
struct CountingReads
{
    std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);

    [[nodiscard]] BrokerPipeServer::RequestHandler handler() const
    {
        return [calls = calls](const Broker::Request& request) -> Broker::Response
        {
            ++*calls;
            if (std::holds_alternative<Broker::ModulesRequest>(request))
            {
                return Broker::ModulesReply{.result = {.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}}};
            }
            return Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}};
        };
    }
};

TEST(BrokerHelperTest, AnOverTheShoulderSessionReadsOnlyTheUsersOwnProcesses)
{
    const CountingReads counting;
    const ProcessTarget mine{.pid = 1, .startTimeTicks = 1};
    const ProcessTarget theirs{.pid = 2, .startTimeTicks = 2};
    const BrokerPipeServer::RequestHandler handler =
        BrokerHelper::readsOnlyOwnProcesses(counting.handler(), [&mine](const ProcessTarget& target) { return target == mine; });

    const auto own = handler(Broker::ModulesRequest{.target = mine});
    ASSERT_TRUE(std::holds_alternative<Broker::ModulesReply>(own));
    EXPECT_EQ(std::get<Broker::ModulesReply>(own).result.status, ModulesReadStatus::Ok);
    EXPECT_EQ(counting.calls->load(), 1);

    const auto modules = handler(Broker::ModulesRequest{.target = theirs});
    ASSERT_TRUE(std::holds_alternative<Broker::ModulesReply>(modules));
    EXPECT_EQ(std::get<Broker::ModulesReply>(modules).result.status, ModulesReadStatus::Failed);
    EXPECT_EQ(std::get<Broker::ModulesReply>(modules).result.detail, BrokerHelper::OTHER_USERS_READ_REFUSED);

    const auto files = handler(Broker::OpenFilesRequest{.target = theirs});
    ASSERT_TRUE(std::holds_alternative<Broker::OpenFilesReply>(files));
    EXPECT_EQ(std::get<Broker::OpenFilesReply>(files).result.status, OpenFilesReadStatus::Failed);
    EXPECT_EQ(std::get<Broker::OpenFilesReply>(files).result.detail, BrokerHelper::OTHER_USERS_READ_REFUSED);

    const auto connections = handler(Broker::ConnectionsRequest{.target = theirs});
    ASSERT_TRUE(std::holds_alternative<Broker::ConnectionsReply>(connections));
    EXPECT_EQ(std::get<Broker::ConnectionsReply>(connections).result.status, ConnectionsReadStatus::Failed);
    EXPECT_EQ(std::get<Broker::ConnectionsReply>(connections).result.detail, BrokerHelper::OTHER_USERS_READ_REFUSED);

    const auto environment = handler(Broker::EnvironmentRequest{.target = theirs});
    ASSERT_TRUE(std::holds_alternative<Broker::EnvironmentReply>(environment));
    EXPECT_EQ(std::get<Broker::EnvironmentReply>(environment).result.status, EnvironmentReadStatus::Failed);

    const auto rollup = handler(Broker::MemoryRollupRequest{.target = theirs});
    ASSERT_TRUE(std::holds_alternative<Broker::MemoryRollupReply>(rollup));
    EXPECT_FALSE(std::get<Broker::MemoryRollupReply>(rollup).result.has_value());

    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(handler(Broker::CrashesRequest{}))) << "every user's crashes";
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(handler(Broker::FirmwareFileRequest{})));
    EXPECT_EQ(counting.calls->load(), 1) << "no refused read reached the readers";

    // Actions aren't limited by owner: ending another account's process is what the administrator approved.
    const auto kill = handler(Broker::SignalRequest{.target = theirs, .signal = Broker::Signal::Kill});
    ASSERT_TRUE(std::holds_alternative<Broker::ActionResult>(kill));
    EXPECT_TRUE(std::get<Broker::ActionResult>(kill).success);
    EXPECT_EQ(counting.calls->load(), 2);
}

TEST(BrokerHelperTest, AnOverTheShoulderSessionEndsOnceItsOneActionIsAnswered)
{
    const UniqueHandle self = ownProcess();
    const std::wstring name = freshName();
    const CountingReads counting;
    const BrokerPipe::Sid me = ownSid();
    const BrokerPipe::Sid administrator = anotherAccount();
    auto session = std::async(std::launch::async,
                              [&self, &name, &me, &administrator, handler = counting.handler()]
                              { return BrokerHelper::serveParent(self.get(), me, administrator, name, handler, WAIT, WAIT, {}); });
    auto client = BrokerPipe::connectToServer(name, GetCurrentProcessId(), self.get(), WAIT, {});
    ASSERT_TRUE(client.has_value()) << client.error();
    const auto hello = roundTrip(client->get(), 1, Broker::Hello{});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));

    // This process is this user's: read as asked. Another process isn't, and its read is refused.
    const auto own = roundTrip(client->get(), 2, Broker::ModulesRequest{.target = ownTarget()});
    ASSERT_TRUE(std::holds_alternative<Broker::ModulesReply>(own.message));
    EXPECT_EQ(std::get<Broker::ModulesReply>(own.message).result.status, ModulesReadStatus::Ok);
    const ProcessTarget absent{.pid = 4242, .startTimeTicks = 99};
    const auto refused = roundTrip(client->get(), 3, Broker::ModulesRequest{.target = absent});
    ASSERT_TRUE(std::holds_alternative<Broker::ModulesReply>(refused.message));
    EXPECT_EQ(std::get<Broker::ModulesReply>(refused.message).result.detail, BrokerHelper::OTHER_USERS_READ_REFUSED);
    EXPECT_EQ(counting.calls->load(), 1);

    // The kill is the one action: answered, then the session ends.
    const auto kill = roundTrip(client->get(), 4, Broker::SignalRequest{.target = absent, .signal = Broker::Signal::Kill});
    ASSERT_TRUE(std::holds_alternative<Broker::ActionResult>(kill.message));
    EXPECT_TRUE(std::get<Broker::ActionResult>(kill.message).success);
    EXPECT_EQ(counting.calls->load(), 2);

    ASSERT_EQ(session.wait_for(WAIT), std::future_status::ready) << "a session for one action outlived it";
    EXPECT_EQ(session.get(), BrokerHelper::Exit::Done);
    const auto after = BrokerPipe::receiveFrame(client->get(), WAIT, {});
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error(), BrokerPipe::ReceiveEnd::Closed) << "the helper closed the pipe";
}
// --- The process mitigations HelperMain turns on (#2557) --------------------------------------------
// Never in this process: they can't be turned off again. A stand-in for SetProcessMitigationPolicy
// records what each would have been set to.

/// What applyMitigations() asked for, by policy, as the bytes it passed.
struct MitigationRecorder
{
    std::map<PROCESS_MITIGATION_POLICY, std::vector<std::byte>> set;
    std::vector<PROCESS_MITIGATION_POLICY> refuse;

    [[nodiscard]] BrokerHelper::SetMitigationPolicy setter()
    {
        return [this](PROCESS_MITIGATION_POLICY policy, void* value, std::size_t size)
        {
            const std::span<const std::byte> bytes(static_cast<const std::byte*>(value), size);
            set[policy].assign(bytes.begin(), bytes.end());
            return std::ranges::find(refuse, policy) == refuse.end();
        };
    }

    /// The value asked for @p policy, as @p T; all zero when it wasn't asked for, or not with T's size.
    /// A plain value keeps clang-tidy's optional dataflow satisfied: bugprone-unchecked-optional-access
    /// doesn't model gtest's ASSERT_TRUE early return.
    template<typename T> [[nodiscard]] T value(PROCESS_MITIGATION_POLICY policy) const
    {
        T result{};
        const auto found = set.find(policy);
        if (found != set.end() && found->second.size() == sizeof(T))
        {
            std::memcpy(&result, found->second.data(), sizeof(T));
        }
        return result;
    }
};

// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - each policy is a union of its bitfields and Flags
TEST(BrokerHelperTest, TheHelpersMitigationsAreTurnedOn)
{
    MitigationRecorder recorder;
    EXPECT_TRUE(BrokerHelper::applyMitigations(recorder.setter()).empty());
    EXPECT_EQ(recorder.set.size(), 6U);

    const auto imageLoad = recorder.value<PROCESS_MITIGATION_IMAGE_LOAD_POLICY>(ProcessImageLoadPolicy);
    EXPECT_EQ(imageLoad.NoRemoteImages, 1U);
    EXPECT_EQ(imageLoad.NoLowMandatoryLabelImages, 1U);
    EXPECT_EQ(imageLoad.PreferSystem32Images, 1U);

    const auto extensionPoints = recorder.value<PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY>(ProcessExtensionPointDisablePolicy);
    EXPECT_EQ(extensionPoints.DisableExtensionPoints, 1U);

    const auto strictHandles = recorder.value<PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY>(ProcessStrictHandleCheckPolicy);
    EXPECT_EQ(strictHandles.RaiseExceptionOnInvalidHandleReference, 1U);
    EXPECT_EQ(strictHandles.HandleExceptionsPermanentlyEnabled, 1U);

    const auto dynamicCode = recorder.value<PROCESS_MITIGATION_DYNAMIC_CODE_POLICY>(ProcessDynamicCodePolicy);
    EXPECT_EQ(dynamicCode.ProhibitDynamicCode, 1U);
    EXPECT_EQ(dynamicCode.AllowThreadOptOut, 0U);
    EXPECT_EQ(dynamicCode.AllowRemoteDowngrade, 0U);

    const auto fonts = recorder.value<PROCESS_MITIGATION_FONT_DISABLE_POLICY>(ProcessFontDisablePolicy);
    EXPECT_EQ(fonts.DisableNonSystemFonts, 1U);

    const auto redirectionTrust = recorder.value<PROCESS_MITIGATION_REDIRECTION_TRUST_POLICY>(ProcessRedirectionTrustPolicy);
    EXPECT_EQ(redirectionTrust.EnforceRedirectionTrust, 1U);
    EXPECT_EQ(redirectionTrust.AuditRedirectionTrust, 0U);
}
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

TEST(BrokerHelperTest, AMitigationWindowsRefusesIsNamedAndTheRestAreStillTurnedOn)
{
    // An older Windows, which knows neither policy.
    MitigationRecorder recorder;
    recorder.refuse = {ProcessImageLoadPolicy, ProcessRedirectionTrustPolicy};
    const std::vector<std::string_view> refused = BrokerHelper::applyMitigations(recorder.setter());
    EXPECT_EQ(refused, (std::vector<std::string_view>{"image load", "redirection trust"}));
    EXPECT_EQ(recorder.set.size(), 6U) << "each is asked for on its own";
}

// --- TaskSmackHelper.exe, run directly as this user -------------------------------------------------

#if defined(TASKSMACK_WINDOWS_HELPER_PATH) && defined(TASKSMACK_WINDOWS_HELPER_TEST_PARENT_PATH)

/// A folder of its own under %TEMP%, laid out as an install is: a copy of TaskSmackHelper.exe, and a copy
/// of TaskSmackHelperTestParent.exe (HelperTestParent.cpp) as the TaskSmack.exe beside it. Removed on
/// scope exit.
class InstallFolder
{
  public:
    InstallFolder()
    {
        const std::wstring name = freshName();
        m_Path = std::filesystem::temp_directory_path() / (L"TaskSmackHelperTest." + name.substr(name.rfind(L'.') + 1));
        std::error_code error;
        std::filesystem::create_directories(m_Path, error);
        EXPECT_FALSE(error) << error.message();
        std::filesystem::copy_file(TASKSMACK_WINDOWS_HELPER_PATH, helper(), error);
        EXPECT_FALSE(error) << error.message();
        std::filesystem::copy_file(TASKSMACK_WINDOWS_HELPER_TEST_PARENT_PATH, taskSmack(), error);
        EXPECT_FALSE(error) << error.message();
    }
    ~InstallFolder()
    {
        // What ran from here has exited (a test's processes are declared after this), but Windows may
        // hold an image a moment longer.
        try
        {
            constexpr int ATTEMPTS = 50;
            std::error_code error;
            for (int attempt = 0; attempt < ATTEMPTS && std::filesystem::exists(m_Path, error); ++attempt)
            {
                std::filesystem::remove_all(m_Path, error);
                if (error)
                {
                    std::this_thread::sleep_for(20ms);
                }
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch) - out of memory: the folder is left in %TEMP%
        {}
    }
    InstallFolder(const InstallFolder&) = delete;
    InstallFolder& operator=(const InstallFolder&) = delete;
    InstallFolder(InstallFolder&&) = delete;
    InstallFolder& operator=(InstallFolder&&) = delete;

    [[nodiscard]] std::filesystem::path helper() const
    {
        return m_Path / L"TaskSmackHelper.exe";
    }
    [[nodiscard]] std::filesystem::path taskSmack() const
    {
        return m_Path / L"TaskSmack.exe";
    }
    /// Why the TaskSmack.exe run from here failed, in its words.
    [[nodiscard]] std::string log() const
    {
        std::ifstream in(m_Path / L"HelperTestParent.log");
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

  private:
    std::filesystem::path m_Path;
};

/// @p exe started with @p parameters, as this user, never elevated: suspended if asked, and as
/// @p parent's child if one is given (PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, which is how AppInfo makes
/// a "runas" process its caller's child). Terminated, if still running, and closed on scope exit.
struct StartOptions
{
    HANDLE parent = nullptr; ///< the process to make the parent, or none: this one
    bool suspended = false;
};

class Started
{
  public:
    Started(const std::filesystem::path& exe, const std::wstring& parameters, StartOptions options = {})
    {
        std::wstring commandLine = L"\"" + exe.wstring() + L"\" " + parameters;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        DWORD flags = CREATE_NO_WINDOW | (options.suspended ? CREATE_SUSPENDED : 0U);
        std::vector<std::byte> attributes;
        if (options.parent != nullptr)
        {
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            attributes.resize(size);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - the opaque list lives in a byte buffer of the size asked for
            startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) == FALSE)
            {
                ADD_FAILURE() << "couldn't make an attribute list: " << GetLastError();
                return;
            }
            if (UpdateProcThreadAttribute(startup.lpAttributeList,
                                          0,
                                          PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                                          static_cast<PVOID>(&options.parent),
                                          sizeof(options.parent),
                                          nullptr,
                                          nullptr) == FALSE)
            {
                ADD_FAILURE() << "couldn't name the parent: " << GetLastError();
                DeleteProcThreadAttributeList(startup.lpAttributeList);
                return;
            }
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        }
        PROCESS_INFORMATION info{};
        const BOOL created =
            CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup.StartupInfo, &info);
        const DWORD error = GetLastError();
        if (startup.lpAttributeList != nullptr)
        {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        }
        if (created == FALSE)
        {
            ADD_FAILURE() << "couldn't start " << exe.string() << ": " << error;
            return;
        }
        m_Process.reset(info.hProcess);
        m_Thread.reset(info.hThread);
    }
    ~Started()
    {
        if (m_Process && WaitForSingleObject(m_Process.get(), 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(m_Process.get(), 1);
            WaitForSingleObject(m_Process.get(), static_cast<DWORD>(WAIT.count()));
        }
    }
    Started(const Started&) = delete;
    Started& operator=(const Started&) = delete;
    Started(Started&&) = delete;
    Started& operator=(Started&&) = delete;

    [[nodiscard]] HANDLE process() const noexcept
    {
        return m_Process.get();
    }

    /// The arguments that name it as the helper's parent, with a fresh pipe name.
    [[nodiscard]] BrokerHelper::Arguments asParent() const
    {
        return {.pipeName = freshName(), .parentPid = GetProcessId(m_Process.get()), .parentStartTicks = startTicksOf(m_Process.get())};
    }

    /// Its exit status once it has exited within @p timeout; nullopt if it is still running. A freshly
    /// copied image may be scanned before it runs, so the default is the startup bound.
    [[nodiscard]] std::optional<DWORD> exitCode(std::chrono::milliseconds timeout = STARTUP_WAIT) const
    {
        if (!m_Process || WaitForSingleObject(m_Process.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
        {
            return std::nullopt;
        }
        DWORD code = 0;
        EXPECT_NE(GetExitCodeProcess(m_Process.get(), &code), FALSE);
        return code;
    }

  private:
    UniqueHandle m_Process;
    UniqueHandle m_Thread;
};

[[nodiscard]] std::optional<DWORD> exitStatus(BrokerHelper::Exit exit)
{
    return static_cast<DWORD>(exit);
}

// #2572 review, finding 1, the whole way: a program copies the signed helper into a folder of its own,
// saves itself beside it as TaskSmack.exe, starts it as TaskSmack would (HelperLauncher, CreateProcessW
// standing in for "runas"), and asks it to end a process. Every other check passes; the install's folder
// is the program's own, so the helper refuses it, and the process lives.
TEST(TaskSmackHelperTest, RefusesACopiedHelperBesideAProgramCalledTaskSmack)
{
    const InstallFolder installed;
    const SuspendedChild victim;
    ASSERT_TRUE(victim.process() != nullptr);
    const ProcessTarget target = victim.target();
    const Started taskSmack(installed.taskSmack(), std::format(L"kill {} {}", target.pid, target.startTimeTicks));
    ASSERT_TRUE(taskSmack.process() != nullptr);
    EXPECT_EQ(taskSmack.exitCode(2 * STARTUP_WAIT), std::optional<DWORD>(1)) << "it got a broker";
    EXPECT_TRUE(installed.log().contains(BrokerHelper::exitMeaning(static_cast<DWORD>(BrokerHelper::Exit::NoParent)))) << installed.log();
    EXPECT_FALSE(victim.exited(0ms));
}

TEST(TaskSmackHelperTest, RefusesACommandLineTaskSmackDoesntMake)
{
    const Started none(TASKSMACK_WINDOWS_HELPER_PATH, L"");
    EXPECT_EQ(none.exitCode(), exitStatus(BrokerHelper::Exit::Usage));
    const Started otherPipe(TASKSMACK_WINDOWS_HELPER_PATH, std::format(L"--broker \\\\.\\pipe\\Other {} 1", GetCurrentProcessId()));
    EXPECT_EQ(otherPipe.exitCode(), exitStatus(BrokerHelper::Exit::Usage));
}

TEST(TaskSmackHelperTest, RefusesAParentOfAnotherStartTime)
{
    BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    ++arguments.parentStartTicks;
    const Started helper(TASKSMACK_WINDOWS_HELPER_PATH, BrokerHelper::commandLine(arguments));
    EXPECT_EQ(helper.exitCode(), exitStatus(BrokerHelper::Exit::NoParent));
}

// #2548 review, finding 3: a program that starts the signed helper through its UAC prompt itself gets no
// broker, whether it names itself or a real TaskSmack.

TEST(TaskSmackHelperTest, RefusesAProgramThatStartsItAndNamesItself)
{
    // This test is that program: it is the helper's parent, but not the TaskSmack.exe beside it.
    const InstallFolder installed;
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const Started helper(installed.helper(), BrokerHelper::commandLine(arguments));
    EXPECT_EQ(helper.exitCode(), exitStatus(BrokerHelper::Exit::NoParent));
}

TEST(TaskSmackHelperTest, RefusesAProgramThatStartsItAndNamesATaskSmack)
{
    // The TaskSmack named is the one beside the helper, but this test, not it, started the helper.
    const InstallFolder installed;
    const Started taskSmack(installed.taskSmack(), L"", {.suspended = true});
    ASSERT_TRUE(taskSmack.process() != nullptr);
    const Started helper(installed.helper(), BrokerHelper::commandLine(taskSmack.asParent()));
    EXPECT_EQ(helper.exitCode(), exitStatus(BrokerHelper::Exit::NoParent));
}

TEST(TaskSmackHelperTest, RefusesAnotherProgramAsItsParent)
{
    // Made cmd.exe's child, as "runas" from cmd.exe would make it, and naming it.
    const InstallFolder installed;
    const SuspendedChild cmd;
    ASSERT_TRUE(cmd.process() != nullptr);
    const ProcessTarget named = cmd.target();
    const BrokerHelper::Arguments arguments{
        .pipeName = freshName(), .parentPid = static_cast<DWORD>(named.pid), .parentStartTicks = named.startTimeTicks};
    const Started helper(installed.helper(), BrokerHelper::commandLine(arguments), {.parent = cmd.process()});
    EXPECT_EQ(helper.exitCode(), exitStatus(BrokerHelper::Exit::NoParent));
}

TEST(TaskSmackHelperTest, RefusesATaskSmackFromAnotherFolder)
{
    // Named TaskSmack.exe, and its parent, but not the one beside this helper (the build's).
    const InstallFolder installed;
    const Started taskSmack(installed.taskSmack(), L"", {.suspended = true});
    ASSERT_TRUE(taskSmack.process() != nullptr);
    const Started helper(TASKSMACK_WINDOWS_HELPER_PATH, BrokerHelper::commandLine(taskSmack.asParent()), {.parent = taskSmack.process()});
    EXPECT_EQ(helper.exitCode(), exitStatus(BrokerHelper::Exit::NoParent));
}

TEST(TaskSmackHelperTest, RefusesACopiedHelperMadeTheChildOfAProgramCalledTaskSmack)
{
    // The same, with the helper made the child of the TaskSmack.exe beside it the way AppInfo makes a
    // "runas" process its caller's (PROC_THREAD_ATTRIBUTE_PARENT_PROCESS): its creator, start time and
    // name all pass, and only the folder, this user's, gives it away. No pipe is ever made.
    const InstallFolder installed;
    const Started taskSmack(installed.taskSmack(), L"", {.suspended = true});
    ASSERT_TRUE(taskSmack.process() != nullptr);
    const BrokerHelper::Arguments arguments = taskSmack.asParent();
    const Started helper(installed.helper(), BrokerHelper::commandLine(arguments), {.parent = taskSmack.process()});
    ASSERT_TRUE(helper.process() != nullptr);
    EXPECT_EQ(helper.exitCode(), exitStatus(BrokerHelper::Exit::NoParent));
    EXPECT_EQ(WaitNamedPipeW(arguments.pipeName.c_str(), 0), FALSE);
}

// --- TaskSmackHelper.exe's image (#2557) ------------------------------------------------------------

/// A PE file's bytes, read in bounds only. Plain values, all zero for anything that isn't there, keep
/// clang-tidy's optional dataflow satisfied: bugprone-unchecked-optional-access doesn't model gtest's
/// ASSERT_TRUE early return.
class PeFile
{
  public:
    /// Where a data directory is in the file; size 0 when the image has none.
    struct Directory
    {
        std::size_t offset = 0;
        DWORD size = 0;
    };

    explicit PeFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        m_Bytes.resize(chars.size());
        std::memcpy(m_Bytes.data(), chars.data(), chars.size());

        const auto dos = at<IMAGE_DOS_HEADER>(0);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        {
            return;
        }
        const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
        const auto nt = at<IMAGE_NT_HEADERS64>(ntOffset);
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            return;
        }
        m_Nt = nt;
        m_Valid = true;
        const std::size_t sections = ntOffset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        for (std::size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i)
        {
            m_Sections.push_back(at<IMAGE_SECTION_HEADER>(sections + (i * sizeof(IMAGE_SECTION_HEADER))));
        }
    }

    /// Whether it is a 64-bit PE file.
    [[nodiscard]] bool valid() const noexcept
    {
        return m_Valid;
    }

    /// Its 64-bit NT headers; all zero when it isn't a 64-bit PE file.
    [[nodiscard]] const IMAGE_NT_HEADERS64& headers() const noexcept
    {
        return m_Nt;
    }

    /// The @p T at file offset @p offset; all zero when it doesn't fit.
    template<typename T> [[nodiscard]] T at(std::size_t offset) const
    {
        T value{};
        if (offset <= m_Bytes.size() && m_Bytes.size() - offset >= sizeof(T))
        {
            std::memcpy(&value, std::next(m_Bytes.data(), static_cast<std::ptrdiff_t>(offset)), sizeof(T));
        }
        return value;
    }

    /// The data directory @p index; size 0 when it has none.
    [[nodiscard]] Directory directory(std::size_t index) const
    {
        if (!m_Valid || index >= m_Nt.OptionalHeader.NumberOfRvaAndSizes)
        {
            return {};
        }
        const IMAGE_DATA_DIRECTORY entry = std::span(m_Nt.OptionalHeader.DataDirectory)[index];
        const auto offset = entry.VirtualAddress == 0 ? std::nullopt : offsetOf(entry.VirtualAddress);
        return offset.has_value() ? Directory{.offset = *offset, .size = entry.Size} : Directory{};
    }

    /// The NUL-terminated name at relative virtual address @p rva, lower-cased; empty when unreadable.
    [[nodiscard]] std::string nameAt(DWORD rva) const
    {
        std::string name;
        const auto start = offsetOf(rva);
        if (!start.has_value())
        {
            return name;
        }
        for (std::size_t offset = *start; offset < m_Bytes.size(); ++offset)
        {
            const char c = at<char>(offset);
            if (c == '\0')
            {
                break;
            }
            name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return name;
    }

  private:
    /// The file offset of relative virtual address @p rva; empty when no section holds it.
    [[nodiscard]] std::optional<std::size_t> offsetOf(DWORD rva) const
    {
        for (const IMAGE_SECTION_HEADER& section : m_Sections)
        {
            if (rva >= section.VirtualAddress && rva - section.VirtualAddress < section.SizeOfRawData)
            {
                return std::size_t{section.PointerToRawData} + (rva - section.VirtualAddress);
            }
        }
        return std::nullopt;
    }

    std::vector<std::byte> m_Bytes;
    IMAGE_NT_HEADERS64 m_Nt{};
    bool m_Valid = false;
    std::vector<IMAGE_SECTION_HEADER> m_Sections;
};

TEST(TaskSmackHelperImageTest, IsBuiltWithControlFlowGuardAndCetShadowStacks)
{
#ifndef TASKSMACK_WINDOWS_HELPER_HARDENED
    GTEST_SKIP() << "built without CFG and CETCOMPAT (a sanitizer build; CMakeLists.txt)";
#else
    const PeFile image(TASKSMACK_WINDOWS_HELPER_PATH);
    ASSERT_TRUE(image.valid()) << "not a 64-bit PE file";
    const WORD characteristics = image.headers().OptionalHeader.DllCharacteristics;
    EXPECT_NE(characteristics & IMAGE_DLLCHARACTERISTICS_GUARD_CF, 0) << "no /guard:cf";
    EXPECT_NE(characteristics & IMAGE_DLLCHARACTERISTICS_NX_COMPAT, 0);
    EXPECT_NE(characteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, 0);
    EXPECT_NE(characteristics & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA, 0);

    // The load configuration's guard flags: the compiler's checks went in (-cfguard), and the linker's
    // table of valid call targets with them.
    const PeFile::Directory loadConfig = image.directory(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);
    ASSERT_GE(loadConfig.size, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags) + sizeof(DWORD)) << "no load configuration";
    const auto guardFlags = image.at<DWORD>(loadConfig.offset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags));
    EXPECT_NE(guardFlags & IMAGE_GUARD_CF_INSTRUMENTED, 0U) << "no CFG checks";
    EXPECT_NE(guardFlags & IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT, 0U) << "no CFG target table";

    // CET shadow-stack compatibility is an extended DLL characteristic, in a debug directory entry.
    const PeFile::Directory debug = image.directory(IMAGE_DIRECTORY_ENTRY_DEBUG);
    ASSERT_NE(debug.size, 0U) << "no debug directory";
    bool cetCompatible = false;
    for (std::size_t i = 0; i < debug.size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i)
    {
        const auto entry = image.at<IMAGE_DEBUG_DIRECTORY>(debug.offset + (i * sizeof(IMAGE_DEBUG_DIRECTORY)));
        if (entry.Type == IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS)
        {
            cetCompatible = (image.at<DWORD>(entry.PointerToRawData) & IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT) != 0;
        }
    }
    EXPECT_TRUE(cetCompatible) << "no /CETCOMPAT";
#endif
}

TEST(TaskSmackHelperImageTest, ImportsNeitherComNorTheDumpWriter)
{
    // The folder picker (COM's IFileOpenDialog) and the dump writer aren't linked in (#2557).
    const PeFile image(TASKSMACK_WINDOWS_HELPER_PATH);
    ASSERT_TRUE(image.valid()) << "not a 64-bit PE file";
    const PeFile::Directory imports = image.directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    ASSERT_NE(imports.size, 0U) << "no imports";
    std::vector<std::string> names;
    for (std::size_t offset = imports.offset; offset < imports.offset + imports.size; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR))
    {
        const auto descriptor = image.at<IMAGE_IMPORT_DESCRIPTOR>(offset);
        if (descriptor.Name == 0)
        {
            break;
        }
        names.push_back(image.nameAt(descriptor.Name));
    }
    EXPECT_TRUE(std::ranges::contains(names, "kernel32.dll")) << "the import table wasn't read";
    for (const std::string_view unwanted : {"ole32.dll", "oleaut32.dll", "combase.dll", "dbghelp.dll", "dxgi.dll", "setupapi.dll"})
    {
        EXPECT_FALSE(std::ranges::contains(names, unwanted)) << unwanted;
    }
}

/// String @p name of the version resource of @p file, in US English and Unicode (as the .rc gives it).
[[nodiscard]] std::wstring versionString(const std::filesystem::path& file, std::wstring_view name)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0)
    {
        return {};
    }
    std::vector<std::byte> block(size);
    if (GetFileVersionInfoW(file.c_str(), 0, size, block.data()) == FALSE)
    {
        return {};
    }
    const std::wstring query = std::format(L"\\StringFileInfo\\040904B0\\{}", name);
    void* value = nullptr;
    UINT length = 0;
    if (VerQueryValueW(block.data(), query.c_str(), &value, &length) == FALSE || value == nullptr || length == 0)
    {
        return {};
    }
    return {static_cast<const wchar_t*>(value)}; // NUL-terminated within the block
}

TEST(TaskSmackHelperImageTest, NamesItselfInItsVersionResource)
{
    // What the UAC prompt and Explorer show for it.
    const std::filesystem::path helper(TASKSMACK_WINDOWS_HELPER_PATH);
    EXPECT_EQ(versionString(helper, L"FileDescription"), L"TaskSmack administrator helper");
    EXPECT_EQ(versionString(helper, L"ProductName"), L"TaskSmack");
    EXPECT_EQ(versionString(helper, L"CompanyName"), L"TaskSmack");
    EXPECT_EQ(versionString(helper, L"OriginalFilename"), L"TaskSmackHelper.exe");
    EXPECT_FALSE(versionString(helper, L"FileVersion").empty());
}

#endif // TASKSMACK_WINDOWS_HELPER_PATH && TASKSMACK_WINDOWS_HELPER_TEST_PARENT_PATH

} // namespace
} // namespace Platform::Windows
