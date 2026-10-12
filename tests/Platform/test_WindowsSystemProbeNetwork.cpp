/// @file test_WindowsSystemProbeNetwork.cpp
/// @brief WindowsSystemProbe's network read against a fake interface table (#1395): a failed GetIfTable2, the
/// name fallbacks, unknown link speeds, and the removed-adapter filter (#1284) with its device-id cache --
/// read once per interface, an empty read not retried every sample, and a LUID Windows gives to a new
/// interface read afresh (#1369 review). A CI runner has no down or removed adapter, so the real table never
/// shows these. The CPU and memory calls stay real; the real registry and configuration-manager readers are
/// checked with ids that can't exist.

#include "Platform/SystemTypes.h"
#include "Platform/Windows/ProcessorPerformanceCounter.h" // IWYU pragma: keep (destroys the null counter)
#include "Platform/Windows/WindowsSystemProbe.h"
#include "Platform/Windows/WindowsSystemProbeMath.h"

#include <gtest/gtest.h>

// clang-format off
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - MIB_IF_ROW2 and its fields come from netioapi.h through iphlpapi.h
namespace Platform
{
namespace
{

constexpr ULONG OPER_STATUS_DOWN = 2; // IfOperStatusDown

/// What the fake interface table and device lookups answer, set per test.
struct FakeNetwork
{
    DWORD tableStatus = NO_ERROR;
    bool nullTable = false;
    std::vector<MIB_IF_ROW2> rows;
    int tablesFreed = 0;
    // Device instance ids by the interface GUID's first field, and how often each was read
    std::map<unsigned long, std::wstring> ids;
    std::map<unsigned long, int> idReads;
    // Presence by device id; a device not listed is Unknown
    std::map<std::wstring, DevicePresence> presence;
    int presenceQueries = 0;
    // The table handed out last; freed by fakeFreeMibTable
    std::vector<std::byte> table;
};

FakeNetwork& fake()
{
    static FakeNetwork state;
    return state;
}

DWORD WINAPI fakeGetIfTable2(MIB_IF_TABLE2** out)
{
    if (fake().tableStatus != NO_ERROR || fake().nullTable)
    {
        *out = nullptr;
        return fake().tableStatus;
    }
    const std::size_t count = fake().rows.size();
    fake().table.assign(offsetof(MIB_IF_TABLE2, Table) + (std::max<std::size_t>(count, 1) * sizeof(MIB_IF_ROW2)), std::byte{0});
    auto* table = reinterpret_cast<MIB_IF_TABLE2*>(fake().table.data());
    table->NumEntries = static_cast<ULONG>(count);
    if (count > 0)
    {
        std::memcpy(fake().table.data() + offsetof(MIB_IF_TABLE2, Table), fake().rows.data(), count * sizeof(MIB_IF_ROW2));
    }
    *out = table;
    return NO_ERROR;
}

void WINAPI fakeFreeMibTable(PVOID memory)
{
    EXPECT_EQ(memory, static_cast<PVOID>(fake().table.data()));
    ++fake().tablesFreed;
}

std::wstring fakeReadAdapterDeviceInstanceId(const GUID& interfaceGuid)
{
    ++fake().idReads[interfaceGuid.Data1];
    const auto found = fake().ids.find(interfaceGuid.Data1);
    return found == fake().ids.end() ? std::wstring{} : found->second;
}

DevicePresence fakeDevicePresence(const std::wstring& deviceInstanceId)
{
    ++fake().presenceQueries;
    if (deviceInstanceId.empty())
    {
        return DevicePresence::Unknown;
    }
    const auto found = fake().presence.find(deviceInstanceId);
    return found == fake().presence.end() ? DevicePresence::Unknown : found->second;
}

/// A counted hardware Ethernet row, up, with @p alias, at LUID and GUID @p id.
[[nodiscard]] MIB_IF_ROW2 row(std::uint64_t id, const wchar_t* alias, ULONG operStatus = IfOperStatusUp)
{
    MIB_IF_ROW2 r{}; // NOLINT(bugprone-invalid-enum-default-initialization) - zeroed, as Win32 rows are
    r.InterfaceLuid.Value = id;
    r.InterfaceIndex = static_cast<NET_IFINDEX>(id);
    r.InterfaceGuid.Data1 = static_cast<unsigned long>(id);
    r.Type = IF_TYPE_ETHERNET_CSMACD;
    r.InterfaceAndOperStatusFlags.HardwareInterface = TRUE;
    r.OperStatus = static_cast<IF_OPER_STATUS>(operStatus);
    r.InOctets = 1'000 * id;
    r.OutOctets = 2'000 * id;
    r.TransmitLinkSpeed = 1'000'000'000ULL;
    wcsncpy_s(static_cast<wchar_t*>(r.Alias), std::size(r.Alias), alias, _TRUNCATE);
    return r;
}

class WindowsSystemProbeNetworkTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeNetwork{};
    }

    void TearDown() override
    {
        fake() = FakeNetwork{};
    }

    /// The real CPU and memory calls, the fake network.
    [[nodiscard]] static WindowsSystemProbe makeProbe()
    {
        WindowsSystemProbeFunctions fns = WindowsSystemProbe::systemFunctions();
        fns.network.getIfTable2 = &fakeGetIfTable2;
        fns.network.freeMibTable = &fakeFreeMibTable;
        fns.network.readAdapterDeviceInstanceId = &fakeReadAdapterDeviceInstanceId;
        fns.network.devicePresence = &fakeDevicePresence;
        return {2'000, nullptr, fns};
    }

    [[nodiscard]] static std::vector<std::string> names(const SystemCounters& counters)
    {
        std::vector<std::string> out;
        std::size_t i = 0;
        while (i < counters.networkInterfaces.size())
        {
            out.push_back(counters.networkInterfaces[i].name);
            ++i;
        }
        return out;
    }
};

} // namespace

TEST_F(WindowsSystemProbeNetworkTest, FailedOrEmptyTableLeavesTheNetworkEmpty)
{
    fake().tableStatus = ERROR_NOT_ENOUGH_MEMORY;
    WindowsSystemProbe probe = makeProbe();
    SystemCounters counters = probe.read();
    EXPECT_TRUE(counters.networkInterfaces.empty());
    EXPECT_EQ(counters.netRxBytes, 0U);
    EXPECT_EQ(counters.netTxBytes, 0U);
    EXPECT_EQ(fake().tablesFreed, 0) << "nothing was handed out to free";

    fake().tableStatus = NO_ERROR;
    fake().nullTable = true; // Success with no table: still nothing read or freed
    counters = probe.read();
    EXPECT_TRUE(counters.networkInterfaces.empty());
    EXPECT_EQ(fake().tablesFreed, 0);

    fake().nullTable = false; // A table with no rows
    counters = probe.read();
    EXPECT_TRUE(counters.networkInterfaces.empty());
    EXPECT_EQ(fake().tablesFreed, 1);
}

TEST_F(WindowsSystemProbeNetworkTest, EveryTableIsFreedOnce)
{
    fake().rows = {row(1, L"Ethernet")};
    WindowsSystemProbe probe = makeProbe();
    (void) probe.read();
    (void) probe.read();
    EXPECT_EQ(fake().tablesFreed, 2);
}

TEST_F(WindowsSystemProbeNetworkTest, NameFallsBackFromAliasToDescriptionToIndex)
{
    MIB_IF_ROW2 described = row(2, L"");
    wcsncpy_s(static_cast<wchar_t*>(described.Description), std::size(described.Description), L"Contoso Gigabit", _TRUNCATE);
    MIB_IF_ROW2 both = row(3, L"Wi-Fi");
    wcsncpy_s(static_cast<wchar_t*>(both.Description), std::size(both.Description), L"Contoso Wireless", _TRUNCATE);
    fake().rows = {row(7, L""), described, both};

    WindowsSystemProbe probe = makeProbe();
    const SystemCounters counters = probe.read();
    ASSERT_EQ(counters.networkInterfaces.size(), 3U);
    EXPECT_EQ(counters.networkInterfaces[0].name, "Interface 7");
    EXPECT_EQ(counters.networkInterfaces[0].displayName, "Interface 7");
    EXPECT_EQ(counters.networkInterfaces[1].name, "Contoso Gigabit");
    EXPECT_EQ(counters.networkInterfaces[1].displayName, "Contoso Gigabit");
    EXPECT_EQ(counters.networkInterfaces[2].name, "Wi-Fi");
    EXPECT_EQ(counters.networkInterfaces[2].displayName, "Contoso Wireless");
}

TEST_F(WindowsSystemProbeNetworkTest, UnknownLinkSpeedsReadAsZero)
{
    MIB_IF_ROW2 zero = row(1, L"Zero");
    zero.TransmitLinkSpeed = 0;
    MIB_IF_ROW2 unknown = row(2, L"Unknown");
    unknown.TransmitLinkSpeed = std::numeric_limits<ULONG64>::max();
    fake().rows = {zero, unknown, row(3, L"Gigabit")};

    WindowsSystemProbe probe = makeProbe();
    const SystemCounters counters = probe.read();
    ASSERT_EQ(counters.networkInterfaces.size(), 3U);
    EXPECT_EQ(counters.networkInterfaces[0].linkSpeedMbps, 0U);
    EXPECT_EQ(counters.networkInterfaces[1].linkSpeedMbps, 0U);
    EXPECT_EQ(counters.networkInterfaces[2].linkSpeedMbps, 1'000U);
    EXPECT_EQ(counters.netRxBytes, 6'000U) << "all three are hardware, so all are in the total";
}

TEST_F(WindowsSystemProbeNetworkTest, DownAdapterWhoseDeviceIsGoneIsLeftOut)
{
    fake().rows = {
        row(1, L"Up"),
        row(2, L"Unplugged", OPER_STATUS_DOWN),
        row(3, L"Phantom", OPER_STATUS_DOWN),
        row(4, L"Removed", IfOperStatusNotPresent),
        row(5, L"Tunnel", OPER_STATUS_DOWN),
    };
    fake().ids = {{2, L"PCI\\VEN_1\\2"}, {3, L"USB\\VID_1\\3"}}; // The tunnel has no PnP device
    fake().presence = {{L"PCI\\VEN_1\\2", DevicePresence::Present}, {L"USB\\VID_1\\3", DevicePresence::Absent}};

    WindowsSystemProbe probe = makeProbe();
    const SystemCounters counters = probe.read();
    EXPECT_EQ(names(counters), (std::vector<std::string>{"Up", "Unplugged", "Tunnel"}));
    // Only the down rows are looked up: an up row is present, a not-present one decides on its own.
    EXPECT_EQ(fake().idReads.count(1), 0U);
    EXPECT_EQ(fake().idReads.count(4), 0U);
    EXPECT_EQ(fake().presenceQueries, 3);
}

TEST_F(WindowsSystemProbeNetworkTest, DeviceIdIsReadOncePerInterface)
{
    fake().rows = {row(2, L"Unplugged", OPER_STATUS_DOWN)};
    fake().ids = {{2, L"PCI\\VEN_1\\2"}};
    fake().presence = {{L"PCI\\VEN_1\\2", DevicePresence::Present}};

    WindowsSystemProbe probe = makeProbe();
    int sample = 0;
    while (sample < 3)
    {
        EXPECT_EQ(probe.read().networkInterfaces.size(), 1U);
        ++sample;
    }
    EXPECT_EQ(fake().idReads[2], 1) << "a found id is kept";
    EXPECT_EQ(fake().presenceQueries, 3) << "presence can change, so it is asked each sample";
}

TEST_F(WindowsSystemProbeNetworkTest, EmptyDeviceIdIsNotRereadEverySample)
{
    fake().rows = {
        row(5, L"Tunnel", OPER_STATUS_DOWN),
    };

    WindowsSystemProbe probe = makeProbe();
    (void) probe.read();
    fake().ids = {{5, L"ROOT\\NET\\5"}}; // Written later: found at the next retry, not the next sample
    fake().presence = {{L"ROOT\\NET\\5", DevicePresence::Absent}};
    const SystemCounters counters = probe.read();
    EXPECT_EQ(fake().idReads[5], 1);
    EXPECT_EQ(names(counters), (std::vector<std::string>{"Tunnel"})) << "an unknown device keeps its row";
}

TEST_F(WindowsSystemProbeNetworkTest, ReusedLuidWithANewGuidRereadsTheId)
{
    const MIB_IF_ROW2 first = row(2, L"Dock", OPER_STATUS_DOWN);
    fake().rows = {first};
    fake().ids = {{2, L"USB\\DOCK\\1"}, {9, L"USB\\DOCK\\9"}};
    fake().presence = {{L"USB\\DOCK\\1", DevicePresence::Present}, {L"USB\\DOCK\\9", DevicePresence::Absent}};

    WindowsSystemProbe probe = makeProbe();
    EXPECT_EQ(probe.read().networkInterfaces.size(), 1U);

    // Windows gives the freed LUID to another interface: a different GUID, a different adapter.
    MIB_IF_ROW2 second = first;
    second.InterfaceGuid.Data1 = 9;
    fake().rows = {second};
    EXPECT_TRUE(probe.read().networkInterfaces.empty()) << "the new interface's device is gone, not the old one's";
    EXPECT_EQ(fake().idReads[2], 1);
    EXPECT_EQ(fake().idReads[9], 1);
}

// The real readers, given ids that can't exist.
TEST(WindowsSystemProbeNetworkReadersTest, UnknownInterfaceAndDeviceAreNotFound)
{
    const WindowsSystemProbeFunctions::Network network = WindowsSystemProbe::systemFunctions().network;
    const GUID noInterface{.Data1 = 0x13951395, .Data2 = 0x1395, .Data3 = 0x1395, .Data4 = {1, 3, 9, 5, 1, 3, 9, 5}};
    EXPECT_TRUE(network.readAdapterDeviceInstanceId(noInterface).empty());

    EXPECT_EQ(network.devicePresence(L""), DevicePresence::Unknown);
    EXPECT_EQ(network.devicePresence(L"ROOT\\TASKSMACK_1395\\0000"), DevicePresence::Absent);
    EXPECT_EQ(network.devicePresence(L"not a device id"), DevicePresence::Unknown) << "a malformed id isn't 'removed'";
}

TEST(WindowsSystemProbeNetworkReadersTest, RealAdapterIdsAreDeviceIds)
{
    const WindowsSystemProbeFunctions::Network network = WindowsSystemProbe::systemFunctions().network;
    MIB_IF_TABLE2* table = nullptr;
    ASSERT_EQ(network.getIfTable2(&table), static_cast<DWORD>(NO_ERROR));
    ASSERT_NE(table, nullptr);
    ULONG i = 0;
    while (i < table->NumEntries)
    {
        const MIB_IF_ROW2& r = table->Table[i];
        const std::wstring id = network.readAdapterDeviceInstanceId(r.InterfaceGuid);
        if (!id.empty())
        {
            EXPECT_EQ(id.find(L'\0'), std::wstring::npos) << "the terminator is dropped";
            EXPECT_NE(network.devicePresence(id), DevicePresence::Unknown);
        }
        ++i;
    }
    network.freeMibTable(table);
}

} // namespace Platform
// NOLINTEND(misc-include-cleaner)
