/// @file test_WindowsTcpClose.cpp
/// @brief Close connection on Windows (#3358): Platform::Windows::closeTcpConnectionOf() and
/// WindowsProcessActions::closeTcpConnection(), against a fake IPv4 TCP table and a fake SetTcpEntry, so no test
/// resets a connection: the row found again by both endpoints and its holder, then set to MIB_TCP_STATE_DELETE_TCB;
/// every refusal before anything is set (no peer, gone, another PID's, a listener, TIME_WAIT, an unreadable table);
/// SetTcpEntry's refusals, access denied and 317 as permission refusals the elevated helper can retry; and the
/// identity check and PID 4 guard in front. Plus a CiRealSystem test that resets a real loopback connection of this
/// process.

#include "Platform/IProcessActions.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"
#include "Platform/Windows/WindowsTcpClose.h"
#include "Support/CiRealSystem.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
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

constexpr std::int32_t HOLDER_PID = 4242;
constexpr std::int32_t OTHER_PID = 777;
constexpr std::array<std::uint8_t, 4> LOCAL{10, 0, 0, 5};
constexpr std::array<std::uint8_t, 4> REMOTE{142, 250, 80, 46};
constexpr std::uint16_t LOCAL_PORT = 52144;
constexpr std::uint16_t REMOTE_PORT = 443;
constexpr TcpConnectionToClose CONNECTION{
    .localAddress = LOCAL,
    .localPort = LOCAL_PORT,
    .remoteAddress = REMOTE,
    .remotePort = REMOTE_PORT,
};

[[nodiscard]] DWORD tableAddress(std::array<std::uint8_t, 4> bytes)
{
    DWORD address = 0;
    std::memcpy(&address, bytes.data(), sizeof(address));
    return address;
}

[[nodiscard]] MIB_TCPROW_OWNER_PID row(std::int32_t pid,
                                       DWORD state,
                                       std::uint16_t localPort = LOCAL_PORT,
                                       std::uint16_t remotePort = REMOTE_PORT,
                                       std::array<std::uint8_t, 4> remote = REMOTE)
{
    MIB_TCPROW_OWNER_PID owner{};
    owner.dwState = state;
    owner.dwLocalAddr = tableAddress(LOCAL);
    owner.dwLocalPort = htons(localPort);
    owner.dwRemoteAddr = tableAddress(remote);
    owner.dwRemotePort = htons(remotePort);
    owner.dwOwningPid = static_cast<DWORD>(pid);
    return owner;
}

[[nodiscard]] std::vector<std::byte> table(const std::vector<MIB_TCPROW_OWNER_PID>& rows)
{
    constexpr std::size_t FIRST_ROW = offsetof(MIB_TCPTABLE_OWNER_PID, table);
    std::vector<std::byte> bytes(FIRST_ROW + (rows.size() * sizeof(MIB_TCPROW_OWNER_PID)));
    const auto count = static_cast<DWORD>(rows.size());
    std::memcpy(bytes.data(), &count, sizeof(count));
    if (!rows.empty())
    {
        std::memcpy(bytes.data() + FIRST_ROW, rows.data(), rows.size() * sizeof(MIB_TCPROW_OWNER_PID));
    }
    return bytes;
}

/// The fake table and SetTcpEntry, with what they were asked.
struct FakeTcp
{
    static inline std::vector<std::byte> tcpTable;
    static inline DWORD tableError = NO_ERROR;
    static inline int tableCalls = 0;
    static inline DWORD setError = NO_ERROR;
    static inline std::vector<MIB_TCPROW> rowsSet;

    static void reset(const std::vector<MIB_TCPROW_OWNER_PID>& rows)
    {
        tcpTable = table(rows);
        tableError = NO_ERROR;
        tableCalls = 0;
        setError = NO_ERROR;
        rowsSet.clear();
    }

    static DWORD WINAPI getTcp(PVOID data, PDWORD size, BOOL /*order*/, ULONG family, TCP_TABLE_CLASS tableClass, ULONG /*reserved*/)
    {
        ++tableCalls;
        EXPECT_EQ(family, static_cast<ULONG>(AF_INET)) << "only the IPv4 table";
        EXPECT_EQ(tableClass, TCP_TABLE_OWNER_PID_ALL);
        if (tableError != NO_ERROR)
        {
            return tableError;
        }
        if (*size < tcpTable.size())
        {
            *size = static_cast<DWORD>(tcpTable.size());
            return ERROR_INSUFFICIENT_BUFFER;
        }
        std::memcpy(data, tcpTable.data(), tcpTable.size());
        *size = static_cast<DWORD>(tcpTable.size());
        return NO_ERROR;
    }

    static DWORD WINAPI
    getUdp(PVOID /*data*/, PDWORD /*size*/, BOOL /*order*/, ULONG /*family*/, UDP_TABLE_CLASS /*cls*/, ULONG /*reserved*/)
    {
        ADD_FAILURE() << "a UDP table read for a TCP close";
        return ERROR_NOT_SUPPORTED;
    }

    static DWORD WINAPI setTcpEntry(PMIB_TCPROW entry)
    {
        rowsSet.push_back(*entry);
        return setError;
    }

    [[nodiscard]] static TcpCloseFunctions functions()
    {
        return {.tables = {.getExtendedTcpTable = &getTcp, .getExtendedUdpTable = &getUdp}, .setTcpEntry = &setTcpEntry};
    }
};

class WindowsTcpCloseTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // The holder's connection, its listener on the same port and another process's connection beside it.
        FakeTcp::reset({
            row(HOLDER_PID, MIB_TCP_STATE_LISTEN, LOCAL_PORT, 0, {0, 0, 0, 0}),
            row(OTHER_PID, MIB_TCP_STATE_ESTAB, static_cast<std::uint16_t>(LOCAL_PORT + 1)),
            row(HOLDER_PID, MIB_TCP_STATE_ESTAB),
        });
    }
};

TEST_F(WindowsTcpCloseTest, TheRowTheHolderHasIsSetToDeleteTcb)
{
    const ProcessActionResult result = closeTcpConnectionOf(FakeTcp::functions(), HOLDER_PID, CONNECTION);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(FakeTcp::tableCalls, 1);
    ASSERT_EQ(FakeTcp::rowsSet.size(), 1U);
    const MIB_TCPROW& set = FakeTcp::rowsSet.front();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - MIB_TCPROW's state is a union
    EXPECT_EQ(set.dwState, static_cast<DWORD>(MIB_TCP_STATE_DELETE_TCB));
    EXPECT_EQ(set.dwLocalAddr, tableAddress(LOCAL));
    EXPECT_EQ(set.dwRemoteAddr, tableAddress(REMOTE));
    EXPECT_EQ(set.dwLocalPort, static_cast<DWORD>(htons(LOCAL_PORT))) << "the table's network-order port, as it stands";
    EXPECT_EQ(set.dwRemotePort, static_cast<DWORD>(htons(REMOTE_PORT)));
}

// Windows build 26300 reports state 0 for many rows (#3257): one with a peer, still held, is closed.
TEST_F(WindowsTcpCloseTest, ARowInStateZeroWithAPeerIsClosed)
{
    FakeTcp::reset({row(HOLDER_PID, 0)});
    EXPECT_TRUE(closeTcpConnectionOf(FakeTcp::functions(), HOLDER_PID, CONNECTION).success);
    EXPECT_EQ(FakeTcp::rowsSet.size(), 1U);
}

TEST_F(WindowsTcpCloseTest, NothingIsSetForAConnectionThatIsGoneMovedOrNoConnection)
{
    const TcpCloseFunctions fns = FakeTcp::functions();

    TcpConnectionToClose listener = CONNECTION;
    listener.remoteAddress = {};
    listener.remotePort = 0;
    const ProcessActionResult noPeer = closeTcpConnectionOf(fns, HOLDER_PID, listener);
    EXPECT_FALSE(noPeer.success);
    EXPECT_EQ(noPeer.errorMessage, "This socket has no remote end, so there is no connection to close");
    EXPECT_EQ(FakeTcp::tableCalls, 0) << "refused before the table is read";

    const ProcessActionResult otherHolder = closeTcpConnectionOf(fns, OTHER_PID, CONNECTION);
    EXPECT_FALSE(otherHolder.success);
    EXPECT_EQ(otherHolder.errorMessage, "PID 777 no longer holds this connection");
    EXPECT_FALSE(otherHolder.permissionDenied);

    TcpConnectionToClose gone = CONNECTION;
    gone.remotePort = 8443;
    EXPECT_EQ(closeTcpConnectionOf(fns, HOLDER_PID, gone).errorMessage, "PID 4242 no longer holds this connection");

    FakeTcp::reset({row(HOLDER_PID, MIB_TCP_STATE_LISTEN)});
    EXPECT_EQ(closeTcpConnectionOf(fns, HOLDER_PID, CONNECTION).errorMessage,
              "This socket is listening for connections, so there is no connection to close");
    EXPECT_TRUE(FakeTcp::rowsSet.empty()) << "a listener is never set";
    for (const DWORD closed : {
             static_cast<DWORD>(MIB_TCP_STATE_TIME_WAIT),
             static_cast<DWORD>(MIB_TCP_STATE_CLOSED),
             static_cast<DWORD>(MIB_TCP_STATE_DELETE_TCB),
         })
    {
        FakeTcp::reset({row(HOLDER_PID, closed)});
        EXPECT_EQ(closeTcpConnectionOf(fns, HOLDER_PID, CONNECTION).errorMessage, "The connection is already closed") << closed;
        EXPECT_TRUE(FakeTcp::rowsSet.empty()) << "state " << closed << " is never set";
    }

    FakeTcp::reset({row(HOLDER_PID, MIB_TCP_STATE_ESTAB)});
    FakeTcp::tableError = ERROR_NOT_SUPPORTED;
    const ProcessActionResult unread = closeTcpConnectionOf(fns, HOLDER_PID, CONNECTION);
    EXPECT_FALSE(unread.success);
    EXPECT_TRUE(unread.errorMessage.starts_with("Couldn't read the TCP connection table: ")) << unread.errorMessage;

    EXPECT_TRUE(FakeTcp::rowsSet.empty());

    TcpCloseFunctions none = fns;
    none.setTcpEntry = nullptr;
    FakeTcp::tableError = NO_ERROR;
    EXPECT_FALSE(closeTcpConnectionOf(none, HOLDER_PID, CONNECTION).success);
    EXPECT_TRUE(FakeTcp::rowsSet.empty());
}

TEST_F(WindowsTcpCloseTest, OnlyAnAdministratorsRefusalIsAPermissionRefusal)
{
    const TcpCloseFunctions fns = FakeTcp::functions();
    for (const DWORD notAdministrator : {static_cast<DWORD>(ERROR_ACCESS_DENIED), SET_TCP_ENTRY_NOT_ADMINISTRATOR})
    {
        FakeTcp::setError = notAdministrator;
        const ProcessActionResult refused = closeTcpConnectionOf(fns, HOLDER_PID, CONNECTION);
        EXPECT_FALSE(refused.success);
        EXPECT_TRUE(refused.permissionDenied) << notAdministrator << ": the helper can retry it";
        EXPECT_EQ(refused.errorMessage, CLOSE_CONNECTION_NEEDS_ADMINISTRATOR);
    }

    FakeTcp::setError = ERROR_NOT_FOUND; // closed between the table read and the call
    const ProcessActionResult gone = closeTcpConnectionOf(fns, HOLDER_PID, CONNECTION);
    EXPECT_FALSE(gone.permissionDenied);
    EXPECT_EQ(gone.errorMessage, "PID 4242 no longer holds this connection");

    FakeTcp::setError = ERROR_INVALID_PARAMETER;
    const ProcessActionResult other = closeTcpConnectionOf(fns, HOLDER_PID, CONNECTION);
    EXPECT_FALSE(other.success);
    EXPECT_FALSE(other.permissionDenied);
    EXPECT_TRUE(other.errorMessage.starts_with("Windows refused to close it: ")) << other.errorMessage;
}

[[nodiscard]] ProcessTarget thisProcess()
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime);
    return {
        .pid = static_cast<std::int32_t>(GetCurrentProcessId()),
        .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
    };
}

/// WindowsProcessActions with the real identity calls and the fake table and SetTcpEntry.
[[nodiscard]] WindowsProcessFunctions actionsFunctions()
{
    WindowsProcessFunctions fns{};
    fns.openProcess = &OpenProcess;
    fns.getProcessTimes = &GetProcessTimes;
    fns.isProcessCritical = &IsProcessCritical;
    fns.getProcessInformation = &GetProcessInformation;
    fns.tcpClose = FakeTcp::functions();
    return fns;
}

TEST_F(WindowsTcpCloseTest, TheActionChecksTheProcessThenClosesItsConnection)
{
    const ProcessTarget self = thisProcess();
    FakeTcp::reset({row(self.pid, MIB_TCP_STATE_ESTAB)});
    WindowsProcessActions actions(actionsFunctions());
    const ProcessActionCapabilities caps = actions.actionCapabilities();
    EXPECT_TRUE(caps.canCloseConnections);
    EXPECT_EQ(caps.connectionCloseKey, ConnectionCloseKey::Tcp4Endpoints);

    const ProcessActionResult closed = actions.closeTcpConnection(self, CONNECTION);
    ASSERT_TRUE(closed.success) << closed.errorMessage;
    EXPECT_EQ(FakeTcp::rowsSet.size(), 1U);

    // A PID reused since the row was listed: refused before the table is read.
    FakeTcp::reset({row(self.pid, MIB_TCP_STATE_ESTAB)});
    const ProcessActionResult reused = actions.closeTcpConnection({.pid = self.pid, .startTimeTicks = self.startTimeTicks + 1}, CONNECTION);
    EXPECT_FALSE(reused.success);
    EXPECT_EQ(FakeTcp::tableCalls, 0);

    // The System process's sockets are the kernel's: refused before anything is opened.
    const ProcessActionResult system = actions.closeTcpConnection({.pid = 4, .startTimeTicks = 1}, CONNECTION);
    EXPECT_FALSE(system.success);
    EXPECT_EQ(system.errorMessage, "PID 4 is the System process; TaskSmack won't close its connections");
    EXPECT_FALSE(system.permissionDenied);
    EXPECT_FALSE(actions.closeTcpConnection({.pid = 0, .startTimeTicks = 1}, CONNECTION).success);
    EXPECT_EQ(FakeTcp::tableCalls, 0);
    EXPECT_TRUE(FakeTcp::rowsSet.empty());

    // SetTcpEntry's refusal for want of administrator rights comes back as one, for the helper to retry.
    FakeTcp::setError = SET_TCP_ENTRY_NOT_ADMINISTRATOR;
    EXPECT_TRUE(actions.closeTcpConnection(self, CONNECTION).permissionDenied);
}

TEST_F(WindowsTcpCloseTest, WithoutSetTcpEntryThereIsNoCloseConnection)
{
    WindowsProcessFunctions fns = actionsFunctions();
    fns.tcpClose.setTcpEntry = nullptr;
    WindowsProcessActions actions(fns);
    EXPECT_FALSE(actions.actionCapabilities().canCloseConnections);
    const ProcessActionResult result = actions.closeTcpConnection(thisProcess(), CONNECTION);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Closing a connection isn't available on this system");
    EXPECT_EQ(FakeTcp::tableCalls, 0);
    // A test's table without it, as the other suites build theirs, never resets a real connection.
    EXPECT_FALSE(WindowsProcessActions(WindowsProcessFunctions{}).actionCapabilities().canCloseConnections);
    EXPECT_NE(realWindowsProcessFunctions().tcpClose.setTcpEntry, nullptr);
}

/// IsProcessCritical() fakes: the process is critical, or the call fails.
struct FakeCritical
{
    static constexpr DWORD CHECK_ERROR = ERROR_INVALID_HANDLE;

    static BOOL WINAPI critical(HANDLE /*process*/, PBOOL isCritical)
    {
        *isCritical = TRUE;
        return TRUE;
    }
    static BOOL WINAPI fails(HANDLE /*process*/, PBOOL /*isCritical*/)
    {
        SetLastError(CHECK_ERROR);
        return FALSE;
    }
};

// A critical process's connections are left alone, as Linux leaves init's (#3295), and so are those of one whose
// criticality can't be read: refused before the table is read, and never as a permission refusal, since no
// administrator makes it safe.
TEST_F(WindowsTcpCloseTest, ACriticalProcessOrAnUnknownOneIsRefusedWithNoRetry)
{
    const ProcessTarget self = thisProcess();
    FakeTcp::reset({row(self.pid, MIB_TCP_STATE_ESTAB)});
    struct Case
    {
        BOOL(WINAPI* isProcessCritical)(HANDLE, PBOOL);
        std::string message;
    };
    for (const Case& c : {
             Case{
                 .isProcessCritical = &FakeCritical::critical,
                 .message = criticalProcessRefusal(self.pid, GuardedAction::CloseConnection),
             },
             Case{.isProcessCritical = &FakeCritical::fails, .message = criticalCheckFailure(self.pid, FakeCritical::CHECK_ERROR)},
             Case{.isProcessCritical = nullptr, .message = criticalCheckFailure(self.pid, ERROR_CALL_NOT_IMPLEMENTED)},
         })
    {
        WindowsProcessFunctions fns = actionsFunctions();
        fns.isProcessCritical = c.isProcessCritical;
        WindowsProcessActions actions(fns);
        const ProcessActionResult refused = actions.closeTcpConnection(self, CONNECTION);
        EXPECT_FALSE(refused.success) << c.message;
        EXPECT_FALSE(refused.permissionDenied) << c.message << ": no administrator retry";
        EXPECT_EQ(refused.errorMessage, c.message);
        EXPECT_EQ(FakeTcp::tableCalls, 0) << c.message;
        EXPECT_TRUE(FakeTcp::rowsSet.empty()) << c.message;
    }
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

/// A TCP socket closed on scope exit.
class OwnedSocket
{
  public:
    explicit OwnedSocket(SOCKET socket) : m_Socket(socket)
    {}
    ~OwnedSocket()
    {
        if (valid())
        {
            closesocket(m_Socket);
        }
    }
    OwnedSocket(const OwnedSocket&) = delete;
    OwnedSocket& operator=(const OwnedSocket&) = delete;
    OwnedSocket(OwnedSocket&&) = delete;
    OwnedSocket& operator=(OwnedSocket&&) = delete;

    [[nodiscard]] bool valid() const
    {
        return std::cmp_not_equal(m_Socket, INVALID_SOCKET);
    }
    [[nodiscard]] SOCKET get() const
    {
        return m_Socket;
    }
    /// The port it is bound to, host order; 0 on failure.
    [[nodiscard]] std::uint16_t localPort() const
    {
        sockaddr_in address{};
        int length = sizeof(address);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Winsock takes the generic sockaddr
        if (::getsockname(m_Socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        {
            return 0;
        }
        return ntohs(address.sin_port);
    }

  private:
    SOCKET m_Socket = INVALID_SOCKET;
};

[[nodiscard]] sockaddr_in loopback(std::uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

// A real loopback connection of this process, reset through SetTcpEntry (#3358). In the CiRealSystem group with
// the other real OS paths: it runs only on a disposable runner.
class CiRealSystemWindowsCloseConnectionTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemWindowsCloseConnectionTest, ResetsThisProcesssLoopbackConnection)
{
    const WinsockSession winsock;
    ASSERT_TRUE(winsock.started());
    const OwnedSocket listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    ASSERT_TRUE(listener.valid());
    const sockaddr_in any = loopback(0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Winsock takes the generic sockaddr
    ASSERT_EQ(::bind(listener.get(), reinterpret_cast<const sockaddr*>(&any), sizeof(any)), 0);
    ASSERT_EQ(::listen(listener.get(), 4), 0);
    const std::uint16_t serverPort = listener.localPort();
    ASSERT_NE(serverPort, 0);
    const OwnedSocket client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    ASSERT_TRUE(client.valid());
    const sockaddr_in server = loopback(serverPort);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Winsock takes the generic sockaddr
    ASSERT_EQ(::connect(client.get(), reinterpret_cast<const sockaddr*>(&server), sizeof(server)), 0);
    const OwnedSocket accepted(::accept(listener.get(), nullptr, nullptr));
    ASSERT_TRUE(accepted.valid());
    const std::uint16_t clientPort = client.localPort();
    ASSERT_NE(clientPort, 0);
    const DWORD timeoutMs = 5000; // a reset that never comes fails the test rather than hang it
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - setsockopt takes the option as bytes
    ASSERT_EQ(::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)), 0);

    WindowsProcessActions actions;
    ASSERT_TRUE(actions.actionCapabilities().canCloseConnections);
    const TcpConnectionToClose connection{
        .localAddress = {127, 0, 0, 1},
        .localPort = clientPort,
        .remoteAddress = {127, 0, 0, 1},
        .remotePort = serverPort,
    };
    const ProcessActionResult result = actions.closeTcpConnection(thisProcess(), connection);
    if (!isProcessElevated())
    {
        // Only an administrator may set a connection's state: a refusal the elevated helper can retry.
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.permissionDenied) << result.errorMessage;
        std::cout << "[   NOTE   ] not elevated on this runner: SetTcpEntry refused, as expected\n";
        return;
    }
    ASSERT_TRUE(result.success) << result.errorMessage;

    std::array<char, 16> buffer{};
    const int received = ::recv(client.get(), buffer.data(), static_cast<int>(buffer.size()), 0);
    const int error = WSAGetLastError();
    EXPECT_EQ(received, SOCKET_ERROR) << "the reset connection still reads";
    EXPECT_NE(error, WSAETIMEDOUT) << "the connection wasn't reset";

    // It is gone from the table, so a second close is refused.
    const ProcessActionResult again = actions.closeTcpConnection(thisProcess(), connection);
    EXPECT_FALSE(again.success);
    EXPECT_FALSE(again.permissionDenied) << again.errorMessage;
}

} // namespace
} // namespace Platform::Windows
