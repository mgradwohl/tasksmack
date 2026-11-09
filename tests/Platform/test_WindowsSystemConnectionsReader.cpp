/// @file test_WindowsSystemConnectionsReader.cpp
/// @brief Platform::Windows::WindowsSystemConnectionsReader (#3356), the Connections tab's reader, against fake
/// iphlpapi and ntdll calls: every row of the four owner-PID tables kept with its PID, IPv4 and IPv6, TCP states and
/// UDP as UNCONN; each holder's name and creation time from the process snapshot; a table that grows between the
/// size query and the copy; a failed table left out, and every table failing as a failure; TCP EStats counters when
/// elevated, the reason when not, and when an elevated read is refused. Plus a CiRealSystem test that opens a
/// loopback TCP listener and client in this process and finds both, held by this PID.

#include "Domain/ProcessConnections.h"
#include "Platform/IProcessConnections.h"
#include "Platform/ISystemConnections.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsSocketTables.h"
#include "Platform/Windows/WindowsSystemConnectionsReader.h"
#include "Support/CiRealSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - Winsock and NT: include-cleaner lacks mappings for sockaddr_in, htons, ...
#include <winsock2.h>
#include <winternl.h>
#include <ws2tcpip.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform::Windows
{
namespace
{

constexpr DWORD BROWSER_PID = 4242;
constexpr DWORD SERVER_PID = 777;
constexpr DWORD GONE_PID = 999; // in the tables, not in the snapshots
constexpr std::uint64_t BROWSER_START = 0x01DB'0000'1234'5678ULL;
constexpr std::uint64_t SERVER_START = 0x01DB'0000'8765'4321ULL;
constexpr LONG STATUS_INFO_LENGTH_MISMATCH_NT = static_cast<LONG>(0xC0000004L);
constexpr LONG STATUS_ACCESS_DENIED_NT = static_cast<LONG>(0xC0000022L);

[[nodiscard]] DWORD tablePort(std::uint16_t port)
{
    return htons(port);
}

[[nodiscard]] DWORD tableAddress(std::array<std::uint8_t, 4> bytes)
{
    DWORD address = 0;
    std::memcpy(&address, bytes.data(), sizeof(address));
    return address;
}

constexpr std::array<std::uint8_t, 16> V6_LOCAL{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
constexpr std::array<std::uint8_t, 16> V6_REMOTE{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};

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
    row.dwLocalAddr = tableAddress({0, 0, 0, 0});
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

struct FakeTable
{
    std::vector<std::byte> bytes;
    DWORD error = NO_ERROR;
    std::size_t growOnFirstCall = 0;
    int calls = 0;
};

/// One process in the fake snapshot.
struct FakeProcess
{
    DWORD pid = 0;
    std::uint64_t createTime = 0;
    std::wstring name;
};

/// The answers the fakes give, set per test.
struct FakeSystem
{
    FakeTable tcp4;
    FakeTable tcp6;
    FakeTable udp4;
    FakeTable udp6;
    std::vector<FakeProcess> processes;
    std::vector<FakeProcess> processesAfter; // what snapshots list from the read's second on, if set (#3573)
    LONG snapshotStatus = 0;                 // non-zero: the snapshot fails with it
    int snapshots = 0;                       // snapshots taken
    bool elevated = false;
    ULONG setStatus = NO_ERROR;
    ULONG getStatus = NO_ERROR;
    std::map<std::uint16_t, std::pair<std::uint64_t, std::uint64_t>> bytesByLocalPort; // port -> {out, in}
    int sets = 0;
    int gets = 0;
};

FakeSystem& fake()
{
    static FakeSystem instance;
    return instance;
}

DWORD answer(FakeTable& table, PVOID data, PDWORD size)
{
    ++table.calls;
    if (table.error != NO_ERROR)
    {
        return table.error;
    }
    if (table.calls == 2 && table.growOnFirstCall > 0)
    {
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

DWORD WINAPI fakeGetExtendedTcpTable(PVOID data, PDWORD size, BOOL order, ULONG family, TCP_TABLE_CLASS tableClass, ULONG /*reserved*/)
{
    EXPECT_EQ(order, FALSE);
    EXPECT_EQ(tableClass, TCP_TABLE_OWNER_PID_ALL);
    return answer(family == AF_INET6 ? fake().tcp6 : fake().tcp4, data, size);
}

DWORD WINAPI fakeGetExtendedUdpTable(PVOID data, PDWORD size, BOOL order, ULONG family, UDP_TABLE_CLASS tableClass, ULONG /*reserved*/)
{
    EXPECT_EQ(order, FALSE);
    EXPECT_EQ(tableClass, UDP_TABLE_OWNER_PID);
    return answer(family == AF_INET6 ? fake().udp6 : fake().udp4, data, size);
}

/// The bytes one snapshot entry takes: the record, then its name, rounded up to 8.
[[nodiscard]] std::size_t entryBytes(const FakeProcess& process)
{
    const std::size_t bytes = sizeof(SYSTEM_PROCESS_INFORMATION) + (process.name.size() * sizeof(wchar_t));
    return (bytes + 7U) & ~std::size_t{7};
}

/// Writes @p processes into @p out as SystemProcessInformation lays them out, names pointing into @p out.
void writeSnapshot(std::span<std::byte> out, const std::vector<FakeProcess>& processes)
{
    std::size_t offset = 0;
    for (std::size_t i = 0; i < processes.size(); ++i)
    {
        const FakeProcess& process = processes[i];
        SYSTEM_PROCESS_INFORMATION entry{};
        const bool last = i + 1 == processes.size();
        entry.NextEntryOffset = last ? 0 : static_cast<ULONG>(entryBytes(process));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr) - a PID is a HANDLE-sized integer
        entry.UniqueProcessId = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(process.pid));
        std::byte* name = out.subspan(offset + sizeof(SYSTEM_PROCESS_INFORMATION)).data();
        entry.ImageName.Length = static_cast<USHORT>(process.name.size() * sizeof(wchar_t));
        entry.ImageName.MaximumLength = entry.ImageName.Length;
        entry.ImageName.Buffer =
            process.name.empty() ? nullptr : reinterpret_cast<PWSTR>(name); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        std::memcpy(out.subspan(offset).data(), &entry, sizeof(entry));
        const LARGE_INTEGER createTime{.QuadPart = static_cast<LONGLONG>(process.createTime)};
        std::memcpy(out.subspan(offset + 32).data(), &createTime, sizeof(createTime));
        std::memcpy(name, process.name.data(), process.name.size() * sizeof(wchar_t));
        offset += entryBytes(process);
    }
}

LONG NTAPI fakeNtQuerySystemInformation(ULONG infoClass, PVOID data, ULONG length, PULONG returned)
{
    EXPECT_EQ(infoClass, 5U); // SystemProcessInformation
    if (fake().snapshotStatus != 0)
    {
        return fake().snapshotStatus;
    }
    const std::vector<FakeProcess>& processes =
        (fake().snapshots > 0 && !fake().processesAfter.empty()) ? fake().processesAfter : fake().processes;
    std::size_t needed = 0;
    for (const FakeProcess& process : processes)
    {
        needed += entryBytes(process);
    }
    *returned = static_cast<ULONG>(needed);
    if (length < needed)
    {
        return STATUS_INFO_LENGTH_MISMATCH_NT;
    }
    writeSnapshot(std::span<std::byte>(static_cast<std::byte*>(data), length), processes);
    ++fake().snapshots;
    return 0;
}

bool fakeIsElevated()
{
    return fake().elevated;
}

ULONG estatsRead(std::uint16_t localPort, PUCHAR rw, ULONG rwSize, PUCHAR rod, ULONG rodSize)
{
    ++fake().gets;
    EXPECT_EQ(rwSize, sizeof(TCP_ESTATS_DATA_RW_v0));
    EXPECT_EQ(rodSize, sizeof(TCP_ESTATS_DATA_ROD_v0));
    if (fake().getStatus != NO_ERROR)
    {
        return fake().getStatus;
    }
    TCP_ESTATS_DATA_RW_v0 state{};
    state.EnableCollection = TRUE;
    std::memcpy(rw, &state, sizeof(state));
    TCP_ESTATS_DATA_ROD_v0 counters{};
    if (const auto found = fake().bytesByLocalPort.find(localPort); found != fake().bytesByLocalPort.end())
    {
        counters.DataBytesOut = found->second.first;
        counters.DataBytesIn = found->second.second;
    }
    std::memcpy(rod, &counters, sizeof(counters));
    return NO_ERROR;
}

ULONG WINAPI fakeGetEStats(PMIB_TCPROW row,
                           TCP_ESTATS_TYPE type,
                           PUCHAR rw,
                           ULONG /*rwVersion*/,
                           ULONG rwSize,
                           PUCHAR /*ros*/,
                           ULONG /*rosVersion*/,
                           ULONG /*rosSize*/,
                           PUCHAR rod,
                           ULONG /*rodVersion*/,
                           ULONG rodSize)
{
    EXPECT_EQ(type, TcpConnectionEstatsData);
    return estatsRead(toHostPort(row->dwLocalPort), rw, rwSize, rod, rodSize);
}

ULONG WINAPI fakeGetEStats6(PMIB_TCP6ROW row,
                            TCP_ESTATS_TYPE type,
                            PUCHAR rw,
                            ULONG /*rwVersion*/,
                            ULONG rwSize,
                            PUCHAR /*ros*/,
                            ULONG /*rosVersion*/,
                            ULONG /*rosSize*/,
                            PUCHAR rod,
                            ULONG /*rodVersion*/,
                            ULONG rodSize)
{
    EXPECT_EQ(type, TcpConnectionEstatsData);
    return estatsRead(toHostPort(row->dwLocalPort), rw, rwSize, rod, rodSize);
}

ULONG WINAPI fakeSetEStats(PMIB_TCPROW /*row*/, TCP_ESTATS_TYPE type, PUCHAR rw, ULONG /*rwVersion*/, ULONG rwSize, ULONG /*offset*/)
{
    ++fake().sets;
    EXPECT_EQ(type, TcpConnectionEstatsData);
    EXPECT_EQ(rwSize, sizeof(TCP_ESTATS_DATA_RW_v0));
    TCP_ESTATS_DATA_RW_v0 asked{};
    std::memcpy(&asked, rw, sizeof(asked));
    EXPECT_NE(asked.EnableCollection, FALSE);
    return fake().setStatus;
}

ULONG WINAPI fakeSetEStats6(PMIB_TCP6ROW /*row*/, TCP_ESTATS_TYPE type, PUCHAR rw, ULONG version, ULONG rwSize, ULONG offset)
{
    return fakeSetEStats(nullptr, type, rw, version, rwSize, offset);
}

[[nodiscard]] SystemConnectionsFunctions fakeFunctions()
{
    SystemConnectionsFunctions api;
    api.tables = {.getExtendedTcpTable = &fakeGetExtendedTcpTable, .getExtendedUdpTable = &fakeGetExtendedUdpTable};
    api.ntQuerySystemInformation = &fakeNtQuerySystemInformation;
    api.isElevated = &fakeIsElevated;
    api.getPerTcpConnectionEStats = &fakeGetEStats;
    api.setPerTcpConnectionEStats = &fakeSetEStats;
    api.getPerTcp6ConnectionEStats = &fakeGetEStats6;
    api.setPerTcp6ConnectionEStats = &fakeSetEStats6;
    return api;
}

[[nodiscard]] const SystemConnection*
findLocal(const SystemConnectionsRead& read, ConnectionProtocol protocol, ConnectionFamily family, std::uint16_t port)
{
    const auto found = std::ranges::find_if(
        read.connections,
        [&](const SystemConnection& c)
        { return c.connection.protocol == protocol && c.connection.family == family && c.connection.local.port == port; });
    return found == read.connections.end() ? nullptr : &*found;
}

class WindowsSystemConnectionsReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeSystem{};
        fake().tcp4.bytes = makeTable<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>({
            tcp4Row(BROWSER_PID, MIB_TCP_STATE_ESTAB, 52144, 443),
            tcp4Row(SERVER_PID, MIB_TCP_STATE_LISTEN, 8080),
            tcp4Row(0, MIB_TCP_STATE_TIME_WAIT, 52145, 443),
            tcp4Row(GONE_PID, MIB_TCP_STATE_CLOSE_WAIT, 52146, 80),
        });
        fake().tcp6.bytes = makeTable<MIB_TCP6TABLE_OWNER_PID, MIB_TCP6ROW_OWNER_PID>({
            tcp6Row(BROWSER_PID, MIB_TCP_STATE_ESTAB, 52200, 443),
        });
        fake().udp4.bytes = makeTable<MIB_UDPTABLE_OWNER_PID, MIB_UDPROW_OWNER_PID>({udp4Row(SERVER_PID, 5353)});
        fake().udp6.bytes = makeTable<MIB_UDP6TABLE_OWNER_PID, MIB_UDP6ROW_OWNER_PID>({udp6Row(BROWSER_PID, 5355)});
        fake().processes = {
            {.pid = 0, .createTime = 0, .name = L""},
            {.pid = 4, .createTime = 0, .name = L"System"},
            {.pid = SERVER_PID, .createTime = SERVER_START, .name = L"svchost.exe"},
            {.pid = BROWSER_PID, .createTime = BROWSER_START, .name = L"msedge.exe"},
        };
        fake().bytesByLocalPort = {{52144, {1000, 5000}}, {52200, {200, 300}}};
    }

    WindowsSystemConnectionsReader m_Reader{fakeFunctions()};
};

TEST_F(WindowsSystemConnectionsReaderTest, ListsEveryRowOfTheFourTablesWithItsHolder)
{
    EXPECT_TRUE(m_Reader.hasConnections());
    const SystemConnectionsRead read = m_Reader.read();
    ASSERT_EQ(read.status, SystemConnectionsStatus::Ok) << read.detail;
    EXPECT_EQ(read.connections.size(), 7U); // every process's rows, not one process's

    const SystemConnection* browser = findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52144);
    ASSERT_NE(browser, nullptr);
    EXPECT_EQ(browser->pid, static_cast<std::int32_t>(BROWSER_PID));
    EXPECT_EQ(browser->processName, "msedge.exe");
    EXPECT_EQ(browser->startTimeTicks, BROWSER_START);
    EXPECT_EQ(browser->connection.state, ConnectionState::Established);
    EXPECT_EQ(browser->connection.remote.port, 443);
    const std::array<std::uint8_t, 4> remote{93, 184, 216, 34};
    EXPECT_TRUE(std::equal(remote.begin(), remote.end(), browser->connection.remote.address.begin())); // network order

    const SystemConnection* listener = findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 8080);
    ASSERT_NE(listener, nullptr);
    EXPECT_EQ(listener->connection.state, ConnectionState::Listen);
    EXPECT_EQ(listener->processName, "svchost.exe");
    EXPECT_EQ(listener->startTimeTicks, SERVER_START);

    const SystemConnection* closing = findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52145);
    ASSERT_NE(closing, nullptr);
    EXPECT_EQ(closing->connection.state, ConnectionState::TimeWait);
    EXPECT_EQ(closing->pid, 0); // no process holds it
    EXPECT_TRUE(closing->processName.empty());

    // A holder the snapshots don't list (it exited): nobody's, so the row selects no stranger (#3573).
    const SystemConnection* gone = findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52146);
    ASSERT_NE(gone, nullptr);
    EXPECT_EQ(gone->connection.state, ConnectionState::CloseWait);
    EXPECT_EQ(gone->pid, 0);
    EXPECT_TRUE(gone->processName.empty());
    EXPECT_EQ(gone->startTimeTicks, 0U);
    EXPECT_EQ(fake().snapshots, 2); // one either side of the tables

    const SystemConnection* v6 = findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv6, 52200);
    ASSERT_NE(v6, nullptr);
    EXPECT_EQ(v6->connection.local.address, V6_LOCAL);
    EXPECT_EQ(v6->connection.remote.address, V6_REMOTE);
    EXPECT_EQ(v6->processName, "msedge.exe");

    // UDP: bound, no remote end, UNCONN (Closed), as in Process Details.
    const SystemConnection* udp = findLocal(read, ConnectionProtocol::Udp, ConnectionFamily::IPv4, 5353);
    ASSERT_NE(udp, nullptr);
    EXPECT_EQ(udp->connection.state, ConnectionState::Closed);
    EXPECT_EQ(udp->connection.remote.port, 0);
    EXPECT_EQ(udp->pid, static_cast<std::int32_t>(SERVER_PID));
    const SystemConnection* udp6 = findLocal(read, ConnectionProtocol::Udp, ConnectionFamily::IPv6, 5355);
    ASSERT_NE(udp6, nullptr);
    EXPECT_EQ(udp6->connection.local.address, V6_LOCAL);

    // Windows has no socket inodes: Close connection names a connection by its endpoints (#3358).
    EXPECT_TRUE(std::ranges::all_of(read.connections, [](const SystemConnection& c) { return c.socketInode == 0; }));
}

TEST_F(WindowsSystemConnectionsReaderTest, NotElevatedNoEStatsCallIsMadeAndTheReadSaysWhy)
{
    const SystemConnectionsRead read = m_Reader.read();
    ASSERT_EQ(read.status, SystemConnectionsStatus::Ok);
    EXPECT_EQ(read.ratesUnavailable, RATES_NEED_ADMINISTRATOR);
    EXPECT_TRUE(std::ranges::none_of(read.connections, [](const SystemConnection& c) { return c.hasByteCounters; }));
    EXPECT_EQ(fake().sets, 0);
    EXPECT_EQ(fake().gets, 0);
}

TEST_F(WindowsSystemConnectionsReaderTest, ElevatedEstablishedConnectionsGetEStatsCounters)
{
    fake().elevated = true;
    const SystemConnectionsRead read = m_Reader.read();
    ASSERT_EQ(read.status, SystemConnectionsStatus::Ok);
    EXPECT_TRUE(read.ratesUnavailable.empty());

    const SystemConnection* v4 = findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52144);
    ASSERT_NE(v4, nullptr);
    EXPECT_TRUE(v4->hasByteCounters);
    EXPECT_EQ(v4->bytesSent, 1000U);
    EXPECT_EQ(v4->bytesReceived, 5000U);
    const SystemConnection* v6 = findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv6, 52200);
    ASSERT_NE(v6, nullptr);
    EXPECT_TRUE(v6->hasByteCounters);
    EXPECT_EQ(v6->bytesSent, 200U);
    EXPECT_EQ(v6->bytesReceived, 300U);

    // Only ESTABLISHED connections are read: a listener, TIME_WAIT, CLOSE_WAIT and UDP show the dash.
    EXPECT_FALSE(findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 8080)->hasByteCounters);
    EXPECT_FALSE(findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52145)->hasByteCounters);
    EXPECT_FALSE(findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52146)->hasByteCounters);
    EXPECT_FALSE(findLocal(read, ConnectionProtocol::Udp, ConnectionFamily::IPv4, 5353)->hasByteCounters);
    EXPECT_EQ(fake().sets, 2);
    EXPECT_EQ(fake().gets, 2);

    // Collection stays on: the next read only reads (#1418).
    fake().bytesByLocalPort[52144] = {3000, 9000};
    const SystemConnectionsRead next = m_Reader.read();
    EXPECT_EQ(fake().sets, 2);
    EXPECT_EQ(fake().gets, 4);
    EXPECT_EQ(findLocal(next, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52144)->bytesSent, 3000U);
}

// #3573: a refusal after reads that worked is reported too: every sample is judged, not only until the first success.
TEST_F(WindowsSystemConnectionsReaderTest, ARefusalAfterReadsThatWorkedIsReported)
{
    fake().elevated = true;
    const SystemConnectionsRead worked = m_Reader.read();
    EXPECT_TRUE(worked.ratesUnavailable.empty());
    EXPECT_TRUE(findLocal(worked, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52144)->hasByteCounters);

    fake().getStatus = ERROR_ACCESS_DENIED; // a policy applied since
    const SystemConnectionsRead refused = m_Reader.read();
    EXPECT_EQ(refused.ratesUnavailable, RATES_REFUSED);
    EXPECT_TRUE(std::ranges::none_of(refused.connections, [](const SystemConnection& c) { return c.hasByteCounters; }));

    const int gets = fake().gets;
    EXPECT_EQ(m_Reader.read().ratesUnavailable, RATES_REFUSED);
    EXPECT_EQ(fake().gets, gets); // and then no longer asked
}

TEST_F(WindowsSystemConnectionsReaderTest, ElevatedButRefusedSaysSoAndStopsAsking)
{
    fake().elevated = true;
    fake().setStatus = ERROR_ACCESS_DENIED;
    fake().getStatus = ERROR_ACCESS_DENIED;
    const SystemConnectionsRead read = m_Reader.read();
    ASSERT_EQ(read.status, SystemConnectionsStatus::Ok);
    EXPECT_EQ(read.ratesUnavailable, RATES_REFUSED);
    EXPECT_TRUE(std::ranges::none_of(read.connections, [](const SystemConnection& c) { return c.hasByteCounters; }));

    const int sets = fake().sets;
    const int gets = fake().gets;
    const SystemConnectionsRead next = m_Reader.read();
    EXPECT_EQ(next.ratesUnavailable, RATES_REFUSED);
    EXPECT_EQ(fake().sets, sets); // refused once is refused for good
    EXPECT_EQ(fake().gets, gets);
    EXPECT_EQ(next.connections.size(), 7U); // the sockets are still listed
}

TEST_F(WindowsSystemConnectionsReaderTest, ATableThatGrowsBetweenTheSizeQueryAndTheCopyIsRetried)
{
    // Bigger than the first buffer, and growing again by the second call.
    std::vector<MIB_TCPROW_OWNER_PID> rows(1000, tcp4Row(SERVER_PID, MIB_TCP_STATE_LISTEN, 9000));
    rows.push_back(tcp4Row(BROWSER_PID, MIB_TCP_STATE_ESTAB, 52144, 443));
    fake().tcp4.bytes = makeTable<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>(rows);
    fake().tcp4.growOnFirstCall = 16384; // past the first grown buffer, too
    const SystemConnectionsRead read = m_Reader.read();
    ASSERT_EQ(read.status, SystemConnectionsStatus::Ok) << read.detail;
    EXPECT_GE(fake().tcp4.calls, 3);
    EXPECT_NE(findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52144), nullptr);
    EXPECT_EQ(read.connections.size(), 1001U + 3U);
}

TEST_F(WindowsSystemConnectionsReaderTest, AFailedTableIsLeftOutAndEveryTableFailingIsAFailure)
{
    fake().udp4.error = ERROR_ACCESS_DENIED;
    const SystemConnectionsRead partial = m_Reader.read();
    ASSERT_EQ(partial.status, SystemConnectionsStatus::Ok);
    EXPECT_EQ(findLocal(partial, ConnectionProtocol::Udp, ConnectionFamily::IPv4, 5353), nullptr);
    EXPECT_NE(findLocal(partial, ConnectionProtocol::Udp, ConnectionFamily::IPv6, 5355), nullptr);

    fake().tcp4.error = ERROR_ACCESS_DENIED;
    fake().tcp6.error = ERROR_GEN_FAILURE;
    fake().udp6.error = ERROR_ACCESS_DENIED;
    const SystemConnectionsRead failed = m_Reader.read();
    EXPECT_EQ(failed.status, SystemConnectionsStatus::Failed);
    EXPECT_TRUE(failed.connections.empty());
    EXPECT_FALSE(failed.detail.empty()); // the system's message for the last error
}

TEST_F(WindowsSystemConnectionsReaderTest, AFamilyWindowsDoesNotRunReadsAsEmpty)
{
    fake().tcp6.error = ERROR_NOT_SUPPORTED;
    fake().udp6.error = ERROR_NOT_SUPPORTED;
    fake().tcp4.error = ERROR_ACCESS_DENIED;
    fake().udp4.error = ERROR_ACCESS_DENIED;
    // No table had a socket, but two were read (empty): not a failure.
    const SystemConnectionsRead read = m_Reader.read();
    EXPECT_EQ(read.status, SystemConnectionsStatus::Ok);
    EXPECT_TRUE(read.connections.empty());
}

// #3573: a holder that exited while the tables were read, its PID reused by another process before the second
// snapshot, is withheld (PID 0, no name or identity); a holder the same in both snapshots keeps its identity.
TEST_F(WindowsSystemConnectionsReaderTest, APidReusedWhileTheTablesWereReadNamesNobody)
{
    fake().processesAfter = fake().processes;
    for (FakeProcess& process : fake().processesAfter)
    {
        if (process.pid == SERVER_PID)
        {
            process.createTime = SERVER_START + 1; // a new process under the old PID
            process.name = L"stranger.exe";
        }
    }
    const SystemConnectionsRead read = m_Reader.read();
    ASSERT_EQ(read.status, SystemConnectionsStatus::Ok);
    for (const SystemConnection* reused : {
             findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 8080),
             findLocal(read, ConnectionProtocol::Udp, ConnectionFamily::IPv4, 5353),
         })
    {
        ASSERT_NE(reused, nullptr);
        EXPECT_EQ(reused->pid, 0);
        EXPECT_TRUE(reused->processName.empty());
        EXPECT_EQ(reused->startTimeTicks, 0U);
    }
    const SystemConnection* browser = findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52144);
    ASSERT_NE(browser, nullptr);
    EXPECT_EQ(browser->pid, static_cast<std::int32_t>(BROWSER_PID));
    EXPECT_EQ(browser->startTimeTicks, BROWSER_START);
    EXPECT_EQ(browser->processName, "msedge.exe");
}

// #3573: the same link-local endpoints on two interfaces are two sockets, kept apart by their scope IDs.
TEST_F(WindowsSystemConnectionsReaderTest, ScopeIdsKeepLinkLocalConnectionsOnTwoInterfacesApart)
{
    constexpr std::array<std::uint8_t, 16> LINK_LOCAL{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    std::vector<MIB_TCP6ROW_OWNER_PID> rows(2, tcp6Row(BROWSER_PID, MIB_TCP_STATE_ESTAB, 52300, 445));
    for (MIB_TCP6ROW_OWNER_PID& row : rows)
    {
        std::ranges::copy(LINK_LOCAL, std::begin(row.ucLocalAddr));
        std::ranges::copy(LINK_LOCAL, std::begin(row.ucRemoteAddr));
    }
    rows[0].dwLocalScopeId = 12;
    rows[0].dwRemoteScopeId = 12;
    rows[1].dwLocalScopeId = 7;
    rows[1].dwRemoteScopeId = 7;
    fake().tcp6.bytes = makeTable<MIB_TCP6TABLE_OWNER_PID, MIB_TCP6ROW_OWNER_PID>(rows);
    const SystemConnectionsRead read = m_Reader.read();
    std::vector<const SystemConnection*> linkLocal;
    for (const SystemConnection& c : read.connections)
    {
        if (c.connection.family == ConnectionFamily::IPv6 && c.connection.local.port == 52300)
        {
            linkLocal.push_back(&c);
        }
    }
    ASSERT_EQ(linkLocal.size(), 2U);
    EXPECT_EQ(linkLocal[0]->connection.local.scopeId, 12U);
    EXPECT_EQ(linkLocal[0]->connection.remote.scopeId, 12U);
    EXPECT_EQ(linkLocal[1]->connection.local.scopeId, 7U);
    EXPECT_NE(Domain::Connections::socketKey(linkLocal[0]->connection), Domain::Connections::socketKey(linkLocal[1]->connection));
}

TEST_F(WindowsSystemConnectionsReaderTest, AFailedSnapshotLeavesThePidsWithoutNames)
{
    fake().snapshotStatus = STATUS_ACCESS_DENIED_NT;
    const SystemConnectionsRead read = m_Reader.read();
    ASSERT_EQ(read.status, SystemConnectionsStatus::Ok);
    const SystemConnection* browser = findLocal(read, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, 52144);
    ASSERT_NE(browser, nullptr);
    EXPECT_EQ(browser->pid, static_cast<std::int32_t>(BROWSER_PID));
    EXPECT_TRUE(browser->processName.empty());
    EXPECT_EQ(browser->startTimeTicks, 0U);
}

TEST(WindowsSnapshotIdentitiesTest, ANameOutsideTheBytesIsLeftEmptyAndAShortChainEnds)
{
    const std::vector<FakeProcess> processes{
        {.pid = SERVER_PID, .createTime = SERVER_START, .name = L"svchost.exe"},
        {.pid = BROWSER_PID, .createTime = BROWSER_START, .name = L"msedge.exe"},
    };
    std::vector<std::byte> bytes(entryBytes(processes[0]) + entryBytes(processes[1]));
    writeSnapshot(bytes, processes);
    const auto all = snapshotIdentities(bytes);
    ASSERT_EQ(all.size(), 2U);
    EXPECT_EQ(all.at(BROWSER_PID).name, "msedge.exe");
    EXPECT_EQ(all.at(SERVER_PID).createTime, SERVER_START);

    // Only the first entry's bytes: the second is past the end and isn't read.
    const auto first = snapshotIdentities(std::span<const std::byte>(bytes).first(entryBytes(processes[0])));
    EXPECT_EQ(first.size(), 1U);
    EXPECT_TRUE(first.contains(SERVER_PID));

    // The record's bytes alone: its name, after them, is outside the snapshot.
    const auto nameless = snapshotIdentities(std::span<const std::byte>(bytes).first(sizeof(SYSTEM_PROCESS_INFORMATION)));
    ASSERT_EQ(nameless.size(), 1U);
    EXPECT_TRUE(nameless.at(SERVER_PID).name.empty());
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
        if (::getsockname(m_Socket, reinterpret_cast<sockaddr*>(&address), &length) !=
            0) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
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

/// The row of this process's TCP socket on 127.0.0.1:@p port, or null.
[[nodiscard]] const SystemConnection* findOwn(const SystemConnectionsRead& read, std::uint16_t port, std::uint16_t remotePort)
{
    const auto self = static_cast<std::int32_t>(GetCurrentProcessId());
    const auto found = std::ranges::find_if(read.connections,
                                            [&](const SystemConnection& c)
                                            {
                                                return c.pid == self && c.connection.protocol == ConnectionProtocol::Tcp &&
                                                       c.connection.family == ConnectionFamily::IPv4 && c.connection.local.port == port &&
                                                       c.connection.remote.port == remotePort;
                                            });
    return found == read.connections.end() ? nullptr : &*found;
}

// A real loopback listener and client of this process, on the system's tables (#3356). In the CiRealSystem
// group with the other real OS paths; it runs only on a disposable runner.
class CiRealSystemWindowsConnectionsTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemWindowsConnectionsTest, SeesThisProcesssLoopbackListenerAndClient)
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

    WindowsSystemConnectionsReader reader;
    const SystemConnectionsRead first = reader.read();
    ASSERT_EQ(first.status, SystemConnectionsStatus::Ok) << first.detail;

    const SystemConnection* listening = findOwn(first, serverPort, 0);
    ASSERT_NE(listening, nullptr) << "listener on 127.0.0.1:" << serverPort << " not found";
    // Listen, or Unknown on a host whose table reports state 0 for listeners (#3257).
    EXPECT_TRUE(listening->connection.state == ConnectionState::Listen || listening->connection.state == ConnectionState::Unknown);
    const SystemConnection* connected = findOwn(first, clientPort, serverPort);
    ASSERT_NE(connected, nullptr) << "client 127.0.0.1:" << clientPort << " -> " << serverPort << " not found";
    EXPECT_EQ(connected->connection.state, ConnectionState::Established);
    EXPECT_NE(findOwn(first, serverPort, clientPort), nullptr) << "the accepted end not found";
    EXPECT_FALSE(connected->processName.empty());
    EXPECT_NE(connected->startTimeTicks, 0U);

    // Rates: TCP EStats counts the client's bytes when this process is elevated, else the read says why.
    const std::array<char, 4096> payload{};
    ASSERT_EQ(::send(client.get(), payload.data(), static_cast<int>(payload.size()), 0), static_cast<int>(payload.size()));
    std::array<char, 4096> received{};
    int got = 0;
    while (std::cmp_less(got, received.size()))
    {
        const int n = ::recv(accepted.get(), received.data(), static_cast<int>(received.size()) - got, 0);
        ASSERT_GT(n, 0);
        got += n;
    }
    const SystemConnectionsRead second = reader.read();
    ASSERT_EQ(second.status, SystemConnectionsStatus::Ok) << second.detail;
    const SystemConnection* after = findOwn(second, clientPort, serverPort);
    ASSERT_NE(after, nullptr);
    if (!isProcessElevated())
    {
        EXPECT_EQ(second.ratesUnavailable, RATES_NEED_ADMINISTRATOR);
        EXPECT_FALSE(after->hasByteCounters);
    }
    else if (after->hasByteCounters)
    {
        EXPECT_GE(after->bytesSent, payload.size());
    }
    else
    {
        // Elevated but refused (a policy or driver): said so, never shown as zero.
        EXPECT_FALSE(second.ratesUnavailable.empty());
        std::cout << "[   NOTE   ] TCP EStats refused on this runner: " << second.ratesUnavailable << '\n';
    }
}

} // namespace
} // namespace Platform::Windows
