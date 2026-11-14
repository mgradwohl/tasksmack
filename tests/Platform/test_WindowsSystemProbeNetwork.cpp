/// @file test_WindowsSystemProbeNetwork.cpp
/// @brief WindowsSystemProbe's network read against a fake interface table (#1395): a failed listing, the
/// list kept between samples and its rescan, with only the counted rows' counters read (#3713), the
/// name fallbacks, unknown link speeds, and the removed-adapter filter (#1284) with its device-id cache --
/// read once per interface, an empty read not retried every sample, and a LUID Windows gives to a new
/// interface read afresh (#1369 review) -- and its device-presence cache (#2731): the PnP lookup made once
/// per down interface until the interface set or a status changes, or the backstop runs out, on a fake
/// clock. A CI runner has no down or removed adapter, so the real table never shows these. The CPU and
/// memory calls stay real; the real registry and configuration-manager readers are checked with ids that
/// can't exist.

#include "Domain/SamplingConfig.h"
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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
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
    int listings = 0;
    int tablesFreed = 0;
    // GetIfEntry2 calls by LUID, and the LUIDs whose read fails although the list has them
    std::map<std::uint64_t, int> entryReads;
    std::set<std::uint64_t> entryFailures;
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

/// The probe's clock (#2731), which stands still unless a test moves it. Apart from FakeNetwork so
/// reading it cannot throw.
std::chrono::steady_clock::time_point& fakeClock() noexcept
{
    static std::chrono::steady_clock::time_point now;
    return now;
}

std::chrono::steady_clock::time_point fakeNow() noexcept
{
    return fakeClock();
}

constexpr std::chrono::seconds DEVICE_CACHE_TTL{Domain::Sampling::NETWORK_ADAPTER_DEVICE_CACHE_TTL_SECONDS};
constexpr std::chrono::seconds RESCAN_INTERVAL{Domain::Sampling::NETWORK_INTERFACE_RESCAN_INTERVAL_SECONDS};

/// The list without statistics (#3713): every row, its counters zero, so a figure the probe reports
/// can only have come from GetIfEntry2.
DWORD WINAPI fakeGetIfTableWithoutStatistics(MIB_IF_TABLE2** out)
{
    ++fake().listings;
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
    for (ULONG i = 0; i < table->NumEntries; ++i)
    {
        table->Table[i].InOctets = 0;
        table->Table[i].OutOctets = 0;
    }
    *out = table;
    return NO_ERROR;
}

/// One row by its LUID, with its counters, as GetIfEntry2 gives it; ERROR_FILE_NOT_FOUND for one not there.
DWORD WINAPI fakeGetIfEntry2(MIB_IF_ROW2* entry)
{
    const std::uint64_t luid = entry->InterfaceLuid.Value;
    ++fake().entryReads[luid];
    const auto found = std::ranges::find_if(fake().rows, [luid](const MIB_IF_ROW2& r) { return r.InterfaceLuid.Value == luid; });
    if (found == fake().rows.end() || fake().entryFailures.contains(luid))
    {
        return ERROR_FILE_NOT_FOUND;
    }
    *entry = *found;
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
        fakeClock() = std::chrono::steady_clock::time_point{} + std::chrono::hours{1};
    }

    void TearDown() override
    {
        fake() = FakeNetwork{};
    }

    /// The real CPU and memory calls, the fake network and clock.
    [[nodiscard]] static WindowsSystemProbe makeProbe()
    {
        WindowsSystemProbeFunctions fns = WindowsSystemProbe::systemFunctions();
        fns.network.getIfTableWithoutStatistics = &fakeGetIfTableWithoutStatistics;
        fns.network.freeMibTable = &fakeFreeMibTable;
        fns.network.getIfEntry2 = &fakeGetIfEntry2;
        fns.network.readAdapterDeviceInstanceId = &fakeReadAdapterDeviceInstanceId;
        fns.network.devicePresence = &fakeDevicePresence;
        fns.now = &fakeNow;
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
    EXPECT_EQ(fake().listings, 3) << "a failed listing is tried again the next pass, not at the rescan";
}

TEST_F(WindowsSystemProbeNetworkTest, EveryTableIsFreedOnce)
{
    fake().rows = {row(1, L"Ethernet")};
    WindowsSystemProbe probe = makeProbe();
    (void) probe.read();
    (void) probe.read();
    EXPECT_EQ(fake().tablesFreed, 1) << "one listing for both passes (#3713)";
    fakeClock() += RESCAN_INTERVAL;
    (void) probe.read();
    EXPECT_EQ(fake().tablesFreed, 2);
}

// #3713: GetIfTable2 asked every row for its counters, a context switch per NDIS filter row the probe
// then skipped. The list now comes without them, and only the rows the probe counts are read.

TEST_F(WindowsSystemProbeNetworkTest, OnlyCountedRowsHaveTheirCountersRead)
{
    MIB_IF_ROW2 filter = row(2, L"Ethernet-WFP Native MAC Layer LightWeight Filter-0000");
    filter.InterfaceAndOperStatusFlags.FilterInterface = TRUE;
    MIB_IF_ROW2 loopback = row(3, L"Loopback Pseudo-Interface 1");
    loopback.Type = IF_TYPE_SOFTWARE_LOOPBACK;
    MIB_IF_ROW2 other = row(4, L"Bluetooth Device");
    other.Type = IF_TYPE_OTHER;
    MIB_IF_ROW2 wifi = row(5, L"Wi-Fi");
    wifi.Type = IF_TYPE_IEEE80211;
    MIB_IF_ROW2 wifiFilter = row(6, L"Wi-Fi-Native WiFi Filter Driver-0000");
    wifiFilter.Type = IF_TYPE_IEEE80211;
    wifiFilter.InterfaceAndOperStatusFlags.FilterInterface = TRUE;
    MIB_IF_ROW2 tunnel = row(7, L"Teredo");
    tunnel.Type = IF_TYPE_TUNNEL;
    tunnel.InterfaceAndOperStatusFlags.HardwareInterface = FALSE;
    fake().rows = {row(1, L"Ethernet"), filter, loopback, other, wifi, wifiFilter, tunnel};

    // What GetIfTable2's rows gave, filtered as before: isCountedNetworkRow() on each row, in table order
    std::vector<std::wstring> expectedAliases;
    std::set<std::uint64_t> counted;
    for (const MIB_IF_ROW2& r : fake().rows)
    {
        if (isCountedNetworkRow(r.Type, r.InterfaceAndOperStatusFlags.FilterInterface != 0))
        {
            expectedAliases.emplace_back(static_cast<const wchar_t*>(r.Alias));
            counted.insert(r.InterfaceLuid.Value);
        }
    }
    ASSERT_EQ(expectedAliases, (std::vector<std::wstring>{L"Ethernet", L"Wi-Fi", L"Teredo"}));

    WindowsSystemProbe probe = makeProbe();
    const SystemCounters counters = probe.read();
    EXPECT_EQ(names(counters), (std::vector<std::string>{"Ethernet", "Wi-Fi", "Teredo"}));
    std::set<std::uint64_t> read;
    for (const auto& [luid, reads] : fake().entryReads)
    {
        EXPECT_EQ(reads, 1) << luid;
        read.insert(luid);
    }
    EXPECT_EQ(read, counted) << "no filter, loopback or other row is asked for its counters";

    // The figures are GetIfEntry2's (the list's are zero), and the Total leaves the tunnel out as before
    ASSERT_EQ(counters.networkInterfaces.size(), 3U);
    EXPECT_EQ(counters.networkInterfaces[0].rxBytes, 1'000U);
    EXPECT_EQ(counters.networkInterfaces[1].txBytes, 10'000U);
    EXPECT_TRUE(counters.networkInterfaces[2].isVirtual);
    EXPECT_EQ(counters.netRxBytes, 6'000U);
    EXPECT_EQ(counters.netTxBytes, 12'000U);
}

TEST_F(WindowsSystemProbeNetworkTest, TheListIsKeptUntilTheRescanAndTheCountersReadEachPass)
{
    fake().rows = {row(1, L"Ethernet"), row(2, L"Wi-Fi")};
    WindowsSystemProbe probe = makeProbe();
    EXPECT_EQ(probe.read().netRxBytes, 3'000U);
    EXPECT_EQ(fake().listings, 1);

    fake().rows[0].InOctets += 500;
    fakeClock() += RESCAN_INTERVAL - std::chrono::seconds{1};
    EXPECT_EQ(probe.read().netRxBytes, 3'500U) << "the counters move without a listing";
    EXPECT_EQ(fake().listings, 1);
    EXPECT_EQ(fake().entryReads[1], 2);

    fakeClock() += std::chrono::seconds{1};
    (void) probe.read();
    EXPECT_EQ(fake().listings, 2) << "listed again at the rescan";
}

TEST_F(WindowsSystemProbeNetworkTest, AnAddedInterfaceIsCountedFromTheRescan)
{
    fake().rows = {row(1, L"Ethernet")};
    WindowsSystemProbe probe = makeProbe();
    (void) probe.read();

    fake().rows.push_back(row(3, L"Dock"));
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"Ethernet"})) << "not listed yet";
    EXPECT_EQ(fake().entryReads.count(3), 0U);

    fakeClock() += RESCAN_INTERVAL;
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"Ethernet", "Dock"}));
}

TEST_F(WindowsSystemProbeNetworkTest, AnInterfaceGoneIsDroppedInThePassItGoes)
{
    fake().rows = {row(1, L"Ethernet"), row(2, L"VPN"), row(3, L"Wi-Fi")};
    WindowsSystemProbe probe = makeProbe();
    EXPECT_EQ(probe.read().networkInterfaces.size(), 3U);

    // Its GetIfEntry2 finds no such interface: listed again at once, with the counters read again
    fake().rows.erase(fake().rows.begin() + 1);
    const SystemCounters counters = probe.read();
    EXPECT_EQ(names(counters), (std::vector<std::string>{"Ethernet", "Wi-Fi"}));
    EXPECT_EQ(counters.netRxBytes, 4'000U);
    EXPECT_EQ(fake().listings, 2);
    EXPECT_EQ(fake().entryReads[2], 2) << "read last pass and found gone this one, then not listed";
    EXPECT_EQ(fake().entryReads[1], 3) << "read again after the listing";

    (void) probe.read();
    EXPECT_EQ(fake().listings, 2) << "the new list stands";
    EXPECT_EQ(fake().entryReads[2], 2);
}

TEST_F(WindowsSystemProbeNetworkTest, AnInterfaceWhoseReadFailsIsSkipped)
{
    fake().rows = {row(1, L"Ethernet"), row(2, L"Flaky")};
    fake().entryFailures = {2};
    WindowsSystemProbe probe = makeProbe();
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"Ethernet"})) << "listed, but its row can't be read";
    EXPECT_EQ(fake().listings, 1) << "listed in this pass already: not again";

    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"Ethernet"}));
    EXPECT_EQ(fake().listings, 2) << "the next pass lists again, and still skips it";

    fake().entryFailures.clear();
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"Ethernet", "Flaky"}));
    EXPECT_EQ(fake().listings, 2);
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
    EXPECT_EQ(fake().presenceQueries, 1) << "the presence too, while the table stands (#2731)";
}

// #2731: CM_Locate_DevNodeW is a PnP call, so a down interface's device presence is kept until the
// interface table changes, the id is read again, or the backstop runs out.

TEST_F(WindowsSystemProbeNetworkTest, DevicePresenceIsLookedUpAgainWhenAnInterfaceComesOrGoes)
{
    fake().rows = {row(1, L"Up"), row(2, L"Unplugged", OPER_STATUS_DOWN)};
    fake().ids = {{2, L"PCI\\VEN_1\\2"}};
    fake().presence = {{L"PCI\\VEN_1\\2", DevicePresence::Present}};

    WindowsSystemProbe probe = makeProbe();
    for (int sample = 0; sample < 3; ++sample)
    {
        EXPECT_EQ(probe.read().networkInterfaces.size(), 2U);
    }
    EXPECT_EQ(fake().presenceQueries, 1);

    // A dock's adapter appears: a PnP event, after which the unplugged adapter is asked about again,
    // once the rescan lists it (#3713)
    fake().rows.push_back(row(3, L"Dock"));
    fakeClock() += RESCAN_INTERVAL;
    EXPECT_EQ(probe.read().networkInterfaces.size(), 3U);
    EXPECT_EQ(fake().presenceQueries, 2);
    (void) probe.read();
    EXPECT_EQ(fake().presenceQueries, 2) << "the same table again: no lookup";

    // And it goes, taking the unplugged adapter's device with it this time
    fake().rows.pop_back();
    fake().presence[L"PCI\\VEN_1\\2"] = DevicePresence::Absent;
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"Up"}));
    EXPECT_EQ(fake().presenceQueries, 3);
}

TEST_F(WindowsSystemProbeNetworkTest, DevicePresenceIsLookedUpAgainWhenAStatusChanges)
{
    fake().rows = {row(1, L"Up"), row(2, L"Cable", OPER_STATUS_DOWN)};
    fake().ids = {{2, L"PCI\\VEN_1\\2"}};
    fake().presence = {{L"PCI\\VEN_1\\2", DevicePresence::Present}};

    WindowsSystemProbe probe = makeProbe();
    (void) probe.read();
    (void) probe.read();
    EXPECT_EQ(fake().presenceQueries, 1);

    // The cable is plugged in: an up row's device is plainly present, so nothing is looked up
    fake().rows[1].OperStatus = IfOperStatusUp;
    EXPECT_EQ(probe.read().networkInterfaces.size(), 2U);
    EXPECT_EQ(fake().presenceQueries, 1);

    // And out again: the status changed, so the cached answer is not reused
    fake().rows[1].OperStatus = static_cast<IF_OPER_STATUS>(OPER_STATUS_DOWN);
    EXPECT_EQ(probe.read().networkInterfaces.size(), 2U);
    EXPECT_EQ(fake().presenceQueries, 2);

    // Another interface's status changing asks again too: one link event often changes several rows
    fake().rows[0].OperStatus = static_cast<IF_OPER_STATUS>(OPER_STATUS_DOWN);
    fake().ids[1] = L"PCI\\VEN_1\\1";
    fake().presence[L"PCI\\VEN_1\\1"] = DevicePresence::Present;
    EXPECT_EQ(probe.read().networkInterfaces.size(), 2U);
    EXPECT_EQ(fake().presenceQueries, 4) << "both down rows";
}

TEST_F(WindowsSystemProbeNetworkTest, DevicePresenceIsLookedUpAgainAfterTheBackstop)
{
    fake().rows = {row(2, L"Unplugged", OPER_STATUS_DOWN)};
    fake().ids = {{2, L"PCI\\VEN_1\\2"}};
    fake().presence = {{L"PCI\\VEN_1\\2", DevicePresence::Present}};

    WindowsSystemProbe probe = makeProbe();
    EXPECT_EQ(probe.read().networkInterfaces.size(), 1U);
    EXPECT_EQ(fake().presenceQueries, 1);

    fakeClock() += DEVICE_CACHE_TTL - std::chrono::seconds{1};
    (void) probe.read();
    EXPECT_EQ(fake().presenceQueries, 1) << "just short of the backstop";

    // The device went away with no change in the table (the interface stays listed, down): noticed
    // once the backstop runs out, not before
    fake().presence[L"PCI\\VEN_1\\2"] = DevicePresence::Absent;
    fakeClock() += std::chrono::seconds{1};
    EXPECT_TRUE(probe.read().networkInterfaces.empty());
    EXPECT_EQ(fake().presenceQueries, 2);
    EXPECT_EQ(fake().idReads[2], 1) << "a found id is never read again";
}

TEST_F(WindowsSystemProbeNetworkTest, AnIdFoundOnRetryHasItsPresenceLookedUpAtOnce)
{
    // A phantom first seen before its registry value was written (#1369 review): the empty id is
    // retried after the TTL, and the found id's device looked up in the same pass, not a TTL later.
    fake().rows = {row(5, L"Phantom", OPER_STATUS_DOWN)};
    WindowsSystemProbe probe = makeProbe();
    EXPECT_EQ(probe.read().networkInterfaces.size(), 1U);
    EXPECT_EQ(fake().idReads[5], 1);
    EXPECT_EQ(fake().presenceQueries, 1) << "asked with the empty id: Unknown";

    fake().ids = {{5, L"USB\\VID_1\\5"}};
    fake().presence = {{L"USB\\VID_1\\5", DevicePresence::Absent}};
    fakeClock() += DEVICE_CACHE_TTL;
    EXPECT_TRUE(probe.read().networkInterfaces.empty());
    EXPECT_EQ(fake().idReads[5], 2);
    EXPECT_EQ(fake().presenceQueries, 2);
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
    EXPECT_EQ(fake().presenceQueries, 2) << "the old adapter's presence is not inherited either (#2731)";
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
    ASSERT_EQ(network.getIfTableWithoutStatistics(&table), static_cast<DWORD>(NO_ERROR));
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

// #3713: the list without statistics has GetIfTable2's rows, with what the probe filters on unchanged,
// so the same interfaces are counted; only the counters are left out.
TEST(WindowsSystemProbeNetworkReadersTest, TheListWithoutStatisticsHasGetIfTable2sRows)
{
    const WindowsSystemProbeFunctions::Network network = WindowsSystemProbe::systemFunctions().network;
    MIB_IF_TABLE2* listed = nullptr;
    ASSERT_EQ(network.getIfTableWithoutStatistics(&listed), static_cast<DWORD>(NO_ERROR));
    ASSERT_NE(listed, nullptr);
    MIB_IF_TABLE2* full = nullptr;
    ASSERT_EQ(GetIfTable2(&full), static_cast<DWORD>(NO_ERROR));
    ASSERT_NE(full, nullptr);

    struct Filtered
    {
        std::uint64_t luid = 0;
        ULONG type = 0;
        bool filter = false;
        bool counted = false;
        auto operator<=>(const Filtered&) const = default;
    };
    const auto filtered = [](const MIB_IF_TABLE2& table)
    {
        std::vector<Filtered> rows;
        ULONG i = 0;
        while (i < table.NumEntries)
        {
            const MIB_IF_ROW2& r = table.Table[i];
            const bool filter = r.InterfaceAndOperStatusFlags.FilterInterface != 0;
            rows.push_back(
                {.luid = r.InterfaceLuid.Value, .type = r.Type, .filter = filter, .counted = isCountedNetworkRow(r.Type, filter)});
            ++i;
        }
        std::ranges::sort(rows);
        return rows;
    };
    // An interface could come or go between the two calls; on a runner none does
    EXPECT_EQ(filtered(*listed), filtered(*full));

    // And a counted row's GetIfEntry2 is the row GetIfTable2 gave it
    ULONG i = 0;
    while (i < full->NumEntries)
    {
        const MIB_IF_ROW2& r = full->Table[i];
        if (isCountedNetworkRow(r.Type, r.InterfaceAndOperStatusFlags.FilterInterface != 0))
        {
            MIB_IF_ROW2 entry{}; // NOLINT(bugprone-invalid-enum-default-initialization) - zeroed, as GetIfEntry2 expects
            entry.InterfaceLuid = r.InterfaceLuid;
            ASSERT_EQ(network.getIfEntry2(&entry), static_cast<DWORD>(NO_ERROR));
            EXPECT_EQ(entry.InterfaceIndex, r.InterfaceIndex);
            EXPECT_EQ(entry.Type, r.Type);
            EXPECT_EQ(std::wstring(static_cast<const wchar_t*>(entry.Alias)), std::wstring(static_cast<const wchar_t*>(r.Alias)));
            EXPECT_GE(entry.InOctets, r.InOctets) << "the counters only go up";
        }
        ++i;
    }
    network.freeMibTable(listed);
    FreeMibTable(full);
}

} // namespace Platform
// NOLINTEND(misc-include-cleaner)
