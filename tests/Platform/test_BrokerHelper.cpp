// Tests for TaskSmackHelper.exe (#2517): its command line, the parent checks (the parent's handle, start
// time and user), the watch that ends a session when TaskSmack exits, the whole session run in this
// process, and the executable itself, started directly as this user, never elevated: no UAC prompt is
// ever raised. The processes the tests act on are their own suspended children, which never run.
// Every wait is bounded.

#include "Mocks/MockProbes.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerRequestMapping.h"
#include "Platform/IProcessActions.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace Platform::Windows
{
namespace
{

using namespace std::chrono_literals;

/// The bound on anything that should happen at once: a test that waits this long has failed.
constexpr std::chrono::milliseconds WAIT{10'000};

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

[[nodiscard]] std::chrono::milliseconds since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

/// Waits up to WAIT for pipe @p name to exist: the helper has opened its parent by then.
[[nodiscard]] bool pipeExists(const std::wstring& name)
{
    const auto deadline = std::chrono::steady_clock::now() + WAIT;
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
                              [&arguments, &handler] { return BrokerHelper::run(arguments, handler, WAIT, WAIT, std::stop_token{}); });

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
    EXPECT_EQ(BrokerHelper::run(arguments, handler, WAIT, WAIT, {}), BrokerHelper::Exit::NoParent);
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
                              [&arguments, &handler] { return BrokerHelper::run(arguments, handler, WAIT, 60s, std::stop_token{}); });
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
                              [&arguments, &handler] { return BrokerHelper::run(arguments, handler, WAIT, WAIT, std::stop_token{}); });
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

TEST(BrokerHelperTest, AStoppedSessionEnds)
{
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const BrokerPipeServer::RequestHandler handler = [](const Broker::Request&) -> Broker::Response
    {
        return Broker::ErrorReply{};
    };
    std::stop_source stop;
    auto session =
        std::async(std::launch::async,
                   [&arguments, &handler, token = stop.get_token()] { return BrokerHelper::run(arguments, handler, 60s, 60s, token); });
    stop.request_stop();
    ASSERT_EQ(session.wait_for(WAIT), std::future_status::ready);
    const BrokerHelper::Exit exit = session.get();
    EXPECT_TRUE(exit == BrokerHelper::Exit::NoClient || exit == BrokerHelper::Exit::Done) << static_cast<int>(exit);
}

// --- TaskSmackHelper.exe, run directly as this user -------------------------------------------------

#ifdef TASKSMACK_WINDOWS_HELPER_PATH

/// TaskSmackHelper.exe started with @p parameters, not elevated; terminated (if still running) and
/// closed on scope exit.
class HelperProcess
{
  public:
    explicit HelperProcess(const std::wstring& parameters)
    {
        const std::filesystem::path exe(TASKSMACK_WINDOWS_HELPER_PATH);
        std::wstring commandLine = L"\"" + exe.wstring() + L"\" " + parameters;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) ==
            FALSE)
        {
            ADD_FAILURE() << "couldn't start TaskSmackHelper.exe: " << GetLastError();
            return;
        }
        m_Process.reset(info.hProcess);
        CloseHandle(info.hThread);
    }
    ~HelperProcess()
    {
        if (m_Process && WaitForSingleObject(m_Process.get(), 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(m_Process.get(), 1);
            WaitForSingleObject(m_Process.get(), static_cast<DWORD>(WAIT.count()));
        }
    }
    HelperProcess(const HelperProcess&) = delete;
    HelperProcess& operator=(const HelperProcess&) = delete;
    HelperProcess(HelperProcess&&) = delete;
    HelperProcess& operator=(HelperProcess&&) = delete;

    [[nodiscard]] HANDLE process() const noexcept
    {
        return m_Process.get();
    }

    /// Its exit status once it has exited within WAIT; nullopt if it is still running.
    [[nodiscard]] std::optional<DWORD> exitCode() const
    {
        if (!m_Process || WaitForSingleObject(m_Process.get(), static_cast<DWORD>(WAIT.count())) != WAIT_OBJECT_0)
        {
            return std::nullopt;
        }
        DWORD code = 0;
        EXPECT_NE(GetExitCodeProcess(m_Process.get(), &code), FALSE);
        return code;
    }

  private:
    UniqueHandle m_Process;
};

[[nodiscard]] UniqueHandle connectTo(const HelperProcess& helper, const std::wstring& pipeName)
{
    auto pipe = BrokerPipe::connectToServer(pipeName, GetProcessId(helper.process()), helper.process(), WAIT, {});
    if (!pipe.has_value())
    {
        ADD_FAILURE() << pipe.error();
        return {};
    }
    return std::move(*pipe);
}

TEST(TaskSmackHelperTest, KillsAProcessForItsParentAndExitsWhenThePipeCloses)
{
    const SuspendedChild child;
    ASSERT_TRUE(child.process() != nullptr);
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const HelperProcess helper(BrokerHelper::commandLine(arguments));
    ASSERT_TRUE(helper.process() != nullptr);
    UniqueHandle pipe = connectTo(helper, arguments.pipeName);
    ASSERT_TRUE(pipe);

    const auto hello = roundTrip(pipe.get(), 1, Broker::Hello{});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));
    EXPECT_TRUE(std::get<Broker::HelloReply>(hello.message).accepted);

    // The helper re-checks the target's start time: one off is refused, and the process is untouched.
    ProcessTarget wrong = child.target();
    ++wrong.startTimeTicks;
    const auto refused = roundTrip(pipe.get(), 2, Broker::SignalRequest{.target = wrong, .signal = Broker::Signal::Kill});
    ASSERT_TRUE(std::holds_alternative<Broker::ActionResult>(refused.message));
    EXPECT_FALSE(std::get<Broker::ActionResult>(refused.message).success);
    EXPECT_FALSE(child.exited(0ms));

    const auto ended = roundTrip(pipe.get(), 3, Broker::SignalRequest{.target = child.target(), .signal = Broker::Signal::Kill});
    ASSERT_TRUE(std::holds_alternative<Broker::ActionResult>(ended.message));
    EXPECT_TRUE(std::get<Broker::ActionResult>(ended.message).success) << std::get<Broker::ActionResult>(ended.message).errorMessage;
    EXPECT_TRUE(child.exited());

    // Reads it has no reader for are refused, not guessed at.
    const auto environment = roundTrip(pipe.get(), 4, Broker::EnvironmentRequest{.target = child.target()});
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(environment.message));
    const auto firmware = roundTrip(pipe.get(), 5, Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::SmbiosTable});
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(firmware.message));

    pipe.reset();
    EXPECT_EQ(helper.exitCode(), static_cast<DWORD>(BrokerHelper::Exit::Done));
}

TEST(TaskSmackHelperTest, AnswersGarbageWithAnErrorAndEnds)
{
    const BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    const HelperProcess helper(BrokerHelper::commandLine(arguments));
    ASSERT_TRUE(helper.process() != nullptr);
    const UniqueHandle pipe = connectTo(helper, arguments.pipeName);
    ASSERT_TRUE(pipe);
    const std::vector<std::uint8_t> garbage{4, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    std::string error;
    ASSERT_TRUE(BrokerPipe::sendFrame(pipe.get(), garbage, WAIT, {}, error)) << error;
    const auto frame = BrokerPipe::receiveFrame(pipe.get(), WAIT, {});
    ASSERT_TRUE(frame.has_value());
    const auto reply = Broker::decodeResponse(*frame);
    ASSERT_TRUE(reply.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(reply->message));
    EXPECT_EQ(helper.exitCode(), static_cast<DWORD>(BrokerHelper::Exit::Failed));
}

TEST(TaskSmackHelperTest, RefusesACommandLineTaskSmackDoesntMake)
{
    const HelperProcess none(L"");
    EXPECT_EQ(none.exitCode(), static_cast<DWORD>(BrokerHelper::Exit::Usage));
    const HelperProcess otherPipe(std::format(L"--broker \\\\.\\pipe\\Other {} 1", GetCurrentProcessId()));
    EXPECT_EQ(otherPipe.exitCode(), static_cast<DWORD>(BrokerHelper::Exit::Usage));
}

TEST(TaskSmackHelperTest, RefusesAParentOfAnotherStartTime)
{
    BrokerHelper::Arguments arguments = argumentsForThisProcess(freshName());
    ++arguments.parentStartTicks;
    const HelperProcess helper(BrokerHelper::commandLine(arguments));
    EXPECT_EQ(helper.exitCode(), static_cast<DWORD>(BrokerHelper::Exit::NoParent));
}

TEST(TaskSmackHelperTest, ExitsWhenItsParentDoes)
{
    const SuspendedChild parent;
    ASSERT_TRUE(parent.process() != nullptr);
    const ProcessTarget target = parent.target();
    const BrokerHelper::Arguments arguments{
        .pipeName = freshName(), .parentPid = static_cast<DWORD>(target.pid), .parentStartTicks = target.startTimeTicks};
    const HelperProcess helper(BrokerHelper::commandLine(arguments));
    ASSERT_TRUE(helper.process() != nullptr);
    ASSERT_TRUE(pipeExists(arguments.pipeName)); // it holds its parent by then
    ASSERT_NE(TerminateProcess(parent.process(), 1), FALSE);
    EXPECT_EQ(helper.exitCode(), static_cast<DWORD>(BrokerHelper::Exit::NoClient)) << "well within its 30 s accept timeout";
}

#endif // TASKSMACK_WINDOWS_HELPER_PATH

} // namespace
} // namespace Platform::Windows
