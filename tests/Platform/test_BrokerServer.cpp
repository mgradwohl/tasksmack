// Tests for the privileged helper's side of the broker (#2307): serve()'s handshake rules over a real
// socketpair, and the tasksmack-helper binary itself, run directly (no pkexec) with a socketpair as its
// stdin. handleRequest()'s mapping, which both helpers share, is test_BrokerRequestMapping.cpp's.

#include "Mocks/MockProbes.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerRequestMapping.h"
#include "Platform/BrokerSessionRules.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/BrokerServer.h"
#include "Platform/Linux/BrokerSocket.h"
#include "Platform/Linux/FirmwareFileRead.h"
#include "Platform/Linux/HelperPrivileges.h"
#include "Platform/Linux/HelperSandboxMode.h"
#include "Platform/Linux/PkexecBroker.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/Linux/ProcStatusSecurityParser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
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

[[nodiscard]] Broker::ActionResult actionResult(const Broker::Response& response)
{
    const auto* const result = std::get_if<Broker::ActionResult>(&response);
    return result != nullptr ? *result : Broker::ActionResult{.success = false, .errorMessage = "not an ActionResult", .successNote = {}};
}

TEST(FirmwareFileReadTest, EachFileIsReadFromItsOnePath)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("tasksmack_fw_" + std::to_string(::getpid()));
    std::filesystem::create_directories(root / "sys/firmware/dmi/tables");
    std::filesystem::create_directories(root / "sys/class/dmi/id");
    std::ofstream(root / "sys/firmware/dmi/tables/DMI", std::ios::binary) << std::string("\x11\x15table", 7);
    std::ofstream(root / "sys/class/dmi/id/product_uuid") << "uuid\n";
    std::ofstream(root / "sys/class/dmi/id/board_serial") << "secret\n";
    std::filesystem::permissions(root / "sys/class/dmi/id/board_serial", std::filesystem::perms::none);

    const Broker::FirmwareFileReply table = Linux::readFirmwareFileAt(root, Broker::FirmwareFile::SmbiosTable);
    EXPECT_EQ(table.status, Broker::FirmwareFileStatus::Ok);
    EXPECT_EQ(table.bytes.size(), 7U);
    const Broker::FirmwareFileReply uuid = Linux::readFirmwareFileAt(root, Broker::FirmwareFile::ProductUuid);
    EXPECT_EQ(std::string(uuid.bytes.begin(), uuid.bytes.end()), "uuid\n");
    EXPECT_EQ(Linux::readFirmwareFileAt(root, Broker::FirmwareFile::ProductSerial).status, Broker::FirmwareFileStatus::NotFound);
    if (::geteuid() != 0) // root reads a mode-000 file
    {
        EXPECT_EQ(Linux::readFirmwareFileAt(root, Broker::FirmwareFile::BoardSerial).status, Broker::FirmwareFileStatus::Unreadable);
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// #2417: the entry point's path too, and a file past the protocol's cap is refused rather than cut.
TEST(FirmwareFileReadTest, TheEntryPointIsReadAndAnOversizedFileIsUnreadable)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("tasksmack_fw_big_" + std::to_string(::getpid()));
    std::filesystem::create_directories(root / "sys/firmware/dmi/tables");
    std::ofstream(root / "sys/firmware/dmi/tables/smbios_entry_point", std::ios::binary) << std::string("_SM3_", 5);
    std::ofstream(root / "sys/firmware/dmi/tables/DMI", std::ios::binary) << std::string(Broker::MAX_FIRMWARE_FILE_BYTES + 1, 'x');

    const Broker::FirmwareFileReply entry = Linux::readFirmwareFileAt(root, Broker::FirmwareFile::SmbiosEntryPoint);
    EXPECT_EQ(entry.status, Broker::FirmwareFileStatus::Ok);
    EXPECT_EQ(std::string(entry.bytes.begin(), entry.bytes.end()), "_SM3_");
    EXPECT_EQ(Linux::firmwareFilePath(root, Broker::FirmwareFile::SmbiosEntryPoint), root / "sys/firmware/dmi/tables/smbios_entry_point");

    const Broker::FirmwareFileReply table = Linux::readFirmwareFileAt(root, Broker::FirmwareFile::SmbiosTable);
    EXPECT_EQ(table.status, Broker::FirmwareFileStatus::Unreadable);
    EXPECT_TRUE(table.bytes.empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
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

/// An idle clock that moves only when it is stepped, so a test says when the idle timeout runs out (#2546).
class SteppedClock
{
  public:
    [[nodiscard]] Broker::IdleClock clock() const
    {
        return [ticks = m_Ticks]
        {
            return std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{ticks->load()}};
        };
    }

    void step(std::chrono::steady_clock::duration by) const
    {
        *m_Ticks += by.count();
    }

  private:
    std::shared_ptr<std::atomic<std::chrono::steady_clock::rep>> m_Ticks = std::make_shared<std::atomic<std::chrono::steady_clock::rep>>(0);
};

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

// #2367: a SEQPACKET packet can't be larger than the socket's send buffer (about 208 KiB by default), so a
// larger frame goes in packets and is joined again on receipt.
TEST(BrokerSocketTest, AFrameLargerThanOnePacketArrivesWhole)
{
    for (const std::size_t size : {std::size_t{1} << 20U, BrokerSocket::MAX_PACKET_BYTES * 3 - 4 - Broker::HEADER_BYTES - 5})
    {
        SocketPair pair;
        ASSERT_TRUE(pair.valid());
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = static_cast<std::uint8_t>(i * 31U);
        }
        const Broker::Response sent = Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Ok, .bytes = bytes};
        const std::vector<std::uint8_t> frame = Broker::encodeResponse(5, sent);
        // Sent from another thread: the socket buffers less than the frame
        auto sending = std::async(std::launch::async,
                                  [&]
                                  {
                                      std::string error;
                                      return BrokerSocket::sendFrame(pair.server(), frame, error);
                                  });
        const auto received = BrokerSocket::receiveFrame(pair.client(), REPLY_TIMEOUT, std::stop_token{});
        ASSERT_TRUE(sending.get());
        ASSERT_TRUE(received.has_value()) << size;
        EXPECT_EQ(received->size(), frame.size());
        const auto decoded = Broker::decodeResponse(*received);
        ASSERT_TRUE(decoded.has_value()) << decoded.error();
        EXPECT_EQ(decoded->message, sent);
    }
}

TEST(BrokerSocketTest, AFrameCutShortMidwayIsAFailure)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    std::vector<std::uint8_t> first(BrokerSocket::MAX_PACKET_BYTES, 0);
    first[0] = 0x00; // says 1 MiB follows
    first[2] = 0x10;
    ASSERT_EQ(::send(pair.server(), first.data(), first.size(), 0), static_cast<ssize_t>(first.size()));
    pair.closeServer();
    const auto received = BrokerSocket::receiveFrame(pair.client(), REPLY_TIMEOUT, std::stop_token{});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerSocket::ReceiveEnd::Failed);
}

// #2406: a client that keeps the channel after a timeout would read a frame's remaining packets as the next
// frame, so a frame the timeout cuts short is a failure, not a timeout.
TEST(BrokerSocketTest, AFrameCutShortByTheTimeoutIsAFailure)
{
    const SocketPair pair;
    ASSERT_TRUE(pair.valid());
    std::vector<std::uint8_t> first(BrokerSocket::MAX_PACKET_BYTES, 0);
    first[2] = 0x10; // says 1 MiB follows; none does, and the server end stays open
    ASSERT_EQ(::send(pair.server(), first.data(), first.size(), 0), static_cast<ssize_t>(first.size()));
    const auto received = BrokerSocket::receiveFrame(pair.client(), std::chrono::milliseconds{50}, std::stop_token{});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerSocket::ReceiveEnd::Failed);
}

TEST(BrokerServeTest, AReplyLargerThanTheSocketBufferReachesTheClient)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    BrokerServer::Readers readers;
    readers.firmwareFile = [](Broker::FirmwareFile)
    {
        return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Ok, .bytes = std::vector<std::uint8_t>(900'000, 0x5A)};
    };
    auto session =
        std::async(std::launch::async,
                   [&] { return BrokerServer::serve(pair.server(), actions, std::chrono::seconds{10}, std::stop_token{}, readers); });
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 1, Broker::Hello{}).message));
    const auto reply = roundTrip(pair.client(), 2, Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::SmbiosTable});
    ASSERT_TRUE(std::holds_alternative<Broker::FirmwareFileReply>(reply.message));
    EXPECT_EQ(std::get<Broker::FirmwareFileReply>(reply.message).bytes.size(), 900'000U);
    pair.closeClient();
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::ClientClosed);
}

// #2403: a Modules read of a process mapping tens of thousands of distinct files made a reply past one
// frame; the helper couldn't send it and ended the session, so the authorization was lost for every other
// section and each retry asked for the password again. Now the list is cut to fit and the session goes on.
TEST(BrokerServeTest, AModulesListPastOneFrameIsCutAndTheSessionGoesOn)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    TestMocks::MockProcessModulesReader modules;
    ModulesReadResult many{.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}};
    many.modules.assign(Broker::MAX_MODULES,
                        {.path = std::string(200, 'm'), .baseAddress = 1, .sizeBytes = 2, .version = std::nullopt, .deleted = false});
    modules.setResult(std::move(many));
    const BrokerServer::Readers readers{.environment = nullptr, .modules = &modules, .openFiles = nullptr, .connections = nullptr};
    auto session =
        std::async(std::launch::async,
                   [&] { return BrokerServer::serve(pair.server(), actions, std::chrono::seconds{30}, std::stop_token{}, readers); });
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 1, Broker::Hello{}).message));

    const auto reply = roundTrip(pair.client(), 2, Broker::ModulesRequest{.target = TARGET});
    ASSERT_TRUE(std::holds_alternative<Broker::ModulesReply>(reply.message));
    const ModulesReadResult& result = std::get<Broker::ModulesReply>(reply.message).result;
    EXPECT_TRUE(result.truncated);
    EXPECT_FALSE(result.modules.empty());

    // The same session answers the next request: the helper is still there.
    const auto kill = roundTrip(pair.client(), 3, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill});
    EXPECT_TRUE(actionResult(kill.message).success);
    pair.closeClient();
    ASSERT_EQ(session.wait_for(REPLY_TIMEOUT), std::future_status::ready);
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::ClientClosed);
}

// #2403, defence in depth: any other reply too large for a frame fails that one request with an
// ErrorReply instead of ending the session. Crash events of the longest strings are such a reply.
TEST(BrokerServeTest, AReplyTooLargeForAFrameIsAnErrorAndTheSessionGoesOn)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    BrokerServer::Readers readers;
    readers.crashes = []
    {
        CrashesInfo info;
        info.available = true;
        info.listed = true;
        const std::string longest(Broker::MAX_STRING_BYTES, 'c'); // 200 events of two: about 26 MiB
        CrashEvent event;
        event.unixSeconds = 1;
        event.application = longest;
        event.executable = longest;
        info.events.assign(CRASH_LIST_MAX, event);
        return info;
    };
    auto session =
        std::async(std::launch::async,
                   [&] { return BrokerServer::serve(pair.server(), actions, std::chrono::seconds{30}, std::stop_token{}, readers); });
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 1, Broker::Hello{}).message));

    const auto reply = roundTrip(pair.client(), 2, Broker::CrashesRequest{});
    EXPECT_EQ(reply.requestId, 2U);
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(reply.message));

    const auto kill = roundTrip(pair.client(), 3, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill});
    EXPECT_TRUE(actionResult(kill.message).success);
    pair.closeClient();
    ASSERT_EQ(session.wait_for(REPLY_TIMEOUT), std::future_status::ready);
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::ClientClosed);
}

// #2369: the sampler's PSS/USS reads of a watched process come every refresh by themselves; they must not
// keep the helper running past its idle timeout.
TEST(BrokerServeTest, PssAndUssReadsDontPutOffTheIdleExit)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    // The idle timeout runs on a clock each read steps by half of it, on the serve thread before the reply
    // goes out, so how the session ends hangs on the order of the requests, not on the machine's speed
    // (#2546). The clock's hour is far longer than the test runs, so nothing times out for real.
    constexpr std::chrono::milliseconds IDLE = std::chrono::hours{1};
    const SteppedClock clock;
    BrokerServer::Readers readers;
    readers.memoryRollup = [clock, step = IDLE / 2](std::int32_t, std::uint64_t) -> MemoryRollupRead
    {
        clock.step(step);
        return MemoryRollup{};
    };
    auto session = std::async(std::launch::async,
                              [&, idle = clock.clock()]
                              { return BrokerServer::serve(pair.server(), actions, IDLE, std::stop_token{}, readers, idle); });
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 1, Broker::Hello{}).message));
    for (std::uint32_t id = 2; id <= 3; ++id) // at half the idle timeout after the Hello, then at all of it
    {
        EXPECT_TRUE(std::holds_alternative<Broker::MemoryRollupReply>(
            roundTrip(pair.client(), id, Broker::MemoryRollupRequest{.target = TARGET}).message));
    }
    // Idle the timeout after the Hello, not after the last read
    const auto status = session.wait_for(REPLY_TIMEOUT);
    pair.closeClient(); // a session the reads put off would wait out IDLE for real; this ends it now
    ASSERT_EQ(status, std::future_status::ready) << "the reads put off the idle exit";
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::Idle);
}

// #2405: Process Details re-reads Environment, Modules, Open files and Connections through a connected helper
// on its refresh timer, and System Information its firmware files and crashes; none of it is the user acting,
// so none of it may keep the helper running past its idle timeout.
TEST(BrokerServeTest, ProcessDetailsAndSystemInformationReadsDontPutOffTheIdleExit)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    TestMocks::MockProcessEnvironmentReader environment;
    TestMocks::MockProcessModulesReader modules;
    TestMocks::MockProcessOpenFilesReader openFiles;
    TestMocks::MockProcessConnectionsReader connections;
    BrokerServer::Readers readers{.environment = &environment, .modules = &modules, .openFiles = &openFiles, .connections = &connections};
    readers.firmwareFile = [](Broker::FirmwareFile)
    {
        return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Unreadable, .bytes = {}};
    };
    // The idle timeout runs on a clock each round's last read steps by half of it, as in the test above (#2546).
    constexpr std::chrono::milliseconds IDLE = std::chrono::hours{1};
    const SteppedClock clock;
    readers.crashes = [clock, step = IDLE / 2]
    {
        clock.step(step);
        return CrashesInfo{};
    };
    auto session = std::async(std::launch::async,
                              [&, idle = clock.clock()]
                              { return BrokerServer::serve(pair.server(), actions, IDLE, std::stop_token{}, readers, idle); });
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 1, Broker::Hello{}).message));
    std::uint32_t id = 2;
    for (int round = 0; round < 2; ++round) // the second round's last read takes the clock to the idle timeout
    {
        EXPECT_TRUE(std::holds_alternative<Broker::ConnectionsReply>(
            roundTrip(pair.client(), id++, Broker::ConnectionsRequest{.target = TARGET}).message));
        EXPECT_TRUE(std::holds_alternative<Broker::EnvironmentReply>(
            roundTrip(pair.client(), id++, Broker::EnvironmentRequest{.target = TARGET}).message));
        EXPECT_TRUE(
            std::holds_alternative<Broker::ModulesReply>(roundTrip(pair.client(), id++, Broker::ModulesRequest{.target = TARGET}).message));
        EXPECT_TRUE(std::holds_alternative<Broker::OpenFilesReply>(
            roundTrip(pair.client(), id++, Broker::OpenFilesRequest{.target = TARGET}).message));
        EXPECT_TRUE(std::holds_alternative<Broker::FirmwareFileReply>(
            roundTrip(pair.client(), id++, Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::SmbiosTable}).message));
        EXPECT_TRUE(std::holds_alternative<Broker::CrashesReply>(roundTrip(pair.client(), id++, Broker::CrashesRequest{}).message));
    }
    // Idle the timeout after the Hello, not after the last reads
    const auto status = session.wait_for(REPLY_TIMEOUT);
    pair.closeClient(); // a session the reads put off would wait out IDLE for real; this ends it now
    ASSERT_EQ(status, std::future_status::ready) << "the reads put off the idle exit";
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::Idle);
}

// The user acting is what keeps the helper running: an action restarts the idle timeout.
TEST(BrokerServeTest, AnActionPutsOffTheIdleExit)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    constexpr std::chrono::milliseconds IDLE{2000};
    auto session = serveAsync(pair, actions, IDLE);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 1, Broker::Hello{}).message));
    std::this_thread::sleep_for(std::chrono::milliseconds{1000});
    EXPECT_TRUE(actionResult(roundTrip(pair.client(), 2, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Resume}).message)
                    .success);
    // At about 2.5 s: past the Hello's timeout, inside the action's (about 3 s)
    EXPECT_EQ(session.wait_for(std::chrono::milliseconds{1500}), std::future_status::timeout);
    ASSERT_EQ(session.wait_for(std::chrono::milliseconds{2000}), std::future_status::ready);
    EXPECT_EQ(session.get(), BrokerServer::SessionEnd::Idle);
}

/// serve() on a thread with an idle timeout of an hour, on a clock each PSS/USS read steps past it: a
/// read ends a session that isn't held, at the next look for a frame (#2728).
class SteppedSession
{
  public:
    static constexpr std::chrono::milliseconds IDLE = std::chrono::hours{1};

    explicit SteppedSession(SocketPair& pair) : m_Pair(pair)
    {
        m_Readers.memoryRollup = [clock = m_Clock](std::int32_t, std::uint64_t) -> MemoryRollupRead
        {
            clock.step(IDLE * 2);
            return MemoryRollup{};
        };
        m_Session = std::async(std::launch::async,
                               [this, &pair, idle = m_Clock.clock()]
                               { return BrokerServer::serve(pair.server(), m_Actions, IDLE, std::stop_token{}, m_Readers, idle); });
    }

    // A held session ends only when the client closes: closed here, a failed assertion can't leave the
    // future's destructor waiting on it forever.
    ~SteppedSession()
    {
        m_Pair.closeClient();
    }
    SteppedSession(const SteppedSession&) = delete;
    SteppedSession& operator=(const SteppedSession&) = delete;
    SteppedSession(SteppedSession&&) = delete;
    SteppedSession& operator=(SteppedSession&&) = delete;

    [[nodiscard]] std::future<BrokerServer::SessionEnd>& session()
    {
        return m_Session;
    }

  private:
    SocketPair& m_Pair;
    TestMocks::MockProcessActions m_Actions;
    SteppedClock m_Clock;
    BrokerServer::Readers m_Readers;
    std::future<BrokerServer::SessionEnd> m_Session;
};

[[nodiscard]] bool readsPssAndUss(SocketPair& pair, std::uint32_t id)
{
    return std::holds_alternative<Broker::MemoryRollupReply>(
        roundTrip(pair.client(), id, Broker::MemoryRollupRequest{.target = TARGET}).message);
}

TEST(BrokerServeTest, ASessionHeldFromItsHelloNeverEndsIdleAndEndsWhenTheClientCloses)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    SteppedSession served(pair);
    const auto hello = roundTrip(pair.client(), 1, Broker::Hello{.forSession = true});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));
    ASSERT_TRUE(std::get<Broker::HelloReply>(hello.message).accepted);
    for (std::uint32_t id = 2; id <= 4; ++id) // each read leaves the clock two idle timeouts on
    {
        EXPECT_TRUE(readsPssAndUss(pair, id)) << id;
    }
    EXPECT_EQ(served.session().wait_for(std::chrono::milliseconds{200}), std::future_status::timeout);
    pair.closeClient();
    EXPECT_EQ(served.session().get(), BrokerServer::SessionEnd::ClientClosed);
}

TEST(BrokerServeTest, AHelloMidSessionHoldsIt)
{
    // A helper an "Authenticate and retry" started becomes the session's with no second prompt (#2728).
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    SteppedSession served(pair);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 1, Broker::Hello{}).message));
    const auto held = roundTrip(pair.client(), 2, Broker::Hello{.forSession = true});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(held.message));
    EXPECT_TRUE(std::get<Broker::HelloReply>(held.message).accepted);
    EXPECT_TRUE(readsPssAndUss(pair, 3));
    // A Hello without the flag afterwards doesn't put the idle exit back.
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 4, Broker::Hello{}).message));
    EXPECT_TRUE(readsPssAndUss(pair, 5));
    pair.closeClient();
    EXPECT_EQ(served.session().get(), BrokerServer::SessionEnd::ClientClosed);
}

TEST(BrokerServeTest, ASessionThatIsntHeldEndsIdleOnTheSameClock)
{
    // The control for the two above: the same read, without the flag, ends the session.
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    SteppedSession served(pair);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(roundTrip(pair.client(), 1, Broker::Hello{}).message));
    EXPECT_TRUE(readsPssAndUss(pair, 2));
    const auto status = served.session().wait_for(REPLY_TIMEOUT);
    pair.closeClient();
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(served.session().get(), BrokerServer::SessionEnd::Idle);
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

// --- #2417: what the helper does when a read throws, a client breaks off, or a frame overruns ----------

TEST(BrokerServeTest, ANoIdleTimeEndsTheSessionAtOnce)
{
    const SocketPair pair;
    ASSERT_TRUE(pair.valid());
    TestMocks::MockProcessActions actions;
    EXPECT_EQ(BrokerServer::serve(pair.server(), actions, std::chrono::milliseconds{0}, std::stop_token{}), BrokerServer::SessionEnd::Idle);
}

TEST(BrokerServeTest, AFrameCutShortEndsTheSessionAsBroken)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    std::vector<std::uint8_t> first(BrokerSocket::MAX_PACKET_BYTES, 0);
    first[2] = 0x10; // says 1 MiB follows
    ASSERT_EQ(::send(pair.client(), first.data(), first.size(), 0), static_cast<ssize_t>(first.size()));
    pair.closeClient();
    TestMocks::MockProcessActions actions;
    EXPECT_EQ(BrokerServer::serve(pair.server(), actions, std::chrono::seconds{10}, std::stop_token{}), BrokerServer::SessionEnd::Broken);
}

TEST(BrokerServeTest, AClientGoneBeforeItsReplyEndsTheSessionAsBroken)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    std::string error;
    ASSERT_TRUE(BrokerSocket::sendFrame(pair.client(), Broker::encodeRequest(1, Broker::Hello{}), error)) << error;
    pair.closeClient(); // the request is still there to read; the reply has nobody to go to
    TestMocks::MockProcessActions actions;
    EXPECT_EQ(BrokerServer::serve(pair.server(), actions, std::chrono::seconds{10}, std::stop_token{}), BrokerServer::SessionEnd::Broken);
}

TEST(BrokerSocketTest, AFrameTooLargeOrForAGonePeerIsNotSent)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    std::string error;
    const std::vector<std::uint8_t> huge(Broker::MAX_MESSAGE_BYTES + 1, 0);
    EXPECT_FALSE(BrokerSocket::sendFrame(pair.client(), huge, error));
    EXPECT_EQ(error, "a frame larger than the protocol allows");

    pair.closeServer();
    error.clear();
    EXPECT_FALSE(BrokerSocket::sendFrame(pair.client(), Broker::encodeRequest(1, Broker::Hello{}), error));
    EXPECT_FALSE(error.empty()); // EPIPE's message, never a SIGPIPE
}

TEST(BrokerSocketTest, APacketPastTheFramesLengthIsAFailure)
{
    const SocketPair pair;
    ASSERT_TRUE(pair.valid());
    // A full first packet whose length prefix says only 10 bytes more follow, then 20.
    std::vector<std::uint8_t> first(BrokerSocket::MAX_PACKET_BYTES, 0);
    const std::size_t declared = BrokerSocket::MAX_PACKET_BYTES + 10 - 4;
    first[0] = static_cast<std::uint8_t>(declared & 0xFFU);
    first[1] = static_cast<std::uint8_t>((declared >> 8U) & 0xFFU);
    first[2] = static_cast<std::uint8_t>((declared >> 16U) & 0xFFU);
    ASSERT_EQ(::send(pair.server(), first.data(), first.size(), 0), static_cast<ssize_t>(first.size()));
    const std::vector<std::uint8_t> second(20, 0);
    ASSERT_EQ(::send(pair.server(), second.data(), second.size(), 0), static_cast<ssize_t>(second.size()));
    const auto received = BrokerSocket::receiveFrame(pair.client(), REPLY_TIMEOUT, std::stop_token{});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerSocket::ReceiveEnd::Failed);
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
/// Its seccomp filter enforces (#2752) unless @p sandbox is null, so a call it doesn't list fails the test.
class HelperProcess
{
  public:
    explicit HelperProcess(int stdinFd, const char* argument = "--broker", const char* sandbox = HelperSandbox::ENFORCE_ARGUMENT)
    {
        posix_spawn_file_actions_t files{};
        posix_spawn_file_actions_init(&files);
        posix_spawn_file_actions_adddup2(&files, stdinFd, STDIN_FILENO);
        std::string path = TASKSMACK_HELPER_PATH;
        std::string arg = argument;
        std::string sandboxArg = sandbox != nullptr ? sandbox : "";
        std::array<char*, 4> argv{path.data(), arg.data(), sandbox != nullptr ? sandboxArg.data() : nullptr, nullptr};
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
    [[nodiscard]] pid_t pid() const
    {
        return m_Pid;
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

/// A forked child that sleeps until signalled, as @p runAs when given (the root tests' other user); reaped
/// on scope exit.
class SleepingChild
{
  public:
    explicit SleepingChild(std::optional<uid_t> runAs = std::nullopt) : m_Pid(::fork())
    {
        if (m_Pid == 0)
        {
            if (runAs.has_value() && (::setgroups(0, nullptr) != 0 || ::setgid(*runAs) != 0 || ::setuid(*runAs) != 0))
            {
                ::_exit(1);
            }
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

/// A field of /proc/<pid>/status ("CapEff", "NoNewPrivs"), or empty.
[[nodiscard]] std::string statusField(pid_t pid, std::string_view key)
{
    std::ifstream file("/proc/" + std::to_string(pid) + "/status");
    const std::string status((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return std::string(ProcStatusSecurity::fieldValue(status, key).value_or(""));
}

/// A capability set of /proc/<pid>/status as a mask; nullopt when unreadable.
[[nodiscard]] std::optional<std::uint64_t> capabilitySet(pid_t pid, std::string_view key)
{
    return ProcStatusSecurity::parseCapabilityMask(statusField(pid, key));
}

// #2640: the helper sets no_new_privs before it serves anything, root or not.
TEST(TasksmackHelperTest, SetsNoNewPrivsBeforeServing)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    HelperProcess helper(pair.server());
    ASSERT_TRUE(helper.started());
    pair.closeServer();
    const auto hello = roundTrip(pair.client(), 1, Broker::Hello{});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));
    EXPECT_EQ(statusField(helper.pid(), "NoNewPrivs"), "1");
    // And its seccomp filter is in (#2752): mode 2 (filter), one filter.
    EXPECT_EQ(statusField(helper.pid(), "Seccomp"), "2");
    EXPECT_EQ(statusField(helper.pid(), "Seccomp_filters"), "1");
    pair.closeClient();
    EXPECT_EQ(helper.exitCode(), 0);
}

// #2752: the default, as TaskSmack starts it, is the logging filter; it serves the same.
TEST(TasksmackHelperTest, ServesUnderTheLoggingFilterByDefault)
{
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    HelperProcess helper(pair.server(), "--broker", nullptr);
    ASSERT_TRUE(helper.started());
    pair.closeServer();
    const auto hello = roundTrip(pair.client(), 1, Broker::Hello{});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));
    EXPECT_EQ(statusField(helper.pid(), "Seccomp"), "2");
    pair.closeClient();
    EXPECT_EQ(helper.exitCode(), 0);
}

// #2752: becoming root isn't stopping being root. Asked to run as UID or GID 0, the drop fails before it
// changes anything -- this test process is left without no_new_privs.
TEST(HelperPrivilegesTest, RefusesToRunAsRoot)
{
    const std::string noNewPrivsBefore = statusField(::getpid(), "NoNewPrivs");
    EXPECT_EQ(HelperPrivileges::dropToKeptCapabilities({.uid = 0, .gid = 65534}), HelperPrivileges::DropResult::Failed);
    EXPECT_EQ(HelperPrivileges::dropToKeptCapabilities({.uid = 65534, .gid = 0}), HelperPrivileges::DropResult::Failed);
    EXPECT_EQ(statusField(::getpid(), "NoNewPrivs"), noNewPrivsBefore);
}

/// The user the root test's target runs as: neither root nor nobody, which the helper itself becomes
/// (#2752), so only its capabilities let it act on the target. No passwd entry is needed to setuid().
constexpr uid_t OTHER_USER = 61234;

// #2640, run as root only (`sudo build/debug/tests/TaskSmackTests --gtest_filter='TasksmackHelperRootTest.*'`):
// the helper started as root, as pkexec starts it, keeps only HelperPrivileges::KEPT_CAPABILITIES -- in
// every set, with no_new_privs -- and stops being root, running as nobody with no supplementary groups
// (#2752); that is enough for every request on another user's process.
TEST(TasksmackHelperRootTest, KeepsOnlyItsCapabilitiesAndServesEveryRequestOnAnotherUsersProcess)
{
    if (::geteuid() != 0)
    {
        GTEST_SKIP() << "Needs root: sudo build/debug/tests/TaskSmackTests --gtest_filter='TasksmackHelperRootTest.*'";
    }
    SleepingChild child(OTHER_USER);
    ASSERT_TRUE(child.started());
    ::usleep(100'000); // let it reach setuid()
    const ProcessTarget target = child.target();
    ASSERT_NE(target.startTimeTicks, 0U);
    ASSERT_EQ(statusField(target.pid, "Uid").substr(0, 5), "61234");

    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    HelperProcess helper(pair.server());
    ASSERT_TRUE(helper.started());
    pair.closeServer();
    std::uint32_t id = 1;
    const auto hello = roundTrip(pair.client(), id++, Broker::Hello{});
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello.message));

    // What it kept: only these, in every set, and it can't gain more.
    EXPECT_EQ(capabilitySet(helper.pid(), "CapEff"), HelperPrivileges::KEPT_CAPABILITIES);
    EXPECT_EQ(capabilitySet(helper.pid(), "CapPrm"), HelperPrivileges::KEPT_CAPABILITIES);
    EXPECT_EQ(capabilitySet(helper.pid(), "CapBnd"), HelperPrivileges::KEPT_CAPABILITIES);
    EXPECT_EQ(capabilitySet(helper.pid(), "CapInh"), 0U);
    EXPECT_EQ(capabilitySet(helper.pid(), "CapAmb"), 0U);
    EXPECT_EQ(statusField(helper.pid(), "NoNewPrivs"), "1");
    EXPECT_EQ(statusField(helper.pid(), "Seccomp"), "2"); // its enforcing filter (#2752)

    // Who it runs as (#2752): nobody, in every ID, with no supplementary groups.
    const std::string uid = "65534"; // nobody and nogroup, written out rather than taken from the helper's constant
    const std::string gid = "65534";
    EXPECT_EQ(statusField(helper.pid(), "Uid"), uid + '\t' + uid + '\t' + uid + '\t' + uid);
    EXPECT_EQ(statusField(helper.pid(), "Gid"), gid + '\t' + gid + '\t' + gid + '\t' + gid);
    EXPECT_EQ(statusField(helper.pid(), "Groups"), "");

    const auto expectAction = [&](const Broker::Request& request, const char* what)
    {
        const auto reply = roundTrip(pair.client(), id++, request);
        EXPECT_TRUE(actionResult(reply.message).success) << what << ": " << actionResult(reply.message).errorMessage;
    };
    expectAction(Broker::PriorityRequest{.target = target, .nice = 10}, "lower its priority");
    expectAction(Broker::PriorityRequest{.target = target, .nice = -5}, "raise its priority");
    expectAction(Broker::IoPriorityRequest{.target = target, .ioClass = IoPriorityClass::Realtime, .level = 3}, "Realtime I/O");
    expectAction(Broker::IoPriorityRequest{.target = target, .ioClass = IoPriorityClass::Idle, .level = 0}, "Idle I/O");
    expectAction(Broker::AffinityRequest{.target = target, .cpus = CpuAffinity::fromMask(1)}, "affinity");
    expectAction(Broker::SignalRequest{.target = target, .signal = Broker::Signal::Suspend}, "suspend");
    expectAction(Broker::SignalRequest{.target = target, .signal = Broker::Signal::Resume}, "resume");

    const auto environment = roundTrip(pair.client(), id++, Broker::EnvironmentRequest{.target = target});
    ASSERT_TRUE(std::holds_alternative<Broker::EnvironmentReply>(environment.message));
    EXPECT_EQ(std::get<Broker::EnvironmentReply>(environment.message).result.status, EnvironmentReadStatus::Ok);
    const auto modules = roundTrip(pair.client(), id++, Broker::ModulesRequest{.target = target});
    ASSERT_TRUE(std::holds_alternative<Broker::ModulesReply>(modules.message));
    EXPECT_EQ(std::get<Broker::ModulesReply>(modules.message).result.status, ModulesReadStatus::Ok);
    EXPECT_FALSE(std::get<Broker::ModulesReply>(modules.message).result.modules.empty());
    const auto files = roundTrip(pair.client(), id++, Broker::OpenFilesRequest{.target = target});
    ASSERT_TRUE(std::holds_alternative<Broker::OpenFilesReply>(files.message));
    EXPECT_EQ(std::get<Broker::OpenFilesReply>(files.message).result.status, OpenFilesReadStatus::Ok);
    EXPECT_FALSE(std::get<Broker::OpenFilesReply>(files.message).result.files.empty());
    const auto connections = roundTrip(pair.client(), id++, Broker::ConnectionsRequest{.target = target});
    ASSERT_TRUE(std::holds_alternative<Broker::ConnectionsReply>(connections.message));
    EXPECT_EQ(std::get<Broker::ConnectionsReply>(connections.message).result.status, ConnectionsReadStatus::Ok);
    const auto rollup = roundTrip(pair.client(), id++, Broker::MemoryRollupRequest{.target = target});
    ASSERT_TRUE(std::holds_alternative<Broker::MemoryRollupReply>(rollup.message));
    const MemoryRollupRead& rollupRead = std::get<Broker::MemoryRollupReply>(rollup.message).result;
    EXPECT_TRUE(rollupRead.has_value() || rollupRead.error() == MemoryRollupFailure::Unavailable); // Unavailable: no smaps_rollup
    const auto firmware = roundTrip(pair.client(), id++, Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::SmbiosTable});
    ASSERT_TRUE(std::holds_alternative<Broker::FirmwareFileReply>(firmware.message));
    EXPECT_NE(std::get<Broker::FirmwareFileReply>(firmware.message).status, Broker::FirmwareFileStatus::Unreadable); // NotFound: no DMI
    const auto crashes = roundTrip(pair.client(), id++, Broker::CrashesRequest{});
    ASSERT_TRUE(std::holds_alternative<Broker::CrashesReply>(crashes.message));
    EXPECT_FALSE(std::get<Broker::CrashesReply>(crashes.message).info.accessDenied)
        << std::get<Broker::CrashesReply>(crashes.message).info.unavailableReason;

    expectAction(Broker::SignalRequest{.target = target, .signal = Broker::Signal::Kill}, "kill");
    EXPECT_EQ(child.terminatingSignal(), SIGKILL);
    pair.closeClient();
    EXPECT_EQ(helper.exitCode(), 0);
}

TEST(PkexecBrokerTest, ConnectsToTheHelperAndTerminatesAProcessThroughIt)
{
    SleepingChild child;
    ASSERT_TRUE(child.started());
    const ProcessTarget target = child.target();
    ASSERT_NE(target.startTimeTicks, 0U);

    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce); // the helper itself, without pkexec
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
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
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
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
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

TEST(PkexecBrokerTest, TheHelperListsAProcesssConnections)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const auto response = broker.call(Broker::ConnectionsRequest{.target = child.target()}, std::stop_token{});
    ASSERT_TRUE(response.has_value()) << response.error();
    ASSERT_TRUE(std::holds_alternative<Broker::ConnectionsReply>(*response));
    EXPECT_EQ(std::get<Broker::ConnectionsReply>(*response).result.status, ConnectionsReadStatus::Ok); // none open, but read
}

// #2752: a process with a socket open, read under the helper's enforcing filter, which allows it no
// netlink socket: every table comes from /proc/<pid>/net, and the listener is there.
TEST(PkexecBrokerTest, TheHelperListsAListeningSocketUnderItsFilter)
{
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
              0); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - sockets API
    ASSERT_EQ(::listen(listener, 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length),
              0); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - sockets API
    const std::uint16_t port = ntohs(address.sin_port);

    std::ifstream stat("/proc/self/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const ProcessTarget self{.pid = static_cast<std::int32_t>(::getpid()),
                             .startTimeTicks = ProcParsing::parseStatStartTime(line).value_or(0)};

    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const auto response = broker.call(Broker::ConnectionsRequest{.target = self}, std::stop_token{});
    ::close(listener);
    ASSERT_TRUE(response.has_value()) << response.error();
    ASSERT_TRUE(std::holds_alternative<Broker::ConnectionsReply>(*response));
    const ConnectionsReadResult& result = std::get<Broker::ConnectionsReply>(*response).result;
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok) << result.detail;
    EXPECT_TRUE(std::ranges::any_of(result.connections,
                                    [port](const ProcessConnection& connection)
                                    { return connection.state == ConnectionState::Listen && connection.local.port == port; }))
        << "port " << port;
    EXPECT_TRUE(broker.connected()) << "the read didn't kill the helper";
}

TEST(PkexecBrokerTest, TheHelperReadsAProcesssPssAndUss)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const auto response = broker.call(Broker::MemoryRollupRequest{.target = child.target()}, std::stop_token{});
    ASSERT_TRUE(response.has_value()) << response.error();
    ASSERT_TRUE(std::holds_alternative<Broker::MemoryRollupReply>(*response));
    const MemoryRollupRead& rollup = std::get<Broker::MemoryRollupReply>(*response).result;
    if (!rollup.has_value() && rollup.error() == MemoryRollupFailure::Unavailable)
    {
        GTEST_SKIP() << "This kernel has no smaps_rollup";
    }
    ASSERT_TRUE(rollup.has_value());
    EXPECT_GT(rollup.value_or(MemoryRollup{}).pssBytes, 0U);
}

// #2728: the sampler's batch of refused FD counts and I/O, through the real helper; a target whose start
// time doesn't match is left out.
TEST(PkexecBrokerTest, TheHelperReadsProcessCountersAndLeavesOutAStaleTarget)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const ProcessTarget stale{.pid = child.target().pid, .startTimeTicks = child.target().startTimeTicks + 1};
    const auto response = broker.call(Broker::ProcessCountersRequest{.targets = {child.target(), stale}}, std::stop_token{});
    ASSERT_TRUE(response.has_value()) << response.error();
    ASSERT_TRUE(std::holds_alternative<Broker::ProcessCountersReply>(*response));
    const auto& entries = std::get<Broker::ProcessCountersReply>(*response).entries;
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().target, child.target());
    EXPECT_TRUE(entries.front().handleCountRead);
    EXPECT_GT(entries.front().handleCount, 0) << "its stdin, stdout and stderr at least";
    // A kernel without task I/O accounting (WSL's) has no /proc/[pid]/io to read.
    EXPECT_EQ(entries.front().ioRead, std::filesystem::exists("/proc/self/io"));
}

TEST(PkexecBrokerTest, TheHelperAnswersAFirmwareFileRequest)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const auto response = broker.call(Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::SmbiosTable}, std::stop_token{});
    ASSERT_TRUE(response.has_value()) << response.error();
    // Unprivileged here, so Unreadable where the table exists; NotFound in a VM or container without one
    ASSERT_TRUE(std::holds_alternative<Broker::FirmwareFileReply>(*response));
}

TEST(PkexecBrokerTest, TheHelperAnswersACrashesRequest)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const auto response = broker.call(Broker::CrashesRequest{}, std::stop_token{});
    ASSERT_TRUE(response.has_value()) << response.error();
    ASSERT_TRUE(std::holds_alternative<Broker::CrashesReply>(*response));
    EXPECT_TRUE(std::get<Broker::CrashesReply>(*response).info.available);
}

TEST(PkexecBrokerTest, ACallIfIdleIsAnswered)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const auto response = broker.callIfIdle(Broker::MemoryRollupRequest{.target = child.target()}, std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value()) << response.error();
    EXPECT_TRUE(std::holds_alternative<Broker::MemoryRollupReply>(*response));
}

TEST(PkexecBrokerTest, HoldingTheSessionStartsTheHelperOnce)
{
    const SleepingChild child;
    ASSERT_TRUE(child.started());
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {});
    EXPECT_FALSE(broker.heldForSession());
    const auto held = broker.holdForSession(std::stop_token{});
    ASSERT_TRUE(held.has_value()) << held.error();
    EXPECT_TRUE(broker.connected());
    EXPECT_TRUE(broker.heldForSession());
    EXPECT_TRUE(broker.holdForSession(std::stop_token{}).has_value()); // already held: a Hello again
    EXPECT_TRUE(std::holds_alternative<Broker::MemoryRollupReply>(
        broker.call(Broker::MemoryRollupRequest{.target = child.target()}, std::stop_token{}).value_or(Broker::ErrorReply{})));
    broker.disconnect();
    EXPECT_FALSE(broker.heldForSession()); // nothing to hold once the helper is gone
}

TEST(PkexecBrokerTest, AConnectedHelperIsHeldWithoutAnotherStart)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {});
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    EXPECT_FALSE(broker.heldForSession());
    const auto held = broker.holdForSession(std::stop_token{});
    ASSERT_TRUE(held.has_value()) << held.error();
    EXPECT_TRUE(broker.heldForSession());
}

TEST(PkexecBrokerTest, AHoldThatCantStartTheHelperLeavesTheSessionUnheld)
{
    PkexecBroker broker("/nonexistent/tasksmack-helper", {});
    EXPECT_FALSE(broker.holdForSession(std::stop_token{}).has_value());
    EXPECT_FALSE(broker.heldForSession());
    EXPECT_FALSE(broker.connected());
}

TEST(PkexecBrokerTest, ACallWithoutAHelperSaysSo)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    const auto response = broker.call(Broker::Hello{}, std::stop_token{});
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error(), "not connected");
    EXPECT_FALSE(actionResultOf(response).success);
}

TEST(PkexecBrokerTest, DisconnectEndsTheSessionAndConnectStartsAnother)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    broker.disconnect();
    EXPECT_FALSE(broker.connected());
    EXPECT_FALSE(broker.call(Broker::Hello{}, std::stop_token{}).has_value());
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const auto hello = broker.call(Broker::Hello{}, std::stop_token{});
    ASSERT_TRUE(hello.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::HelloReply>(*hello));
}

/// This process's child running tasksmack-helper, or -1: the helper PkexecBroker started with no launcher.
[[nodiscard]] pid_t helperChild()
{
    for (const auto& entry : std::filesystem::directory_iterator("/proc"))
    {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
        {
            continue;
        }
        std::ifstream statFile(entry.path() / "stat");
        const std::string stat((std::istreambuf_iterator<char>(statFile)), std::istreambuf_iterator<char>());
        const std::size_t close = stat.rfind(')');
        if (close == std::string::npos || !stat.contains("(tasksmack-help"))
        {
            continue;
        }
        int ppid = 0;
        char state = 0;
        if (std::sscanf(stat.c_str() + close + 1, " %c %d", &state, &ppid) == 2 && ppid == ::getpid() && state != 'Z')
        {
            return static_cast<pid_t>(std::stoi(name));
        }
    }
    return -1;
}

// #2368: the helper also ends on its own (its idle exit, a crash); connected() must then say so, so a
// refused action is offered "Authenticate and retry" rather than sent to a socket nobody reads.
TEST(PkexecBrokerTest, AHelperThatExitsIsNoLongerConnected)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    ASSERT_TRUE(broker.connected());
    const pid_t helper = helperChild();
    ASSERT_GT(helper, 0);
    ASSERT_EQ(::kill(helper, SIGKILL), 0);
    bool gone = false;
    for (int attempt = 0; attempt < 200 && !gone; ++attempt) // up to 2 s for the kernel to close its end
    {
        gone = !broker.connected();
        if (!gone)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    EXPECT_TRUE(gone);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value()) << "a fresh helper starts";
    EXPECT_TRUE(broker.connected());
    const auto reply = broker.call(Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::ProductUuid}, std::stop_token{});
    EXPECT_TRUE(reply.has_value());
}

// #2449: a helper that ended on its own is reaped once connected() sees it gone, not left this process's
// zombie until the next connect(), which may never come.
TEST(PkexecBrokerTest, AHelperThatExitsIsReapedWithoutAnotherConnect)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {}, HelperSandbox::Mode::Enforce);
    ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
    const pid_t helper = helperChild();
    ASSERT_GT(helper, 0);
    ASSERT_EQ(::kill(helper, SIGKILL), 0);
    bool reaped = false;
    for (int attempt = 0; attempt < 200 && !reaped; ++attempt) // up to 2 s
    {
        static_cast<void>(broker.connected());
        // A zombie still takes signal 0; only a reaped process is gone (ESRCH).
        reaped = ::kill(helper, 0) == -1 && errno == ESRCH;
        if (!reaped)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    EXPECT_TRUE(reaped);
    EXPECT_FALSE(broker.connected());
}

// #2448 (#2479): the password prompt polkit shows for the helper says what the session allows -- reading
// other users' process details (their environment above all) and protected system information, not only
// acting on processes.
TEST(HelperPolicyTest, ThePromptSaysTheSessionAlsoReads)
{
    std::ifstream file(std::string(TASKSMACK_SOURCE_ASSETS_DIR) + "/linux/app.tasksmack.TaskSmack.helper.policy");
    ASSERT_TRUE(file.is_open());
    const std::string policy((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto element = [&policy](std::string_view tag) -> std::string
    {
        const std::string open = "<" + std::string(tag) + ">";
        const std::size_t start = policy.find(open);
        const std::size_t end = policy.find("</" + std::string(tag) + ">", start);
        return start == std::string::npos || end == std::string::npos ? std::string{}
                                                                      : policy.substr(start + open.size(), end - start - open.size());
    };
    const std::string message = element("message");
    const std::string description = element("description");
    EXPECT_TRUE(message.contains("read")) << message;
    EXPECT_TRUE(message.contains("environment")) << message;
    EXPECT_TRUE(message.contains("system information")) << message;
    EXPECT_TRUE(description.contains("read")) << description;
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

// #2370: a launcher still prompting when the broker stops waiting is ended, not left with its prompt up.
TEST(PkexecBrokerTest, AStartGivenUpOnEndsTheLauncher)
{
    {
        // A launcher that would sit "prompting" for half a minute, ignoring its closed socket
        PkexecBroker broker(TASKSMACK_HELPER_PATH, {"sh", "-c", "exec sleep 30", "sh"});
        const auto connected = broker.connectWithin(std::stop_token{}, std::chrono::milliseconds{200});
        ASSERT_FALSE(connected.has_value());
        EXPECT_EQ(connected.error(), "the helper didn't answer in time");
    }
    // Reaped by the broker's destructor, which waits about a second: only because it was ended
    errno = 0;
    EXPECT_EQ(::waitpid(-1, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

// #2391: a second start while the first is still prompting says so at once, rather than wait out (and
// then repeat) the prompt.
TEST(PkexecBrokerTest, ASecondStartWhileOneIsPromptingSaysSo)
{
    PkexecBroker broker(TASKSMACK_HELPER_PATH, {"sh", "-c", "exec sleep 30", "sh"}); // "prompting" until ended
    std::stop_source stop;
    auto first = std::async(std::launch::async, [&] { return broker.connectWithin(stop.get_token(), std::chrono::seconds{20}); });
    std::this_thread::sleep_for(std::chrono::milliseconds{300}); // the first is now waiting for its answer
    const auto started = std::chrono::steady_clock::now();
    const auto second = broker.connectWithin(std::stop_token{}, std::chrono::seconds{20});
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{2});
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), "already asking for an administrator's password");
    stop.request_stop();
    const auto cancelled = first.get();
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error(), "cancelled");
}

// --- #2417: a launcher that plays the helper, to answer the handshake and calls wrongly ----------------

/// The sh command that sends @p frame from the scripted helper below, as one packet.
[[nodiscard]] std::string sendScript(const std::vector<std::uint8_t>& frame)
{
    std::string script = "printf '";
    for (const std::uint8_t byte : frame)
    {
        script += std::format("\\{:03o}", byte);
    }
    script += "' >&0; ";
    return script;
}

/// A launcher (sh) that plays the helper: for each of @p replies it takes one request off its socket (its
/// stdin) and answers with that frame, then runs @p then. The helper's path and --broker, which
/// PkexecBroker appends, are the script's unused $1 and $2. A script that ends with `exec cat` keeps the
/// socket open until the broker closes it, and then exits, so the broker reaps it.
[[nodiscard]] std::vector<std::string> scriptedHelper(const std::vector<std::vector<std::uint8_t>>& replies, std::string_view then)
{
    std::string script;
    for (const std::vector<std::uint8_t>& reply : replies)
    {
        // One read takes one whole SEQPACKET packet: the request, whatever its size.
        script += "head -c 1 >/dev/null; " + sendScript(reply);
    }
    script += then;
    return {"sh", "-c", script, "sh"};
}

/// The frame of a handshake reply to the broker's first request (its ids start at 1).
[[nodiscard]] std::vector<std::uint8_t> helloReply(std::uint16_t version = Broker::PROTOCOL_VERSION, std::uint32_t id = 1)
{
    return Broker::encodeResponse(id, Broker::HelloReply{.version = version, .accepted = version == Broker::PROTOCOL_VERSION});
}

/// No child of this process is left for anything that waits for any child (DetachedSpawnTest, #2333).
void expectNoChildLeft()
{
    errno = 0;
    EXPECT_EQ(::waitpid(-1, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(PkexecBrokerTest, AHelperOfAnotherVersionIsRefused)
{
    {
        const auto otherVersion = static_cast<std::uint16_t>(Broker::PROTOCOL_VERSION + 1);
        PkexecBroker broker(TASKSMACK_HELPER_PATH, scriptedHelper({helloReply(otherVersion)}, "exec cat >/dev/null"));
        const auto connected = broker.connect(std::stop_token{});
        ASSERT_FALSE(connected.has_value());
        EXPECT_EQ(connected.error(),
                  std::format("the helper speaks protocol version {}, TaskSmack {}", otherVersion, Broker::PROTOCOL_VERSION));
        EXPECT_FALSE(broker.connected());
    }
    expectNoChildLeft();
}

TEST(PkexecBrokerTest, AHandshakeAnsweredWithSomethingElseIsRefused)
{
    {
        const std::vector<std::uint8_t> notHello =
            Broker::encodeResponse(1, Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}});
        PkexecBroker broker(TASKSMACK_HELPER_PATH, scriptedHelper({notHello}, "exec cat >/dev/null"));
        const auto connected = broker.connect(std::stop_token{});
        ASSERT_FALSE(connected.has_value());
        EXPECT_EQ(connected.error(), "the helper didn't answer the handshake");
    }
    expectNoChildLeft();
}

TEST(PkexecBrokerTest, AnAnswerToAnotherRequestOrThatDoesntDecodeIsRefused)
{
    {
        // The launcher keeps running, so there is no exit status to report instead (it is waited for
        // about a second first).
        PkexecBroker broker(TASKSMACK_HELPER_PATH, scriptedHelper({helloReply(Broker::PROTOCOL_VERSION, 2)}, "exec cat >/dev/null"));
        const auto connected = broker.connect(std::stop_token{});
        ASSERT_FALSE(connected.has_value());
        EXPECT_EQ(connected.error(), "the helper answered another request");
    }
    {
        PkexecBroker broker(TASKSMACK_HELPER_PATH, scriptedHelper({std::vector<std::uint8_t>{1, 2, 3}}, "exec cat >/dev/null"));
        const auto connected = broker.connect(std::stop_token{});
        ASSERT_FALSE(connected.has_value());
        EXPECT_EQ(connected.error(), "the helper's answer didn't decode: A frame shorter than its header");
    }
    expectNoChildLeft();
}

TEST(PkexecBrokerTest, ALauncherThatEndsSaysHow)
{
    struct Case
    {
        std::string_view script;
        std::string_view reason;
    };
    const std::array cases{Case{.script = "exit 3", .reason = "the helper exited with status 3"},
                           Case{.script = "exit 127", .reason = "not authorised, or pkexec isn't available"},
                           Case{.script = "kill -KILL $$", .reason = "the helper was ended by a signal"}};
    for (const Case& c : cases)
    {
        PkexecBroker broker(TASKSMACK_HELPER_PATH, {"sh", "-c", std::string(c.script), "sh"});
        const auto connected = broker.connect(std::stop_token{});
        ASSERT_FALSE(connected.has_value()) << c.script;
        EXPECT_EQ(connected.error(), c.reason) << c.script;
    }
    expectNoChildLeft();
}

TEST(PkexecBrokerTest, AHelperThatExitsMidCallEndsTheSession)
{
    {
        // Answers the handshake, takes the next request, and exits without answering it.
        PkexecBroker broker(TASKSMACK_HELPER_PATH, scriptedHelper({helloReply()}, "head -c 1 >/dev/null"));
        ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
        const auto response = broker.call(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}, std::stop_token{});
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(response.error(), "the helper exited");
        EXPECT_FALSE(broker.connected()) << "a channel that failed once isn't used again";
    }
    expectNoChildLeft();
}

TEST(PkexecBrokerTest, ACallIfIdleNeedsAHelperAndAFailedOneEndsTheSession)
{
    {
        PkexecBroker broker(TASKSMACK_HELPER_PATH, scriptedHelper({helloReply()}, "head -c 1 >/dev/null"));
        const auto unconnected = broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, std::chrono::seconds{2});
        ASSERT_FALSE(unconnected.has_value());
        EXPECT_EQ(unconnected.error(), "not connected");

        ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
        const auto response = broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, std::chrono::seconds{10});
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(response.error(), "the helper exited");
        EXPECT_FALSE(broker.connected());
    }
    expectNoChildLeft();
}

TEST(PkexecBrokerTest, AStoppedCallIsCancelledAndEndsTheSession)
{
    {
        PkexecBroker broker(TASKSMACK_HELPER_PATH, scriptedHelper({helloReply()}, "exec cat >/dev/null"));
        ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());
        std::stop_source stop;
        stop.request_stop();
        const auto response = broker.call(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}, stop.get_token());
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(response.error(), "cancelled");
        EXPECT_FALSE(broker.connected());
    }
    expectNoChildLeft();
}

// --- #2406: a PSS/USS read slower than callIfIdle()'s timeout skips a sample, not the session -----------

/// The sh command that takes one request off the scripted helper's socket without answering it.
constexpr std::string_view TAKE_REQUEST = "head -c 1 >/dev/null; ";

/// A file the scripted helper waits for, or makes, so a test orders its steps without sleeping on a guess.
class Marker
{
  public:
    explicit Marker(std::string_view name)
        : m_Path(std::filesystem::temp_directory_path() / std::format("tasksmack-test-{}-{}", ::getpid(), name))
    {
        std::error_code ignored;
        std::filesystem::remove(m_Path, ignored);
    }
    Marker(const Marker&) = delete;
    Marker& operator=(const Marker&) = delete;
    Marker(Marker&&) = delete;
    Marker& operator=(Marker&&) = delete;
    ~Marker()
    {
        std::error_code ignored;
        std::filesystem::remove(m_Path, ignored);
    }

    [[nodiscard]] std::string path() const
    {
        return m_Path.string();
    }
    void make() const
    {
        const std::ofstream file(m_Path);
    }
    /// Waits, bounded, for the scripted helper to make it.
    [[nodiscard]] bool awaitMade() const
    {
        const auto deadline = std::chrono::steady_clock::now() + REPLY_TIMEOUT;
        while (!std::filesystem::exists(m_Path))
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return true;
    }

  private:
    std::filesystem::path m_Path;
};

[[nodiscard]] std::vector<std::uint8_t> rollupReply(std::uint32_t id, std::uint64_t pssBytes)
{
    return Broker::encodeResponse(
        id, Broker::MemoryRollupReply{.result = MemoryRollup{.pssBytes = pssBytes, .ussBytes = 1, .swapPssBytes = 0}});
}

[[nodiscard]] std::uint64_t pssOf(const std::expected<Broker::Response, std::string>& response)
{
    if (!response.has_value())
    {
        ADD_FAILURE() << response.error();
        return 0;
    }
    const auto* const reply = std::get_if<Broker::MemoryRollupReply>(&*response);
    if (reply == nullptr || !reply->result.has_value())
    {
        ADD_FAILURE() << "not a PSS/USS reading";
        return 0;
    }
    return reply->result->pssBytes;
}

TEST(PkexecBrokerTest, ACallIfIdleThatTimesOutKeepsTheSessionAndItsLateReplyIsDropped)
{
    {
        // Takes the read (2) and doesn't answer it until the next request (3) is in; then answers both, in
        // order, and then the read after them (4).
        const std::vector<std::uint8_t> actionReply =
            Broker::encodeResponse(3, Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = "the call's own"});
        const std::string then = std::format("{0}{0}{1}{2}{0}{3}exec cat >/dev/null",
                                             TAKE_REQUEST,
                                             sendScript(rollupReply(2, 222)),
                                             sendScript(actionReply),
                                             sendScript(rollupReply(4, 444)));
        PkexecBroker broker(TASKSMACK_HELPER_PATH, scriptedHelper({helloReply()}, then));
        ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());

        const auto slow = broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, std::chrono::milliseconds{100});
        ASSERT_FALSE(slow.has_value());
        EXPECT_EQ(slow.error(), "the helper didn't answer in time");
        EXPECT_TRUE(broker.connected()) << "a slow read skips a sample; it doesn't end the session";

        // The helper is still on that read: another would only queue behind it.
        const auto skipped = broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, std::chrono::milliseconds{100});
        ASSERT_FALSE(skipped.has_value());
        EXPECT_EQ(skipped.error(), "busy");
        EXPECT_TRUE(broker.connected());

        const auto action = broker.call(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}, std::stop_token{});
        ASSERT_TRUE(action.has_value()) << action.error();
        EXPECT_EQ(actionResultOf(action).successNote, "the call's own") << "the call gets its own reply, not the late one";

        EXPECT_EQ(pssOf(broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, std::chrono::seconds{10})), 444U);
        EXPECT_TRUE(broker.connected());
    }
    expectNoChildLeft();
}

TEST(PkexecBrokerTest, ALateReplyThatHasComeIsTakenBeforeTheNextCallIfIdle)
{
    {
        const Marker answer("2406-answer");
        // Takes the read (2), answers it once the test says so (or gives up waiting, should the test end
        // first), then answers the next read (3).
        const std::string then =
            std::format("{0}i=0; while [ ! -e '{1}' ] && [ $i -lt 1000 ]; do sleep 0.01; i=$((i+1)); done; {2}{0}{3}exec cat >/dev/null",
                        TAKE_REQUEST,
                        answer.path(),
                        sendScript(rollupReply(2, 222)),
                        sendScript(rollupReply(3, 333)));
        PkexecBroker broker(TASKSMACK_HELPER_PATH, scriptedHelper({helloReply()}, then));
        ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());

        ASSERT_FALSE(broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, std::chrono::milliseconds{100}).has_value());
        ASSERT_TRUE(broker.connected());
        answer.make();

        // "busy" until the late reply has come; then it is taken, and the next read is asked and answered.
        std::expected<Broker::Response, std::string> next = std::unexpected(std::string("busy"));
        const auto deadline = std::chrono::steady_clock::now() + REPLY_TIMEOUT;
        while (!next.has_value() && next.error() == "busy" && std::chrono::steady_clock::now() < deadline)
        {
            next = broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, std::chrono::seconds{10});
            if (!next.has_value() && next.error() == "busy")
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        }
        EXPECT_EQ(pssOf(next), 333U);
        EXPECT_TRUE(broker.connected());
    }
    expectNoChildLeft();
}

TEST(PkexecBrokerTest, ACallIfIdleWhileACallIsWaitingIsBusy)
{
    {
        const Marker taken("2406-taken");
        // Takes the call's request, says so, and never answers it.
        PkexecBroker broker(TASKSMACK_HELPER_PATH,
                            scriptedHelper({helloReply()}, std::format("{}: > '{}'; exec cat >/dev/null", TAKE_REQUEST, taken.path())));
        ASSERT_TRUE(broker.connect(std::stop_token{}).has_value());

        std::stop_source stop;
        auto calling = std::async(std::launch::async,
                                  [&broker, token = stop.get_token()]
                                  { return broker.call(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}, token); });
        ASSERT_TRUE(taken.awaitMade());
        const auto response = broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, std::chrono::seconds{10});
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(response.error(), "busy") << "never queued behind the call";

        stop.request_stop();
        const auto called = calling.get();
        ASSERT_FALSE(called.has_value());
        EXPECT_EQ(called.error(), "cancelled");
    }
    expectNoChildLeft();
}

// NOLINTEND(misc-include-cleaner)

#endif // TASKSMACK_HELPER_PATH

} // namespace
} // namespace Platform
