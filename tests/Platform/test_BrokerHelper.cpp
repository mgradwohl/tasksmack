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
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/BrokerPipeServer.h"
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
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
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

/// What an in-process helper "started" by @p parent knows of itself: @p parent's image is its
/// TaskSmack, @p parent created it, and it started now. run() then takes @p parent as TaskSmack.
[[nodiscard]] BrokerHelper::Self selfStartedBy(HANDLE parent)
{
    return {.taskSmackPath = imageOf(parent), .creatorPid = GetProcessId(parent), .startTicks = nowTicks()};
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
        EXPECT_EQ(BrokerHelper::serveParent(self.get(), bytes, name, handler, WAIT, WAIT, {}), BrokerHelper::Exit::NoParent);
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

// The whole way, as TaskSmack will go: TaskSmack.exe starts the helper beside it through HelperLauncher
// (CreateProcessW standing in for "runas"), connects a PipeBroker, and has the helper end a process.
TEST(TaskSmackHelperTest, ServesTheTaskSmackBesideItThatStartedIt)
{
    const InstallFolder installed;
    const SuspendedChild victim;
    ASSERT_TRUE(victim.process() != nullptr);
    const ProcessTarget target = victim.target();
    const Started taskSmack(installed.taskSmack(), std::format(L"kill {} {}", target.pid, target.startTimeTicks));
    ASSERT_TRUE(taskSmack.process() != nullptr);
    EXPECT_EQ(taskSmack.exitCode(2 * STARTUP_WAIT), std::optional<DWORD>(0)) << installed.log();
    EXPECT_TRUE(victim.exited());
}

TEST(TaskSmackHelperTest, AnswersGarbageWithAnErrorAndEnds)
{
    const InstallFolder installed;
    const Started taskSmack(installed.taskSmack(), L"garbage");
    ASSERT_TRUE(taskSmack.process() != nullptr);
    EXPECT_EQ(taskSmack.exitCode(2 * STARTUP_WAIT), std::optional<DWORD>(0)) << installed.log();
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

TEST(TaskSmackHelperTest, ExitsWhenItsParentDoes)
{
    // Made the child of the TaskSmack beside it, as AppInfo makes a "runas" process its caller's: the
    // checks pass and it makes its pipe. That TaskSmack (suspended, it never opens the pipe) then exits.
    const InstallFolder installed;
    const Started taskSmack(installed.taskSmack(), L"", {.suspended = true});
    ASSERT_TRUE(taskSmack.process() != nullptr);
    const BrokerHelper::Arguments arguments = taskSmack.asParent();
    const Started helper(installed.helper(), BrokerHelper::commandLine(arguments), {.parent = taskSmack.process()});
    ASSERT_TRUE(helper.process() != nullptr);
    ASSERT_TRUE(pipeExists(arguments.pipeName)) << "it took the TaskSmack beside it, its parent, as its parent";
    ASSERT_NE(TerminateProcess(taskSmack.process(), 1), FALSE);
    EXPECT_EQ(helper.exitCode(), exitStatus(BrokerHelper::Exit::NoClient)) << "well within its 30 s accept timeout";
}

#endif // TASKSMACK_WINDOWS_HELPER_PATH && TASKSMACK_WINDOWS_HELPER_TEST_PARENT_PATH

} // namespace
} // namespace Platform::Windows
