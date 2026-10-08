/// @file test_LinuxProcessConnectionsReader.cpp
/// @brief Platform::LinuxProcessConnectionsReader against real sockets (#799): sockets this test opens
/// -- a TCP listener, an established loopback pair, a bound UDP socket, an IPv6 listener -- are found
/// for getpid() with their endpoints and states, through netlink and through the /proc/[pid]/net
/// fallback alike; a table netlink refuses is read from /proc instead; and a PID with no process, a
/// start time that does not match (a reused PID), an unknown start time and another user's process
/// each give their status.

#include "Platform/Factory.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/Linux/LinuxProcessConnectionsReader.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/NetlinkTestUtils.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - POSIX socket headers: include-cleaner lacks mappings for sockaddr_in, htons, ...
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/inet_diag.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform
{
namespace
{

/// The start time of @p pid as the probe reports it, or 0 if it cannot be read.
[[nodiscard]] std::uint64_t startTicksOf(pid_t pid)
{
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    return ProcParsing::parseStatStartTime(line).value_or(0);
}

[[nodiscard]] ProcessTarget self()
{
    return {.pid = static_cast<std::int32_t>(::getpid()), .startTimeTicks = startTicksOf(::getpid())};
}

/// A socket descriptor closed on scope exit.
class Socket
{
  public:
    explicit Socket(int fd) : m_Fd(fd)
    {}
    ~Socket()
    {
        if (m_Fd >= 0)
        {
            ::close(m_Fd);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : m_Fd(std::exchange(other.m_Fd, -1))
    {}
    Socket& operator=(Socket&&) = delete;

    [[nodiscard]] int fd() const
    {
        return m_Fd;
    }

    /// The port the socket is bound to (host order), 0 on failure.
    [[nodiscard]] std::uint16_t localPort() const
    {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        if (::getsockname(m_Fd, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        {
            return 0;
        }
        if (address.ss_family == AF_INET6)
        {
            sockaddr_in6 v6{};
            std::memcpy(&v6, &address, sizeof(v6));
            return ntohs(v6.sin6_port);
        }
        sockaddr_in v4{};
        std::memcpy(&v4, &address, sizeof(v4));
        return ntohs(v4.sin_port);
    }

  private:
    int m_Fd = -1;
};

/// A socket of @p type bound to 127.0.0.1 on a free port (listening, for TCP); fd -1 on failure.
[[nodiscard]] Socket loopbackSocket(int type)
{
    Socket socket(::socket(AF_INET, type | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (socket.fd() < 0 || ::bind(socket.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        return Socket(-1);
    }
    if (type == SOCK_STREAM && ::listen(socket.fd(), 4) != 0)
    {
        return Socket(-1);
    }
    return socket;
}

/// A TCP client connected to 127.0.0.1:@p port; fd -1 on failure.
[[nodiscard]] Socket connectTo(std::uint16_t port)
{
    Socket socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (socket.fd() < 0 || ::connect(socket.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        return Socket(-1);
    }
    return socket;
}

constexpr std::array<std::uint8_t, 16> LOOPBACK4{127, 0, 0, 1};

/// The row of @p result whose local port is @p port and protocol @p protocol, or null.
[[nodiscard]] const ProcessConnection* findLocal(const ConnectionsReadResult& result, ConnectionProtocol protocol, std::uint16_t port)
{
    const auto it = std::ranges::find_if(result.connections,
                                         [&](const ProcessConnection& c) { return c.protocol == protocol && c.local.port == port; });
    return (it != result.connections.end()) ? &*it : nullptr;
}

/// Both readers under test: netlink (with the /proc fallback) and /proc alone.
[[nodiscard]] std::vector<std::pair<const char*, std::unique_ptr<LinuxProcessConnectionsReader>>> readers()
{
    std::vector<std::pair<const char*, std::unique_ptr<LinuxProcessConnectionsReader>>> all;
    all.emplace_back("netlink", std::make_unique<LinuxProcessConnectionsReader>());
    all.emplace_back("/proc/[pid]/net", std::make_unique<LinuxProcessConnectionsReader>(nullptr));
    return all;
}

TEST(LinuxProcessConnectionsReaderTest, ReportsSupport)
{
    const LinuxProcessConnectionsReader reader;
    EXPECT_TRUE(reader.hasConnections());
    const auto fromFactory = makeProcessConnectionsReader();
    ASSERT_NE(fromFactory, nullptr);
    EXPECT_TRUE(fromFactory->hasConnections());
}

TEST(LinuxProcessConnectionsReaderTest, FindsItsOwnTcpListenerAndEstablishedPair)
{
    const Socket listener = loopbackSocket(SOCK_STREAM);
    ASSERT_GE(listener.fd(), 0);
    const std::uint16_t port = listener.localPort();
    ASSERT_NE(port, 0);
    const Socket client = connectTo(port);
    ASSERT_GE(client.fd(), 0);
    const Socket accepted(::accept4(listener.fd(), nullptr, nullptr, SOCK_CLOEXEC));
    ASSERT_GE(accepted.fd(), 0);
    const std::uint16_t clientPort = client.localPort();

    for (auto& [name, reader] : readers())
    {
        SCOPED_TRACE(name);
        const ConnectionsReadResult result = reader->readConnections(self());
        ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);

        const ProcessConnection* const listening = findLocal(result, ConnectionProtocol::Tcp, port);
        ASSERT_NE(listening, nullptr);
        // The listener and the accepted socket share the local port: find the listener by state.
        const auto listenerRow = std::ranges::find_if(
            result.connections, [&](const ProcessConnection& c) { return c.local.port == port && c.state == ConnectionState::Listen; });
        ASSERT_NE(listenerRow, result.connections.end());
        EXPECT_EQ(listenerRow->family, ConnectionFamily::IPv4);
        EXPECT_EQ(listenerRow->local.address, LOOPBACK4);
        EXPECT_EQ(listenerRow->remote.port, 0);

        // The client end: 127.0.0.1:clientPort -> 127.0.0.1:port, established.
        const ProcessConnection* const clientRow = findLocal(result, ConnectionProtocol::Tcp, clientPort);
        ASSERT_NE(clientRow, nullptr);
        EXPECT_EQ(clientRow->state, ConnectionState::Established);
        EXPECT_EQ(clientRow->remote.address, LOOPBACK4);
        EXPECT_EQ(clientRow->remote.port, port);

        // The accepted end: 127.0.0.1:port -> 127.0.0.1:clientPort, established.
        const auto acceptedRow = std::ranges::find_if(
            result.connections, [&](const ProcessConnection& c) { return c.local.port == port && c.remote.port == clientPort; });
        ASSERT_NE(acceptedRow, result.connections.end());
        EXPECT_EQ(acceptedRow->state, ConnectionState::Established);
    }
}

TEST(LinuxProcessConnectionsReaderTest, FindsItsOwnUnconnectedUdpSocket)
{
    const Socket udp = loopbackSocket(SOCK_DGRAM);
    ASSERT_GE(udp.fd(), 0);
    const std::uint16_t port = udp.localPort();
    ASSERT_NE(port, 0);

    for (auto& [name, reader] : readers())
    {
        SCOPED_TRACE(name);
        const ConnectionsReadResult result = reader->readConnections(self());
        ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
        const ProcessConnection* const row = findLocal(result, ConnectionProtocol::Udp, port);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->family, ConnectionFamily::IPv4);
        EXPECT_EQ(row->local.address, LOOPBACK4);
        EXPECT_EQ(row->state, ConnectionState::Closed); // bound, not connected: shown as UNCONN
    }
}

TEST(LinuxProcessConnectionsReaderTest, FindsItsOwnIPv6Listener)
{
    const Socket listener(::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    if (listener.fd() < 0 || ::bind(listener.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.fd(), 1) != 0)
    {
        GTEST_SKIP() << "no IPv6 loopback here";
    }
    const std::uint16_t port = listener.localPort();

    for (auto& [name, reader] : readers())
    {
        SCOPED_TRACE(name);
        const ConnectionsReadResult result = reader->readConnections(self());
        ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
        const ProcessConnection* const row = findLocal(result, ConnectionProtocol::Tcp, port);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->family, ConnectionFamily::IPv6);
        EXPECT_EQ(row->state, ConnectionState::Listen);
        std::array<std::uint8_t, 16> loopback6{};
        loopback6[15] = 1;
        EXPECT_EQ(row->local.address, loopback6);
    }
}

TEST(LinuxProcessConnectionsReaderTest, ATableNetlinkRefusesIsReadFromProc)
{
    // Every dump answered "no diag module" (ENOENT): each table comes from /proc/[pid]/net instead.
    auto transport = std::make_unique<TestSupport::ScriptedNetlinkTransport>();
    TestSupport::ScriptedNetlinkTransport* const scripted = transport.get();
    scripted->onRequest = [](const TestSupport::ScriptedNetlinkTransport::Request& request)
    {
        return TestSupport::ScriptedNetlinkTransport::Reply{
            TestSupport::errorDatagram(request.sequence, TestSupport::ScriptedNetlinkTransport::PORT_ID, -ENOENT)};
    };
    LinuxProcessConnectionsReader reader(std::move(transport));

    const Socket listener = loopbackSocket(SOCK_STREAM);
    ASSERT_GE(listener.fd(), 0);
    const ConnectionsReadResult result = reader.readConnections(self());
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
    EXPECT_EQ(scripted->requests.size(), 4U) << "TCP and UDP, IPv4 and IPv6";
    EXPECT_NE(findLocal(result, ConnectionProtocol::Tcp, listener.localPort()), nullptr);
}

TEST(LinuxProcessConnectionsReaderTest, AFailedDumpSendsTheRestOfTheReadToProcWithoutWaitingOnNetlinkAgain)
{
    // The first dump times out: one receive timeout is all this read pays; every table, including the
    // ones not yet dumped, comes from /proc/[pid]/net.
    auto transport = std::make_unique<TestSupport::ScriptedNetlinkTransport>();
    TestSupport::ScriptedNetlinkTransport* const scripted = transport.get();
    scripted->onRequest = [](const TestSupport::ScriptedNetlinkTransport::Request& /*request*/)
    {
        return TestSupport::ScriptedNetlinkTransport::Reply{std::nullopt};
    };
    LinuxProcessConnectionsReader reader(std::move(transport));

    const Socket listener = loopbackSocket(SOCK_STREAM);
    ASSERT_GE(listener.fd(), 0);
    const ConnectionsReadResult result = reader.readConnections(self());
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
    EXPECT_EQ(scripted->requests.size(), 1U);
    EXPECT_NE(findLocal(result, ConnectionProtocol::Tcp, listener.localPort()), nullptr);
}

TEST(LinuxProcessConnectionsReaderTest, ACompleteNetlinkDumpIsWhatTheRowsShow)
{
    // The kernel's (scripted) word for this process's listener: a distinctive remote end, so the row
    // can only have come from the dump, not from /proc.
    const Socket listener = loopbackSocket(SOCK_STREAM);
    ASSERT_GE(listener.fd(), 0);
    struct stat info{};
    ASSERT_EQ(::fstat(listener.fd(), &info), 0);
    const auto inode = static_cast<std::uint32_t>(info.st_ino);

    auto transport = std::make_unique<TestSupport::ScriptedNetlinkTransport>();
    transport->onRequest = [inode](const TestSupport::ScriptedNetlinkTransport::Request& request)
    {
        TestSupport::ScriptedNetlinkTransport::Reply reply;
        if (request.protocol == IPPROTO_TCP && request.family == AF_INET)
        {
            inet_diag_msg message{};
            message.idiag_family = AF_INET;
            message.idiag_state = 1; // ESTABLISHED
            message.idiag_inode = inode;
            message.id.idiag_sport = htons(4321);
            message.id.idiag_dport = htons(9999);
            message.id.idiag_dst[0] = htonl(0xCB007107); // 203.0.113.7
            const std::array<inet_diag_msg, 1> messages{message};
            reply.emplace_back(
                TestSupport::diagMessagesDatagram(messages, request.sequence, TestSupport::ScriptedNetlinkTransport::PORT_ID));
        }
        reply.emplace_back(TestSupport::doneDatagram(request.sequence, TestSupport::ScriptedNetlinkTransport::PORT_ID));
        return reply;
    };
    LinuxProcessConnectionsReader reader(std::move(transport));

    const ConnectionsReadResult result = reader.readConnections(self());
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
    ASSERT_EQ(result.connections.size(), 1U) << "only the dump's socket, matched by inode";
    const ProcessConnection& row = result.connections.front();
    EXPECT_EQ(row.local.port, 4321);
    EXPECT_EQ(row.remote.port, 9999);
    EXPECT_EQ(row.remote.address[0], 203);
    EXPECT_EQ(row.remote.address[3], 7);
    EXPECT_EQ(row.state, ConnectionState::Established);
}

TEST(LinuxProcessConnectionsReaderTest, APidWithNoProcessReadsAsExited)
{
    // A child that exits at once: once reaped, its PID names no process.
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        ::_exit(0);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);

    LinuxProcessConnectionsReader reader;
    const ConnectionsReadResult result = reader.readConnections({.pid = child, .startTimeTicks = 12345});
    EXPECT_EQ(result.status, ConnectionsReadStatus::ProcessExited);
    EXPECT_TRUE(result.connections.empty());
}

TEST(LinuxProcessConnectionsReaderTest, AMismatchedStartTimeReadsAsExited)
{
    // The PID is this process, the start time another's: a reused PID must not show this one's sockets.
    const Socket listener = loopbackSocket(SOCK_STREAM);
    ASSERT_GE(listener.fd(), 0);
    LinuxProcessConnectionsReader reader;
    ProcessTarget target = self();
    target.startTimeTicks += 1;
    const ConnectionsReadResult result = reader.readConnections(target);
    EXPECT_EQ(result.status, ConnectionsReadStatus::ProcessExited);
    EXPECT_TRUE(result.connections.empty());
}

TEST(LinuxProcessConnectionsReaderTest, AnUnknownStartTimeIsRefused)
{
    LinuxProcessConnectionsReader reader;
    const ConnectionsReadResult result = reader.readConnections({.pid = static_cast<std::int32_t>(::getpid()), .startTimeTicks = 0});
    EXPECT_EQ(result.status, ConnectionsReadStatus::IdentityUnknown);
    EXPECT_EQ(reader.readConnections({.pid = 0, .startTimeTicks = 1}).status, ConnectionsReadStatus::ProcessExited);
}

TEST(LinuxProcessConnectionsReaderTest, AnotherUsersProcessIsNotPermitted)
{
    // PID 1 (init) belongs to root: unless this test runs with ptrace rights over it, its fd links
    // are off limits.
    const std::uint64_t initStart = startTicksOf(1);
    if (initStart == 0)
    {
        GTEST_SKIP() << "/proc/1/stat is not readable here (hidepid)";
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int probe = ::open("/proc/1/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (probe >= 0)
    {
        ::close(probe);
        GTEST_SKIP() << "running with access to PID 1's descriptors (root or CAP_SYS_PTRACE)";
    }
    LinuxProcessConnectionsReader reader;
    const ConnectionsReadResult result = reader.readConnections({.pid = 1, .startTimeTicks = initStart});
    EXPECT_EQ(result.status, ConnectionsReadStatus::PermissionDenied);
    EXPECT_TRUE(result.connections.empty());
}

// ========== The /proc/[pid]/net tables over a synthetic /proc ==========

/// A synthetic /proc holding one process, PID 100 started at tick 4242, with two sockets open (inodes
/// 1001 and 1002) and the /proc/[pid]/net tables a test writes.
class SyntheticProcConnections : public ::testing::Test
{
  protected:
    static constexpr std::int32_t PID = 100;
    static constexpr ProcessTarget TARGET{.pid = PID, .startTimeTicks = 4242};

    void SetUp() override
    {
        std::filesystem::create_directories(pidDir() / "fd");
        std::filesystem::create_directories(pidDir() / "net");
        std::filesystem::create_directories(pidDir() / "ns");
        write("stat", "100 (server) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 1 0 4242 1000 50\n");
        std::filesystem::create_symlink("socket:[1001]", pidDir() / "fd" / "3");
        std::filesystem::create_symlink("socket:[1002]", pidDir() / "fd" / "4");
        std::filesystem::create_symlink("/dev/null", pidDir() / "fd" / "5");
        std::filesystem::create_symlink("net:[1]", pidDir() / "ns" / "net");
    }

    [[nodiscard]] std::filesystem::path pidDir() const
    {
        return m_Proc.path / std::to_string(PID);
    }

    void write(const std::string& name, const std::string& contents) const
    {
        std::ofstream(pidDir() / name) << contents;
    }

    /// The four tables, each with a header; tcp holds inode 1001 (listening) and udp inode 1002.
    void writeAllTables() const
    {
        write("net/tcp",
              "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
              "   0: 0100007F:0277 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 1001 1 0\n"
              "   1: 0100007F:0278 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 7777 1 0\n");
        write("net/udp",
              "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n"
              "  5: 00000000:14E9 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000        0 1002 2 0 0\n");
        write("net/tcp6", "  sl  local_address remote_address st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
        write("net/udp6", "  sl  local_address remote_address st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    }

    [[nodiscard]] ConnectionsReadResult read() const
    {
        LinuxProcessConnectionsReader reader(LinuxProcessConnectionsReader::ProcRoot{.path = m_Proc.path.string(), .afterFdScan = {}});
        return reader.readConnections(TARGET);
    }

  private:
    TestSupport::ScopedTempDir m_Proc{"ts_test_proc_connections"};
};

TEST_F(SyntheticProcConnections, EveryTableReadWholeGivesOnlyThatProcessSockets)
{
    writeAllTables();
    const ConnectionsReadResult result = read();
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
    ASSERT_EQ(result.connections.size(), 2U) << "inode 7777 is another process's";
    EXPECT_EQ(result.connections[0].protocol, ConnectionProtocol::Tcp);
    EXPECT_EQ(result.connections[0].local.port, 631);
    EXPECT_EQ(result.connections[0].state, ConnectionState::Listen);
    EXPECT_EQ(result.connections[1].protocol, ConnectionProtocol::Udp);
    EXPECT_EQ(result.connections[1].local.port, 5353);
}

TEST_F(SyntheticProcConnections, AnAbsentFamilyHasCompleteEmptyTables)
{
    // IPv6 disabled: no tcp6 or udp6 at all (ENOENT) -- known absent, not a failure.
    writeAllTables();
    std::filesystem::remove(pidDir() / "net" / "tcp6");
    std::filesystem::remove(pidDir() / "net" / "udp6");
    const ConnectionsReadResult result = read();
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
    EXPECT_EQ(result.connections.size(), 2U);
}

TEST_F(SyntheticProcConnections, ATableThatCannotBeReadFailsTheReadInsteadOfAPartialOk)
{
    // tcp reads, udp does not (EISDIR here; an I/O error in life): never Ok with the UDP rows missing.
    writeAllTables();
    std::filesystem::remove(pidDir() / "net" / "udp");
    std::filesystem::create_directories(pidDir() / "net" / "udp");
    const ConnectionsReadResult result = read();
    EXPECT_EQ(result.status, ConnectionsReadStatus::Failed);
    EXPECT_TRUE(result.connections.empty());
    EXPECT_EQ(result.detail, std::generic_category().message(EISDIR));
}

TEST_F(SyntheticProcConnections, AProcessWithNoSocketsReadsNoTables)
{
    std::filesystem::remove(pidDir() / "fd" / "3");
    std::filesystem::remove(pidDir() / "fd" / "4");
    // No tables at all: with no socket inodes they are never opened.
    const ConnectionsReadResult result = read();
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
    EXPECT_TRUE(result.connections.empty());
}

TEST_F(SyntheticProcConnections, ANoSocketsResultIsConfirmedAgainstAProcessThatExitedDuringTheScan)
{
    // No socket fds; between the fd scan and its conclusion, stat starts naming another process (the
    // PID was reused while the empty fd directory was listed). That must read as exited, not as "no
    // sockets".
    std::filesystem::remove(pidDir() / "fd" / "3");
    std::filesystem::remove(pidDir() / "fd" / "4");
    int scans = 0;
    LinuxProcessConnectionsReader reader(LinuxProcessConnectionsReader::ProcRoot{
        .path = pidDir().parent_path().string(),
        .afterFdScan = [this, &scans]()
        {
            ++scans;
            write("stat", "100 (other) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 1 0 9999 1000 50\n");
        }});
    const ConnectionsReadResult result = reader.readConnections(TARGET);
    EXPECT_EQ(scans, 1);
    EXPECT_EQ(result.status, ConnectionsReadStatus::ProcessExited);
    EXPECT_TRUE(result.connections.empty());
}

TEST_F(SyntheticProcConnections, AStartTimeThatDoesNotMatchReadsAsExited)
{
    writeAllTables();
    LinuxProcessConnectionsReader reader(
        LinuxProcessConnectionsReader::ProcRoot{.path = pidDir().parent_path().string(), .afterFdScan = {}});
    const ConnectionsReadResult result = reader.readConnections({.pid = PID, .startTimeTicks = 4243});
    EXPECT_EQ(result.status, ConnectionsReadStatus::ProcessExited);
}

} // namespace
} // namespace Platform
