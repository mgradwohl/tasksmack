/// @file test_WindowsProcessConnections.cpp
/// @brief Platform::Windows::WindowsProcessConnectionsReader (#1489) against fake iphlpapi/kernel32
/// calls: MIB states mapped, IPv4/IPv6 addresses kept in network byte order and ports converted from
/// it, rows kept only for the target PID, a reused PID, an unknown start time, a denied or failed open,
/// an exited process, a table that grows between the size query and the copy (and one that never stops
/// growing), a table that fails, and the handle closed exactly once. Plus one read of this test process
/// through the real calls, finding loopback sockets it opened.

#include "Platform/Factory.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/Windows/WindowsProcessConnections.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - Winsock: include-cleaner lacks mappings for sockaddr_in, htons, ...
#include <winsock2.h>
#include <ws2tcpip.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform::Windows
{
namespace
{

constexpr std::uint64_t START_TICKS = 0x01DB'0000'1234'5678ULL;
constexpr DWORD TARGET_PID = 4242;
constexpr DWORD OTHER_PID = 777;
constexpr ProcessTarget TARGET{.pid = static_cast<std::int32_t>(TARGET_PID), .startTimeTicks = START_TICKS};

/// A port as the tables store it: network byte order in the low 16 bits.
[[nodiscard]] DWORD tablePort(std::uint16_t port)
{
    return htons(port);
}

/// An IPv4 address DWORD whose in-memory bytes are @p bytes (network order).
[[nodiscard]] DWORD tableAddress(std::array<std::uint8_t, 4> bytes)
{
    DWORD address = 0;
    std::memcpy(&address, bytes.data(), sizeof(address));
    return address;
}

[[nodiscard]] std::array<std::uint8_t, 16> v4(std::array<std::uint8_t, 4> bytes)
{
    std::array<std::uint8_t, 16> address{};
    std::ranges::copy(bytes, address.begin());
    return address;
}

constexpr std::array<std::uint8_t, 16> V6_LOCAL{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
constexpr std::array<std::uint8_t, 16> V6_REMOTE{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};

/// A MIB_*TABLE_OWNER_PID's bytes: the row count, then the rows where the struct puts them.
template<typename TableT, typename RowT> [[nodiscard]] std::vector<std::byte> makeTable(const std::vector<RowT>& rows)
{
    constexpr std::size_t FIRST_ROW = offsetof(TableT, table);
    std::vector<std::byte> bytes(FIRST_ROW + (rows.size() * sizeof(RowT)));
    const auto count = static_cast<DWORD>(rows.size());
    std::memcpy(bytes.data(), &count, sizeof(count));
    if (!rows.empty())
    {
        std::memcpy(bytes.data() + FIRST_ROW, rows.data(), rows.size() * sizeof(RowT));
    }
    return bytes;
}

[[nodiscard]] MIB_TCPROW_OWNER_PID tcp4Row(DWORD pid, MIB_TCP_STATE state, std::uint16_t localPort, std::uint16_t remotePort = 0)
{
    MIB_TCPROW_OWNER_PID row{};
    row.dwState = static_cast<DWORD>(state);
    row.dwLocalAddr = tableAddress({192, 168, 1, 10});
    row.dwLocalPort = tablePort(localPort);
    row.dwRemoteAddr = remotePort == 0 ? 0 : tableAddress({93, 184, 216, 34});
    row.dwRemotePort = tablePort(remotePort);
    row.dwOwningPid = pid;
    return row;
}

[[nodiscard]] MIB_TCP6ROW_OWNER_PID tcp6Row(DWORD pid, MIB_TCP_STATE state, std::uint16_t localPort, std::uint16_t remotePort)
{
    MIB_TCP6ROW_OWNER_PID row{};
    std::ranges::copy(V6_LOCAL, std::begin(row.ucLocalAddr));
    std::ranges::copy(V6_REMOTE, std::begin(row.ucRemoteAddr));
    row.dwLocalPort = tablePort(localPort);
    row.dwRemotePort = tablePort(remotePort);
    row.dwState = static_cast<DWORD>(state);
    row.dwOwningPid = pid;
    return row;
}

[[nodiscard]] MIB_UDPROW_OWNER_PID udp4Row(DWORD pid, std::uint16_t localPort)
{
    MIB_UDPROW_OWNER_PID row{};
    row.dwLocalAddr = tableAddress({127, 0, 0, 1});
    row.dwLocalPort = tablePort(localPort);
    row.dwOwningPid = pid;
    return row;
}

[[nodiscard]] MIB_UDP6ROW_OWNER_PID udp6Row(DWORD pid, std::uint16_t localPort)
{
    MIB_UDP6ROW_OWNER_PID row{};
    std::ranges::copy(V6_LOCAL, std::begin(row.ucLocalAddr));
    row.dwLocalPort = tablePort(localPort);
    row.dwOwningPid = pid;
    return row;
}

/// One fake table: its bytes, an error to fail with, and growth to simulate.
struct FakeTable
{
    std::vector<std::byte> bytes;
    DWORD error = NO_ERROR;          // non-zero: the call fails with it
    std::size_t growOnFirstCall = 0; // bytes the table grows by after the first call
    bool growsForever = false;       // outgrows every buffer it is given
    int calls = 0;
};

/// The answers the fakes give, set per test.
struct FakeSystem
{
    DWORD openError = 0; // non-zero: OpenProcess fails with it
    std::uint64_t creationTicks = START_TICKS;
    DWORD exitCode = STILL_ACTIVE;
    FakeTable tcp4;
    FakeTable tcp6;
    FakeTable udp4;
    FakeTable udp6;
    int opens = 0;
    int closes = 0;
};

FakeSystem& fake()
{
    static FakeSystem instance;
    return instance;
}

HANDLE fakeProcessHandle()
{
    static int token = 0;
    return &token;
}

DWORD answer(FakeTable& table, PVOID data, PDWORD size)
{
    ++table.calls;
    if (table.error != NO_ERROR)
    {
        return table.error;
    }
    if (table.growsForever)
    {
        *size += 4096;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (table.calls == 2 && table.growOnFirstCall > 0)
    {
        // Sockets opened between the size query and the copy: padding rows of another process.
        table.bytes.resize(table.bytes.size() + table.growOnFirstCall);
    }
    const auto needed = static_cast<DWORD>(table.bytes.size());
    if (*size < needed)
    {
        *size = needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    std::memcpy(data, table.bytes.data(), table.bytes.size());
    return NO_ERROR;
}

DWORD WINAPI fakeGetExtendedTcpTable(PVOID data, PDWORD size, BOOL order, ULONG family, TCP_TABLE_CLASS tableClass, ULONG reserved)
{
    EXPECT_EQ(order, FALSE);
    EXPECT_EQ(tableClass, TCP_TABLE_OWNER_PID_ALL);
    EXPECT_EQ(reserved, 0U);
    return answer(family == AF_INET6 ? fake().tcp6 : fake().tcp4, data, size);
}

DWORD WINAPI fakeGetExtendedUdpTable(PVOID data, PDWORD size, BOOL order, ULONG family, UDP_TABLE_CLASS tableClass, ULONG reserved)
{
    EXPECT_EQ(order, FALSE);
    EXPECT_EQ(tableClass, UDP_TABLE_OWNER_PID);
    EXPECT_EQ(reserved, 0U);
    return answer(family == AF_INET6 ? fake().udp6 : fake().udp4, data, size);
}

HANDLE WINAPI fakeOpenProcess(DWORD access, BOOL /*inherit*/, DWORD pid)
{
    EXPECT_EQ(access, static_cast<DWORD>(PROCESS_QUERY_LIMITED_INFORMATION));
    EXPECT_EQ(pid, TARGET_PID);
    if (fake().openError != 0)
    {
        SetLastError(fake().openError);
        return nullptr;
    }
    ++fake().opens;
    return fakeProcessHandle();
}

BOOL WINAPI fakeCloseHandle(HANDLE handle)
{
    EXPECT_EQ(handle, fakeProcessHandle());
    ++fake().closes;
    return TRUE;
}

BOOL WINAPI fakeGetProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
{
    creation->dwLowDateTime = static_cast<DWORD>(fake().creationTicks & 0xFFFFFFFFU);
    creation->dwHighDateTime = static_cast<DWORD>(fake().creationTicks >> 32U);
    return TRUE;
}

BOOL WINAPI fakeGetExitCodeProcess(HANDLE /*process*/, LPDWORD code)
{
    *code = fake().exitCode;
    return TRUE;
}

[[nodiscard]] ProcessConnectionsFunctions fakeApi()
{
    return {
        .getExtendedTcpTable = &fakeGetExtendedTcpTable,
        .getExtendedUdpTable = &fakeGetExtendedUdpTable,
        .openProcess = &fakeOpenProcess,
        .closeHandle = &fakeCloseHandle,
        .getProcessTimes = &fakeGetProcessTimes,
        .getExitCodeProcess = &fakeGetExitCodeProcess,
    };
}

class WindowsProcessConnectionsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = {};
        fake().tcp4.bytes = makeTable<MIB_TCPTABLE_OWNER_PID>(std::vector{
            tcp4Row(TARGET_PID, MIB_TCP_STATE_ESTAB, 55026, 443),
            tcp4Row(OTHER_PID, MIB_TCP_STATE_ESTAB, 55027, 443),
            tcp4Row(TARGET_PID, MIB_TCP_STATE_LISTEN, 8080),
            tcp4Row(0, MIB_TCP_STATE_TIME_WAIT, 55000, 80), // TIME_WAIT: no process holds it
        });
        fake().tcp6.bytes = makeTable<MIB_TCP6TABLE_OWNER_PID>(std::vector{tcp6Row(TARGET_PID, MIB_TCP_STATE_SYN_SENT, 50000, 22)});
        fake().udp4.bytes = makeTable<MIB_UDPTABLE_OWNER_PID>(std::vector{udp4Row(OTHER_PID, 53), udp4Row(TARGET_PID, 5353)});
        fake().udp6.bytes = makeTable<MIB_UDP6TABLE_OWNER_PID>(std::vector{udp6Row(TARGET_PID, 546)});
    }

    void TearDown() override
    {
        EXPECT_EQ(fake().opens, fake().closes); // every handle opened is closed, once
    }

    WindowsProcessConnectionsReader m_Reader{fakeApi()};
};

[[nodiscard]] const ProcessConnection* findLocal(const ConnectionsReadResult& result, ConnectionProtocol protocol, std::uint16_t port)
{
    const auto found = std::ranges::find_if(result.connections,
                                            [&](const ProcessConnection& connection)
                                            { return connection.protocol == protocol && connection.local.port == port; });
    return found == result.connections.end() ? nullptr : &*found;
}

TEST(WindowsProcessConnectionsMappingTest, MapsEveryMibTcpState)
{
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_CLOSED), ConnectionState::Closed);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_LISTEN), ConnectionState::Listen);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_SYN_SENT), ConnectionState::SynSent);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_SYN_RCVD), ConnectionState::SynReceived);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_ESTAB), ConnectionState::Established);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_FIN_WAIT1), ConnectionState::FinWait1);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_FIN_WAIT2), ConnectionState::FinWait2);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_CLOSE_WAIT), ConnectionState::CloseWait);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_CLOSING), ConnectionState::Closing);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_LAST_ACK), ConnectionState::LastAck);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_TIME_WAIT), ConnectionState::TimeWait);
    EXPECT_EQ(toConnectionState(MIB_TCP_STATE_DELETE_TCB), ConnectionState::Closed);
    EXPECT_EQ(toConnectionState(0), ConnectionState::Unknown);
    EXPECT_EQ(toConnectionState(99), ConnectionState::Unknown);
}

TEST(WindowsProcessConnectionsMappingTest, PortsAreConvertedFromNetworkOrderInTheLow16Bits)
{
    // 443 is 0x01BB: on the wire 01 BB, which a little-endian DWORD reads as 0x0000BB01.
    EXPECT_EQ(toHostPort(0x0000BB01U), 443U);
    EXPECT_EQ(toHostPort(0x00005000U), 80U);
    EXPECT_EQ(toHostPort(0x0000FFFFU), 65535U);
    EXPECT_EQ(toHostPort(0U), 0U);
    // The high 16 bits are not part of the port.
    EXPECT_EQ(toHostPort(0xABCDBB01U), 443U);
}

TEST(WindowsProcessConnectionsMappingTest, Ipv4AddressesKeepTheirNetworkOrderBytes)
{
    const ProcessConnection connection = toConnection(tcp4Row(TARGET_PID, MIB_TCP_STATE_ESTAB, 55026, 443));
    EXPECT_EQ(connection.protocol, ConnectionProtocol::Tcp);
    EXPECT_EQ(connection.family, ConnectionFamily::IPv4);
    EXPECT_EQ(connection.local.address, v4({192, 168, 1, 10}));
    EXPECT_EQ(connection.local.port, 55026U);
    EXPECT_EQ(connection.remote.address, v4({93, 184, 216, 34}));
    EXPECT_EQ(connection.remote.port, 443U);
    EXPECT_EQ(connection.state, ConnectionState::Established);
}

TEST(WindowsProcessConnectionsMappingTest, Ipv6AddressesAreCopiedAsTheyAre)
{
    const ProcessConnection connection = toConnection(tcp6Row(TARGET_PID, MIB_TCP_STATE_LISTEN, 443, 0));
    EXPECT_EQ(connection.family, ConnectionFamily::IPv6);
    EXPECT_EQ(connection.local.address, V6_LOCAL);
    EXPECT_EQ(connection.remote.address, V6_REMOTE);
    EXPECT_EQ(connection.local.port, 443U);
    EXPECT_EQ(connection.remote.port, 0U);
    EXPECT_EQ(connection.state, ConnectionState::Listen);
}

TEST(WindowsProcessConnectionsMappingTest, UdpRowsAreUnconnectedWithNoRemoteEnd)
{
    const ProcessConnection udp4 = toConnection(udp4Row(TARGET_PID, 5353));
    EXPECT_EQ(udp4.protocol, ConnectionProtocol::Udp);
    EXPECT_EQ(udp4.family, ConnectionFamily::IPv4);
    EXPECT_EQ(udp4.local.address, v4({127, 0, 0, 1}));
    EXPECT_EQ(udp4.local.port, 5353U);
    EXPECT_EQ(udp4.remote.address, (std::array<std::uint8_t, 16>{}));
    EXPECT_EQ(udp4.remote.port, 0U);
    EXPECT_EQ(udp4.state, ConnectionState::Closed);

    const ProcessConnection udp6 = toConnection(udp6Row(TARGET_PID, 546));
    EXPECT_EQ(udp6.protocol, ConnectionProtocol::Udp);
    EXPECT_EQ(udp6.family, ConnectionFamily::IPv6);
    EXPECT_EQ(udp6.local.address, V6_LOCAL);
    EXPECT_EQ(udp6.local.port, 546U);
    EXPECT_EQ(udp6.state, ConnectionState::Closed);
}

TEST_F(WindowsProcessConnectionsTest, ListsOnlyTheTargetsRowsFromAllFourTables)
{
    const ConnectionsReadResult result = m_Reader.readConnections(TARGET);
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
    EXPECT_TRUE(result.detail.empty());
    ASSERT_EQ(result.connections.size(), 5U); // 2 TCP4, 1 TCP6, 1 UDP4, 1 UDP6

    const ProcessConnection* established = findLocal(result, ConnectionProtocol::Tcp, 55026);
    ASSERT_NE(established, nullptr);
    EXPECT_EQ(established->state, ConnectionState::Established);
    EXPECT_EQ(findLocal(result, ConnectionProtocol::Tcp, 55027), nullptr); // another process's
    EXPECT_EQ(findLocal(result, ConnectionProtocol::Tcp, 55000), nullptr); // TIME_WAIT, owner 0
    ASSERT_NE(findLocal(result, ConnectionProtocol::Tcp, 8080), nullptr);
    EXPECT_EQ(findLocal(result, ConnectionProtocol::Tcp, 8080)->state, ConnectionState::Listen);
    ASSERT_NE(findLocal(result, ConnectionProtocol::Tcp, 50000), nullptr);
    EXPECT_EQ(findLocal(result, ConnectionProtocol::Tcp, 50000)->family, ConnectionFamily::IPv6);
    EXPECT_EQ(findLocal(result, ConnectionProtocol::Udp, 53), nullptr); // another process's
    ASSERT_NE(findLocal(result, ConnectionProtocol::Udp, 5353), nullptr);
    ASSERT_NE(findLocal(result, ConnectionProtocol::Udp, 546), nullptr);
    EXPECT_EQ(findLocal(result, ConnectionProtocol::Udp, 546)->family, ConnectionFamily::IPv6);
    EXPECT_EQ(fake().opens, 1);
}

TEST_F(WindowsProcessConnectionsTest, NoSocketsIsAnEmptyOk)
{
    fake().tcp4.bytes = makeTable<MIB_TCPTABLE_OWNER_PID>(std::vector<MIB_TCPROW_OWNER_PID>{});
    fake().tcp6.bytes = makeTable<MIB_TCP6TABLE_OWNER_PID>(std::vector<MIB_TCP6ROW_OWNER_PID>{});
    fake().udp4.bytes = makeTable<MIB_UDPTABLE_OWNER_PID>(std::vector{udp4Row(OTHER_PID, 53)});
    fake().udp6.bytes = makeTable<MIB_UDP6TABLE_OWNER_PID>(std::vector<MIB_UDP6ROW_OWNER_PID>{});
    const ConnectionsReadResult result = m_Reader.readConnections(TARGET);
    EXPECT_EQ(result.status, ConnectionsReadStatus::Ok);
    EXPECT_TRUE(result.connections.empty());
}

TEST_F(WindowsProcessConnectionsTest, ReusedPidIsProcessExitedAndNoTableIsRead)
{
    fake().creationTicks = START_TICKS + 1;
    const ConnectionsReadResult result = m_Reader.readConnections(TARGET);
    EXPECT_EQ(result.status, ConnectionsReadStatus::ProcessExited);
    EXPECT_TRUE(result.connections.empty());
    EXPECT_EQ(fake().tcp4.calls, 0);
    EXPECT_EQ(fake().udp6.calls, 0);
}

TEST_F(WindowsProcessConnectionsTest, UnknownStartTimeIsIdentityUnknownAndNothingIsOpened)
{
    const ConnectionsReadResult result = m_Reader.readConnections({.pid = TARGET.pid, .startTimeTicks = 0});
    EXPECT_EQ(result.status, ConnectionsReadStatus::IdentityUnknown);
    EXPECT_EQ(fake().opens, 0);
    EXPECT_EQ(fake().tcp4.calls, 0);
}

TEST_F(WindowsProcessConnectionsTest, NonPositivePidIsProcessExited)
{
    EXPECT_EQ(m_Reader.readConnections({.pid = 0, .startTimeTicks = START_TICKS}).status, ConnectionsReadStatus::ProcessExited);
    EXPECT_EQ(m_Reader.readConnections({.pid = -1, .startTimeTicks = START_TICKS}).status, ConnectionsReadStatus::ProcessExited);
    EXPECT_EQ(fake().opens, 0);
}

TEST_F(WindowsProcessConnectionsTest, DeniedOpenIsPermissionDeniedAndNoTableIsRead)
{
    // The tables would list the process's sockets, but its identity cannot be confirmed.
    fake().openError = ERROR_ACCESS_DENIED;
    const ConnectionsReadResult result = m_Reader.readConnections(TARGET);
    EXPECT_EQ(result.status, ConnectionsReadStatus::PermissionDenied);
    EXPECT_TRUE(result.connections.empty());
    EXPECT_EQ(fake().tcp4.calls, 0);
}

TEST_F(WindowsProcessConnectionsTest, GoneOrExitedProcessIsProcessExited)
{
    fake().openError = ERROR_INVALID_PARAMETER; // no process has the PID
    EXPECT_EQ(m_Reader.readConnections(TARGET).status, ConnectionsReadStatus::ProcessExited);

    fake().openError = 0;
    fake().exitCode = 0; // exited, its object still held open somewhere
    EXPECT_EQ(m_Reader.readConnections(TARGET).status, ConnectionsReadStatus::ProcessExited);
    EXPECT_EQ(fake().tcp4.calls, 0);
}

TEST_F(WindowsProcessConnectionsTest, OtherOpenErrorIsFailedWithTheReason)
{
    fake().openError = ERROR_NOT_ENOUGH_MEMORY;
    const ConnectionsReadResult result = m_Reader.readConnections(TARGET);
    EXPECT_EQ(result.status, ConnectionsReadStatus::Failed);
    EXPECT_FALSE(result.detail.empty());
}

TEST_F(WindowsProcessConnectionsTest, GrowsTheBufferWhenTheTableOutgrowsIt)
{
    // A table bigger than the first buffer, which grows again between the size query and the copy.
    std::vector<MIB_TCPROW_OWNER_PID> rows(2000, tcp4Row(OTHER_PID, MIB_TCP_STATE_ESTAB, 1000, 443));
    rows.push_back(tcp4Row(TARGET_PID, MIB_TCP_STATE_ESTAB, 55026, 443));
    fake().tcp4.bytes = makeTable<MIB_TCPTABLE_OWNER_PID>(rows);
    fake().tcp4.growOnFirstCall = 20000; // more than the headroom the resize leaves
    const ConnectionsReadResult result = m_Reader.readConnections(TARGET);
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
    EXPECT_NE(findLocal(result, ConnectionProtocol::Tcp, 55026), nullptr);
    EXPECT_EQ(fake().tcp4.calls, 3); // too small, grown past the headroom in between, then copied
}

TEST_F(WindowsProcessConnectionsTest, ATableThatNeverStopsGrowingFailsAfterBoundedTries)
{
    fake().udp4.growsForever = true;
    const ConnectionsReadResult result = m_Reader.readConnections(TARGET);
    EXPECT_EQ(result.status, ConnectionsReadStatus::Failed);
    EXPECT_FALSE(result.detail.empty());
    EXPECT_TRUE(result.connections.empty()); // no partial list
    EXPECT_EQ(fake().udp4.calls, MAX_TABLE_ATTEMPTS);
}

TEST_F(WindowsProcessConnectionsTest, AFailedTableFailsTheReadWithTheReason)
{
    fake().tcp6.error = ERROR_INVALID_PARAMETER;
    const ConnectionsReadResult result = m_Reader.readConnections(TARGET);
    EXPECT_EQ(result.status, ConnectionsReadStatus::Failed);
    EXPECT_FALSE(result.detail.empty());
    EXPECT_TRUE(result.connections.empty());
    EXPECT_EQ(fake().udp4.calls, 0); // stopped at the first failure
}

TEST_F(WindowsProcessConnectionsTest, ACountLargerThanTheTableIsClampedToItsRows)
{
    auto bytes = makeTable<MIB_UDPTABLE_OWNER_PID>(std::vector{udp4Row(TARGET_PID, 5353)});
    const DWORD lie = 1'000'000;
    std::memcpy(bytes.data(), &lie, sizeof(lie));
    fake().udp4.bytes = std::move(bytes);
    const ConnectionsReadResult result = m_Reader.readConnections(TARGET);
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok);
    EXPECT_NE(findLocal(result, ConnectionProtocol::Udp, 5353), nullptr);
}

/// Winsock, started for the test's lifetime.
class WinsockSession
{
  public:
    WinsockSession()
    {
        WSADATA data{};
        m_Started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockSession()
    {
        if (m_Started)
        {
            WSACleanup();
        }
    }
    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
    WinsockSession(WinsockSession&&) = delete;
    WinsockSession& operator=(WinsockSession&&) = delete;

    [[nodiscard]] bool started() const
    {
        return m_Started;
    }

  private:
    bool m_Started = false;
};

/// A socket closed on scope exit, and the loopback port it is bound to.
class LoopbackSocket
{
  public:
    /// A @p type socket bound to 127.0.0.1 on a free port (listening, for TCP); !valid() on failure.
    LoopbackSocket(int type, int protocol) : m_Socket(::socket(AF_INET, type, protocol))
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        int length = sizeof(address);
        if (!isOpen() || ::bind(m_Socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            (type == SOCK_STREAM && ::listen(m_Socket, 4) != 0) ||
            ::getsockname(m_Socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        {
            return;
        }
        m_Port = ntohs(address.sin_port);
    }
    ~LoopbackSocket()
    {
        if (isOpen())
        {
            closesocket(m_Socket);
        }
    }
    LoopbackSocket(const LoopbackSocket&) = delete;
    LoopbackSocket& operator=(const LoopbackSocket&) = delete;
    LoopbackSocket(LoopbackSocket&&) = delete;
    LoopbackSocket& operator=(LoopbackSocket&&) = delete;

    [[nodiscard]] bool valid() const
    {
        return m_Port != 0;
    }
    [[nodiscard]] std::uint16_t port() const
    {
        return m_Port;
    }

  private:
    [[nodiscard]] bool isOpen() const
    {
        return std::cmp_not_equal(m_Socket, INVALID_SOCKET);
    }

    SOCKET m_Socket = INVALID_SOCKET;
    std::uint16_t m_Port = 0;
};

TEST(WindowsProcessConnectionsRealTest, FindsALoopbackListenerAndUdpSocketOfThisProcess)
{
    const WinsockSession winsock;
    ASSERT_TRUE(winsock.started());
    const LoopbackSocket listener(SOCK_STREAM, IPPROTO_TCP);
    ASSERT_TRUE(listener.valid());
    const LoopbackSocket udp(SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(udp.valid());
    const std::uint16_t tcpPort = listener.port();
    const std::uint16_t udpPort = udp.port();

    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    ASSERT_NE(GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime), 0);
    const ProcessTarget self{
        .pid = static_cast<std::int32_t>(GetCurrentProcessId()),
        .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
    };

    const auto reader = makeProcessConnectionsReader();
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->hasConnections());
    const ConnectionsReadResult result = reader->readConnections(self);
    ASSERT_EQ(result.status, ConnectionsReadStatus::Ok) << result.detail;

    const ProcessConnection* tcp = findLocal(result, ConnectionProtocol::Tcp, tcpPort);
    ASSERT_NE(tcp, nullptr) << "listener on 127.0.0.1:" << tcpPort << " not found";
    EXPECT_EQ(tcp->family, ConnectionFamily::IPv4);
    EXPECT_EQ(tcp->state, ConnectionState::Listen);
    EXPECT_EQ(tcp->local.address, v4({127, 0, 0, 1}));

    const ProcessConnection* bound = findLocal(result, ConnectionProtocol::Udp, udpPort);
    ASSERT_NE(bound, nullptr) << "UDP socket on 127.0.0.1:" << udpPort << " not found";
    EXPECT_EQ(bound->state, ConnectionState::Closed);
    EXPECT_EQ(bound->local.address, v4({127, 0, 0, 1}));

    // A different start time for the same PID reads as a reused PID.
    const ProcessTarget stranger{.pid = self.pid, .startTimeTicks = self.startTimeTicks + 1};
    EXPECT_EQ(reader->readConnections(stranger).status, ConnectionsReadStatus::ProcessExited);
}

} // namespace
} // namespace Platform::Windows
