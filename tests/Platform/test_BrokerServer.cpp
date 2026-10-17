// Tests for the privileged helper's side of the broker (#2307): handleRequest()'s mapping onto
// IProcessActions, serve()'s handshake rules over a real socketpair, and the tasksmack-helper binary
// itself, run directly (no pkexec) with a socketpair as its stdin.

#include "Mocks/MockProbes.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/Linux/BrokerServer.h"
#include "Platform/Linux/BrokerSocket.h"
#include "Platform/Linux/PkexecBroker.h"
#include "Platform/Linux/ProcParsing.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers) - POSIX signal.h provides kill(), csignal does not
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Platform
{
namespace
{

constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 99};
/// How long a test waits for a reply before it calls the helper hung.
constexpr std::chrono::milliseconds REPLY_TIMEOUT{10'000};

/// Actions whose every call throws, for handleRequest()'s no-throw promise.
class ThrowingActions : public TestMocks::MockProcessActions
{
  public:
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& /*target*/) override
    {
        throw std::runtime_error("boom");
    }
};

[[nodiscard]] Broker::ActionResult actionResult(const Broker::Response& response)
{
    const auto* const result = std::get_if<Broker::ActionResult>(&response);
    return result != nullptr ? *result : Broker::ActionResult{.success = false, .errorMessage = "not an ActionResult", .successNote = {}};
}

TEST(BrokerServerTest, HelloOfThisVersionIsAccepted)
{
    TestMocks::MockProcessActions actions;
    const Broker::Response response = BrokerServer::handleRequest(Broker::Hello{}, actions);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(response));
    EXPECT_TRUE(std::get<Broker::HelloReply>(response).accepted);
    EXPECT_EQ(std::get<Broker::HelloReply>(response).version, Broker::PROTOCOL_VERSION);
}

TEST(BrokerServerTest, HelloOfAnotherVersionIsRefused)
{
    TestMocks::MockProcessActions actions;
    const Broker::Response response = BrokerServer::handleRequest(Broker::Hello{.version = Broker::PROTOCOL_VERSION + 1}, actions);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(response));
    EXPECT_FALSE(std::get<Broker::HelloReply>(response).accepted);
}

TEST(BrokerServerTest, EachSignalReachesItsAction)
{
    TestMocks::MockProcessActions actions;
    for (const Broker::Signal signal : {Broker::Signal::Terminate, Broker::Signal::Kill, Broker::Signal::Suspend, Broker::Signal::Resume})
    {
        EXPECT_TRUE(actionResult(BrokerServer::handleRequest(Broker::SignalRequest{.target = TARGET, .signal = signal}, actions)).success);
    }
    EXPECT_EQ(actions.terminateCount(), 1);
    EXPECT_EQ(actions.killCount(), 1);
    EXPECT_EQ(actions.stopCount(), 1);
    EXPECT_EQ(actions.resumeCount(), 1);
    EXPECT_EQ(actions.lastTarget(), TARGET);
}

TEST(BrokerServerTest, PriorityIoPriorityAndAffinityReachTheirActions)
{
    TestMocks::MockProcessActions actions;
    EXPECT_TRUE(actionResult(BrokerServer::handleRequest(Broker::PriorityRequest{.target = TARGET, .nice = -5}, actions)).success);
    EXPECT_EQ(actions.setPriorityCount(), 1);
    EXPECT_EQ(actions.lastSetPriorityNice(), -5);

    EXPECT_TRUE(actionResult(BrokerServer::handleRequest(
                                 Broker::IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Realtime, .level = 2}, actions))
                    .success);
    EXPECT_EQ(actions.setIoPriorityCount(), 1);
    EXPECT_EQ(actions.lastSetIoPriority().ioClass, IoPriorityClass::Realtime);
    EXPECT_EQ(actions.lastSetIoPriority().level, 2);

    EXPECT_TRUE(
        actionResult(BrokerServer::handleRequest(Broker::AffinityRequest{.target = TARGET, .cpus = CpuAffinity::fromMask(0b101)}, actions))
            .success);
    EXPECT_EQ(actions.setAffinityCount(), 1);
    EXPECT_EQ(actions.lastAffinity(), CpuAffinity::fromMask(0b101));
    EXPECT_EQ(actions.lastTarget(), TARGET);
}

TEST(BrokerServerTest, AFailedActionCarriesItsMessage)
{
    TestMocks::MockProcessActions actions;
    actions.setKillResult(ProcessActionResult::error("Permission denied"));
    const Broker::ActionResult result =
        actionResult(BrokerServer::handleRequest(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}, actions));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Permission denied");
}

TEST(BrokerServerTest, AnActionThatThrowsIsAFailedResult)
{
    ThrowingActions actions;
    const Broker::ActionResult result =
        actionResult(BrokerServer::handleRequest(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Terminate}, actions));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}

TEST(BrokerServerTest, AnEnvironmentRequestIsReadThroughTheReader)
{
    TestMocks::MockProcessActions actions;
    TestMocks::MockProcessEnvironmentReader environment;
    environment.setResult({.status = EnvironmentReadStatus::Ok, .variables = {{.name = "HOME", .value = "/root"}}});
    const Broker::Response response =
        BrokerServer::handleRequest(Broker::EnvironmentRequest{.target = TARGET}, actions, {.environment = &environment});
    ASSERT_TRUE(std::holds_alternative<Broker::EnvironmentReply>(response));
    EXPECT_EQ(std::get<Broker::EnvironmentReply>(response).result.status, EnvironmentReadStatus::Ok);
    EXPECT_EQ(environment.lastTarget(), TARGET);
}

TEST(BrokerServerTest, AnEnvironmentRequestWithoutAReaderIsRefused)
{
    TestMocks::MockProcessActions actions;
    const Broker::Response response = BrokerServer::handleRequest(Broker::EnvironmentRequest{.target = TARGET}, actions);
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(response));
}

TEST(BrokerServerTest, ModulesAndOpenFilesRequestsAreReadThroughTheirReaders)
{
    TestMocks::MockProcessActions actions;
    TestMocks::MockProcessModulesReader modules;
    TestMocks::MockProcessOpenFilesReader openFiles;
    const BrokerServer::Readers readers{.environment = nullptr, .modules = &modules, .openFiles = &openFiles};
    EXPECT_TRUE(std::holds_alternative<Broker::ModulesReply>(
        BrokerServer::handleRequest(Broker::ModulesRequest{.target = TARGET}, actions, readers)));
    EXPECT_EQ(modules.readCount(), 1);
    EXPECT_TRUE(std::holds_alternative<Broker::OpenFilesReply>(
        BrokerServer::handleRequest(Broker::OpenFilesRequest{.target = TARGET}, actions, readers)));
    EXPECT_EQ(openFiles.readCount(), 1);
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(BrokerServer::handleRequest(Broker::ModulesRequest{.target = TARGET}, actions)));
}

/// A connected SOCK_SEQPACKET pair, both ends closed on scope exit and on exec.
class SocketPair
{
  public:
    SocketPair()
    {
        if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, m_Fds.data()) != 0)
        {
            m_Fds = {-1, -1};
        }
    }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
    SocketPair(SocketPair&&) = delete;
    SocketPair& operator=(SocketPair&&) = delete;
    ~SocketPair()
    {
        closeClient();
        closeServer();
    }

    [[nodiscard]] bool valid() const
    {
        return m_Fds[0] >= 0;
    }
    [[nodiscard]] int client() const
    {
        return m_Fds[0];
    }
    [[nodiscard]] int server() const
    {
        return m_Fds[1];
    }
    void closeClient()
    {
        closeFd(m_Fds[0]);
    }
    void closeServer()
    {
        closeFd(m_Fds[1]);
    }

  private:
    static void closeFd(int& fd)
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
    }

    std::array<int, 2> m_Fds{-1, -1};
};

/// What roundTrip() returns when there is no reply.
[[nodiscard]] Broker::Decoded<Broker::Response> noReply()
{
    return {.requestId = 0, .message = Broker::ErrorReply{.message = "no reply"}};
}

/// Sends @p request and returns the decoded reply; a test failure, and an ErrorReply, when none arrives.
[[nodiscard]] Broker::Decoded<Broker::Response> roundTrip(int fd, std::uint32_t id, const Broker::Request& request)
{
    std::string error;
    if (!BrokerSocket::sendFrame(fd, Broker::encodeRequest(id, request), error))
    {
        ADD_FAILURE() << "send failed: " << error;
        return noReply();
    }
    const auto frame = BrokerSocket::receiveFrame(fd, REPLY_TIMEOUT, std::stop_token{});
    if (!frame.has_value())
    {
        ADD_FAILURE() << "no reply";
        return noReply();
    }
    auto decoded = Broker::decodeResponse(*frame);
    if (!decoded.has_value())
    {
        ADD_FAILURE() << "undecodable reply: " << decoded.error();
        return noReply();
    }
    return *decoded;
}

/// serve() on a thread, over @p pair's server end.
[[nodiscard]] std::future<BrokerServer::SessionEnd>
serveAsync(SocketPair& pair, IProcessActions& actions, std::chrono::milliseconds idle = std::chrono::seconds{10})
{
    return std::async(std::launch::async,
                      [&pair, &actions, idle] { return BrokerServer::serve(pair.server(), actions, idle, std::stop_token{}); });
}

TEST(BrokerServeTest, AnswersRequestsAfterTheHelloAndEndsWhenTheClientCloses)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    auto session = serveAsync(pair, actions);

    const auto hello = roundTrip(pair.client(), 1, Broker::Hello{});
    EXPECT_EQ(hello.requestId, 1U);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));
    EXPECT_TRUE(std::get<Broker::HelloReply>(hello.message).accepted);

    const auto kill = roundTrip(pair.client(), 2, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill});
    EXPECT_EQ(kill.requestId, 2U);
    EXPECT_TRUE(actionResult(kill.message).success);
    EXPECT_EQ(actions.killCount(), 1);

    pair.closeClient();
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::ClientClosed);
}

TEST(BrokerServeTest, AnActionBeforeTheHelloIsRefusedAndEndsTheSession)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    auto session = serveAsync(pair, actions);

    const auto reply = roundTrip(pair.client(), 7, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill});
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(reply.message));
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::HandshakeFailed);
    EXPECT_EQ(actions.killCount(), 0);
}

TEST(BrokerServeTest, AHelloOfAnotherVersionEndsTheSession)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    auto session = serveAsync(pair, actions);

    const auto reply = roundTrip(pair.client(), 1, Broker::Hello{.version = Broker::PROTOCOL_VERSION + 1});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(reply.message));
    EXPECT_FALSE(std::get<Broker::HelloReply>(reply.message).accepted);
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::HandshakeFailed);
}

TEST(BrokerServeTest, AMalformedFrameAfterTheHelloGetsAnErrorAndTheSessionGoesOn)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    auto session = serveAsync(pair, actions);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 1, Broker::Hello{}).message));

    std::string error;
    const std::vector<std::uint8_t> garbage{1, 2, 3};
    ASSERT_TRUE(BrokerSocket::sendFrame(pair.client(), garbage, error)) << error;
    const auto frame = BrokerSocket::receiveFrame(pair.client(), REPLY_TIMEOUT, std::stop_token{});
    ASSERT_TRUE(frame.has_value());
    const auto decoded = Broker::decodeResponse(*frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(decoded->message));

    const auto stop = roundTrip(pair.client(), 3, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Suspend});
    EXPECT_TRUE(actionResult(stop.message).success);

    pair.closeClient();
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::ClientClosed);
}

TEST(BrokerServeTest, AnIdleSessionEnds)
{
    const SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    EXPECT_EQ(BrokerServer::serve(pair.server(), actions, std::chrono::milliseconds{150}, std::stop_token{}),
              BrokerServer::SessionEnd::Idle);
}

TEST(BrokerServeTest, AStoppedSessionEnds)
{
    const SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(BrokerServer::serve(pair.server(), actions, std::chrono::seconds{10}, stop.get_token()), BrokerServer::SessionEnd::Stopped);
}

TEST(BrokerSocketTest, PeerCredentialsAreThisProcess)
{
    const SocketPair pair;
    ASSERT_TRUE(pair.valid());
    EXPECT_TRUE(BrokerSocket::isSeqPacketSocket(pair.client()));
    const auto peer = BrokerSocket::peerCredentials(pair.client());
    ASSERT_TRUE(peer.has_value());
    EXPECT_EQ(peer.value_or(BrokerSocket::PeerCredentials{}).pid, ::getpid());
    EXPECT_EQ(peer.value_or(BrokerSocket::PeerCredentials{}).uid, ::getuid());
}

TEST(BrokerSocketTest, APipeIsNotASeqPacketSocket)
{
    std::array<int, 2> fds{-1, -1};
    ASSERT_EQ(::pipe(fds.data()), 0);
    EXPECT_FALSE(BrokerSocket::isSeqPacketSocket(fds[0]));
    EXPECT_FALSE(BrokerSocket::peerCredentials(fds[0]).has_value());
    ::close(fds[0]);
    ::close(fds[1]);
}

#ifdef TASKSMACK_HELPER_PATH

// NOLINTBEGIN(misc-include-cleaner) - include-cleaner lacks mappings for <sys/wait.h>'s WNOHANG and W* macros

/// tasksmack-helper, started with @p stdinFd as its stdin; reaped (killed first if need be) on scope exit.
class HelperProcess
{
  public:
    explicit HelperProcess(int stdinFd, const char* argument = "--broker")
    {
        posix_spawn_file_actions_t files{};
        posix_spawn_file_actions_init(&files);
        posix_spawn_file_actions_adddup2(&files, stdinFd, STDIN_FILENO);
        std::string path = TASKSMACK_HELPER_PATH;
        std::string arg = argument;
        std::array<char*, 3> argv{path.data(), arg.data(), nullptr};
        if (::posix_spawn(&m_Pid, path.c_str(), &files, nullptr, argv.data(), environ) != 0)
        {
            m_Pid = -1;
        }
        posix_spawn_file_actions_destroy(&files);
    }
    HelperProcess(const HelperProcess&) = delete;
    HelperProcess& operator=(const HelperProcess&) = delete;
    HelperProcess(HelperProcess&&) = delete;
    HelperProcess& operator=(HelperProcess&&) = delete;
    ~HelperProcess()
    {
        if (m_Pid > 0 && !m_ExitCode.has_value())
        {
            ::kill(m_Pid, SIGKILL);
            ::waitpid(m_Pid, nullptr, 0);
        }
    }

    [[nodiscard]] bool started() const
    {
        return m_Pid > 0;
    }
    /// Its exit status once it exits within a few seconds, or nullopt (killed by a signal, or still running).
    [[nodiscard]] std::optional<int> exitCode()
    {
        for (int attempt = 0; attempt < 1000 && !m_ExitCode.has_value(); ++attempt)
        {
            int status = 0;
            if (::waitpid(m_Pid, &status, WNOHANG) == m_Pid)
            {
                m_ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                break;
            }
            ::usleep(10'000);
        }
        return m_ExitCode;
    }

  private:
    pid_t m_Pid = -1;
    std::optional<int> m_ExitCode;
};

/// A forked child that sleeps until signalled; reaped on scope exit.
class SleepingChild
{
  public:
    SleepingChild() : m_Pid(::fork())
    {
        if (m_Pid == 0)
        {
            for (;;)
            {
                ::pause();
            }
        }
    }
    SleepingChild(const SleepingChild&) = delete;
    SleepingChild& operator=(const SleepingChild&) = delete;
    SleepingChild(SleepingChild&&) = delete;
    SleepingChild& operator=(SleepingChild&&) = delete;
    ~SleepingChild()
    {
        if (m_Pid > 0)
        {
            ::kill(m_Pid, SIGKILL);
            ::waitpid(m_Pid, nullptr, 0);
        }
    }

    [[nodiscard]] bool started() const
    {
        return m_Pid > 0;
    }
    [[nodiscard]] ProcessTarget target() const
    {
        std::ifstream stat("/proc/" + std::to_string(m_Pid) + "/stat");
        const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
        return {.pid = static_cast<std::int32_t>(m_Pid), .startTimeTicks = ProcParsing::parseStatStartTime(line).value_or(0)};
    }
    /// The signal that ended it, once it ends within a few seconds; 0 if it doesn't.
    [[nodiscard]] int terminatingSignal()
    {
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            int status = 0;
            if (::waitpid(m_Pid, &status, WNOHANG) == m_Pid)
            {
                m_Pid = -1;
                return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            }
            ::usleep(10'000);
        }
        return 0;
    }

  private:
    pid_t m_Pid;
};

TEST(TasksmackHelperTest, TerminatesAProcessForItsClientAndExitsWhenTheClientCloses)
{
    SleepingChild child;
    ASSERT_TRUE(child.started());
    const ProcessTarget target = child.target();
    ASSERT_NE(target.startTimeTicks, 0U);

    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    HelperProcess helper(pair.server());
    ASSERT_TRUE(helper.started());
    pair.closeServer();

    const auto hello = roundTrip(pair.client(), 1, Broker::Hello{});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));
    EXPECT_TRUE(std::get<Broker::HelloReply>(hello.message).accepted);

    // A stale identity (the right PID, another start time) is refused, as LinuxProcessActions refuses it.
    const auto stale = roundTrip(pair.client(),
                                 2,
                                 Broker::SignalRequest{.target = {.pid = target.pid, .startTimeTicks = target.startTimeTicks + 1},
                                                       .signal = Broker::Signal::Terminate});
    EXPECT_FALSE(actionResult(stale.message).success);

    const auto terminate = roundTrip(pair.client(), 3, Broker::SignalRequest{.target = target, .signal = Broker::Signal::Terminate});
    EXPECT_EQ(terminate.requestId, 3U);
    EXPECT_TRUE(actionResult(terminate.message).success) << actionResult(terminate.message).errorMessage;
    EXPECT_EQ(child.terminatingSignal(), SIGTERM);

    pair.closeClient();
    EXPECT_EQ(helper.exitCode(), 0);
}

TEST(TasksmackHelperTest, AnswersGarbageWithAnErrorAndEnds)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    HelperProcess helper(pair.server());
    ASSERT_TRUE(helper.started());
    pair.closeServer();

    std::string error;
    const std::vector<std::uint8_t> garbage{0xFF, 0xFF};
    ASSERT_TRUE(BrokerSocket::sendFrame(pair.client(), garbage, error)) << error;
    const auto frame = BrokerSocket::receiveFrame(pair.client(), REPLY_TIMEOUT, std::stop_token{});
    ASSERT_TRUE(frame.has_value());
    const auto decoded = Broker::decodeResponse(*frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(decoded->message));
    EXPECT_EQ(helper.exitCode(), 1); // a session that doesn't start with a Hello failed
}

TEST(TasksmackHelperTest, RefusesAStdinThatIsNotItsParentsSocket)
{
    std::array<int, 2> fds{-1, -1};
    ASSERT_EQ(::pipe2(fds.data(), O_CLOEXEC), 0);
    HelperProcess helper(fds[0]);
    ASSERT_TRUE(helper.started());
    EXPECT_EQ(helper.exitCode(), 3);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(TasksmackHelperTest, RefusesToRunWithoutBroker)
{
    const SocketPair pair;
    ASSERT_TRUE(pair.valid());
    HelperProcess helper(pair.server(), "--help");
    ASSERT_TRUE(helper.started());
    EXPECT_EQ(helper.exitCode(), 2);
}

TEST(PkexecBrokerTest, ConnectsToTheHelperAndTerminatesAProcessThroughIt)
{
    SleepingChild child;
    ASSERT_TRUE(child.started());
    const ProcessTarget target = child.target();
    ASSERT_NE(target.startTimeTicks, 0U);

    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}); // the helper itself, without pkexec
    EXPECT_FALSE(broker.connected());
    const auto connected = broker.connect(std::stop_token{});
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_TRUE(broker.connected());
    EXPECT_TRUE(broker.connect(std::stop_token{}).has_value()); // already connected: nothing to do

    const ProcessActionResult result =
        actionResultOf(broker.call(Broker::SignalRequest{.target = target, .signal = Broker::Signal::Terminate}, std::stop_token{}));
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(child.terminatingSignal(), SIGTERM);
}

TEST(PkexecBrokerTest, TheHelperReadsAProcesssEnvironment)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {});
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const auto response = broker.call(Broker::EnvironmentRequest{.target = child.target()}, std::stop_token{});
    ASSERT_TRUE(response.has_value()) << response.error();
    ASSERT_TRUE(std::holds_alternative<Broker::EnvironmentReply>(*response));
    const EnvironmentReadResult& result = std::get<Broker::EnvironmentReply>(*response).result;
    EXPECT_EQ(result.status, EnvironmentReadStatus::Ok);
    EXPECT_FALSE(result.variables.empty()); // the child inherited this test's environment
}

TEST(PkexecBrokerTest, TheHelperListsAProcesssModulesAndOpenFiles)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {});
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());

    const auto modules = broker.call(Broker::ModulesRequest{.target = child.target()}, std::stop_token{});
    ASSERT_TRUE(modules.has_value()) << modules.error();
    ASSERT_TRUE(std::holds_alternative<Broker::ModulesReply>(*modules));
    EXPECT_EQ(std::get<Broker::ModulesReply>(*modules).result.status, ModulesReadStatus::Ok);
    EXPECT_FALSE(std::get<Broker::ModulesReply>(*modules).result.modules.empty()); // at least the C library

    const auto files = broker.call(Broker::OpenFilesRequest{.target = child.target()}, std::stop_token{});
    ASSERT_TRUE(files.has_value()) << files.error();
    ASSERT_TRUE(std::holds_alternative<Broker::OpenFilesReply>(*files));
    EXPECT_EQ(std::get<Broker::OpenFilesReply>(*files).result.status, OpenFilesReadStatus::Ok);
}

TEST(PkexecBrokerTest, ACallWithoutAHelperSaysSo)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {});
    const auto response = broker.call(Broker::Hello{}, std::stop_token{});
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error(), "not connected");
    EXPECT_FALSE(actionResultOf(response).success);
}

TEST(PkexecBrokerTest, DisconnectEndsTheSessionAndConnectStartsAnother)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {});
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    broker.disconnect();
    EXPECT_FALSE(broker.connected());
    EXPECT_FALSE(broker.call(Broker::Hello{}, std::stop_token{}).has_value());
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const auto hello = broker.call(Broker::Hello{}, std::stop_token{});
    ASSERT_TRUE(hello.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::HelloReply>(*hello));
}

TEST(PkexecBrokerTest, ADismissedPromptIsReportedAsSuch)
{
    // A launcher that exits as pkexec does when the prompt is dismissed (126).
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {"sh", "-c", "exit 126", "sh"});
    const auto connected = broker.connect(std::stop_token{});
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error(), "the password prompt was dismissed");
    EXPECT_FALSE(broker.connected());
}

TEST(PkexecBrokerTest, AMissingLauncherIsReported)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {"/nonexistent/tasksmack-pkexec"});
    const auto connected = broker.connect(std::stop_token{});
    ASSERT_FALSE(connected.has_value());
    EXPECT_FALSE(connected.error().empty());
    EXPECT_FALSE(broker.connected());
}

TEST(PkexecBrokerTest, AHelperThatNeverAnswersTimesOut)
{
    // A launcher that keeps the socket open and says nothing, and exits once the broker closes it, so it
    // is reaped then: a launcher left running would be this process's child for later tests that wait
    // for any child (DetachedSpawnTest, #2333).
    {
        PkexecBroker broker(TASKSMACK_HELPER_PATH, {"sh", "-c", "cat >/dev/null", "sh"});
        const auto connected = broker.connectWithin(std::stop_token{}, std::chrono::milliseconds{200});
        ASSERT_FALSE(connected.has_value());
        EXPECT_EQ(connected.error(), "the helper didn't answer in time");
    }
    // The broker gone, so is its launcher: nothing of it is left for this process to wait for.
    errno = 0;
    EXPECT_EQ(::waitpid(-1, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

// NOLINTEND(misc-include-cleaner)

#endif // TASKSMACK_HELPER_PATH

} // namespace
} // namespace Platform
