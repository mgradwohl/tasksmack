/// @file test_WindowsSystemProbeMath.cpp
/// @brief Unit tests for WindowsSystemProbeMath.h's pure page-file, CPU-time and network-total math,
/// and its network-interface classification (#1284)
///
/// WindowsSystemProbeMath.h includes no Windows header, so these tests build and run on every
/// platform, including Linux CI's sanitizer and coverage jobs (#1133). Tests that need the real
/// probe stay in test_WindowsSystemProbe.cpp.

#include "Platform/SystemTypes.h"
#include "Platform/Windows/WindowsSystemProbeMath.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Platform
{

// =============================================================================
// WindowsSystemProbeMath: pure helpers, fabricated inputs
// =============================================================================

namespace
{

/// One SYSTEM_PAGEFILE_INFORMATION-shaped entry: next offset, size, in use, peak, then 16 bytes of
/// (ignored) file-name UNICODE_STRING.
void appendPageFileEntry(std::vector<std::byte>& buffer, std::uint32_t next, std::uint32_t total, std::uint32_t inUse)
{
    const std::size_t at = buffer.size();
    buffer.resize(at + 32);
    const std::array<std::uint32_t, 4> header{next, total, inUse, inUse};
    std::memcpy(buffer.data() + at, header.data(), sizeof(header));
}

} // namespace

TEST(WindowsSystemProbeMathTest, SumsEveryPageFileInTheChain)
{
    std::vector<std::byte> buffer;
    appendPageFileEntry(buffer, 32, 1'048'576, 58'381); // 4 GiB file, ~228 MiB in use
    appendPageFileEntry(buffer, 0, 262'144, 1'000);     // 1 GiB file

    const auto result = sumPageFiles(buffer);
    ASSERT_TRUE(result.has_value());
    const PageFileTotals totals = result.value_or(PageFileTotals{});
    EXPECT_EQ(totals.totalPages, 1'310'720ULL);
    EXPECT_EQ(totals.inUsePages, 59'381ULL);

    const SwapBytes swap = swapFromPageFiles(totals, 4096);
    EXPECT_EQ(swap.totalBytes, 1'310'720ULL * 4096);
    EXPECT_EQ(swap.freeBytes, (1'310'720ULL - 59'381ULL) * 4096);
}

TEST(WindowsSystemProbeMathTest, MalformedChainsEndTheWalkWithoutReadingPastTheBuffer)
{
    // A link that points past the buffer is ignored after the entry it belongs to.
    std::vector<std::byte> pastEnd;
    appendPageFileEntry(pastEnd, 4096, 100, 10);
    const auto a = sumPageFiles(pastEnd);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a.value_or(PageFileTotals{}).totalPages, 100ULL);

    // A link shorter than an entry would loop or overlap; it ends the walk.
    std::vector<std::byte> tooShort;
    appendPageFileEntry(tooShort, 4, 100, 10);
    appendPageFileEntry(tooShort, 0, 999, 999);
    const auto b = sumPageFiles(tooShort);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b.value_or(PageFileTotals{}).totalPages, 100ULL);

    // Less than one entry is no answer at all.
    const std::vector<std::byte> truncated(8);
    EXPECT_FALSE(sumPageFiles(truncated).has_value());
}

TEST(WindowsSystemProbeMathTest, InUseAboveSizeIsCappedSoFreeCannotWrap)
{
    const SwapBytes swap = swapFromPageFiles({.totalPages = 100, .inUsePages = 150}, 4096);
    EXPECT_EQ(swap.totalBytes, 409'600ULL);
    EXPECT_EQ(swap.freeBytes, 0ULL);
}

TEST(WindowsSystemProbeMathTest, ProcessorTimesTakeInterruptAndDpcOutOfKernel)
{
    // kernel 1000 includes idle 600, so 400 busy, of which 50 interrupt and 30 DPC.
    const CpuCounters core = processorTimes(1000, 600, 200, 30, 50);
    EXPECT_EQ(core.idle, 600ULL);
    EXPECT_EQ(core.user, 200ULL);
    EXPECT_EQ(core.irq, 50ULL);
    EXPECT_EQ(core.softirq, 30ULL);
    EXPECT_EQ(core.system, 320ULL);
    // Each tick counted once: busy = user + kernel-busy, not + interrupt + DPC again.
    EXPECT_EQ(core.active(), 600ULL);
    EXPECT_EQ(core.total(), 1200ULL);
}

TEST(WindowsSystemProbeMathTest, ProcessorTimesClampComponentsThatOvershoot)
{
    // Counters are not sampled atomically, so a component can briefly exceed its parent.
    const CpuCounters idleAboveKernel = processorTimes(100, 150, 10, 5, 5);
    EXPECT_EQ(idleAboveKernel.system, 0ULL);
    EXPECT_EQ(idleAboveKernel.irq, 0ULL);
    EXPECT_EQ(idleAboveKernel.softirq, 0ULL);

    const CpuCounters dpcAboveBusy = processorTimes(1000, 900, 0, 500, 60);
    EXPECT_EQ(dpcAboveBusy.irq, 60ULL);
    EXPECT_EQ(dpcAboveBusy.softirq, 40ULL);
    EXPECT_EQ(dpcAboveBusy.system, 0ULL);
}

namespace
{
// Stand-in for SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION: the core's identity is its user time.
struct FakeProcessorEntry
{
    std::uint64_t id = 0;
    std::uint64_t padding = 0;
};

[[nodiscard]] CpuCounters fakeEntryToCounters(const FakeProcessorEntry& entry)
{
    CpuCounters core{};
    core.user = entry.id;
    return core;
}
} // namespace

TEST(WindowsSystemProbeMathTest, ProcessorGroupsAreAppendedInOrderAndCountIsTheSum)
{
    // Two processor groups, as on a >64-logical-processor machine (#1107): group 0 has 4 cores,
    // group 1 has 3. Every core of both appears, group 0's first.
    const std::array<FakeProcessorEntry, 4> group0{{{.id = 0}, {.id = 1}, {.id = 2}, {.id = 3}}};
    const std::array<FakeProcessorEntry, 3> group1{{{.id = 100}, {.id = 101}, {.id = 102}}};

    std::vector<CpuCounters> cores;
    EXPECT_EQ(appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group0), sizeof(group0), 0, fakeEntryToCounters), 4U);
    EXPECT_EQ(appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group1), sizeof(group1), 4, fakeEntryToCounters), 3U);

    ASSERT_EQ(cores.size(), group0.size() + group1.size());
    const std::array<std::uint64_t, 7> expectedOrder{0, 1, 2, 3, 100, 101, 102};
    for (std::size_t i = 0; i < expectedOrder.size(); ++i)
    {
        EXPECT_EQ(cores[i].user, expectedOrder[i]) << "core " << i;
        // Identity runs 0..N-1 across groups, so group 1's cores don't repeat group 0's ids and
        // SystemModel's match by coreId (#1229) keeps them apart.
        EXPECT_EQ(cores[i].coreId, i) << "core " << i;
    }
}

// #1248 review: ids come from each group's maximum size, fixed for the boot session, so a group
// whose active count changes (a hot-added processor) does not renumber the groups after it.
TEST(WindowsSystemProbeMathTest, ProcessorGroupFirstCoreIdsAreRunningSumsOfMaximums)
{
    const std::array<std::uint32_t, 3> maximums{48, 48, 32};
    EXPECT_EQ(processorGroupFirstCoreIds(maximums), (std::vector<std::size_t>{0, 48, 96}));
    EXPECT_TRUE(processorGroupFirstCoreIds({}).empty());
}

TEST(WindowsSystemProbeMathTest, AnEarlierGroupGrowingDoesNotRenumberLaterGroups)
{
    // Group 0 has room for 6 processors; 4 are active, then a fifth is hot-added. Group 1's ids
    // must not move, or SystemModel would compare one CPU's counters with another's.
    const auto firstIds = processorGroupFirstCoreIds(std::array<std::uint32_t, 2>{6, 3});
    const std::array<FakeProcessorEntry, 6> group0{{{.id = 0}, {.id = 1}, {.id = 2}, {.id = 3}, {.id = 4}, {.id = 5}}};
    const std::array<FakeProcessorEntry, 3> group1{{{.id = 100}, {.id = 101}, {.id = 102}}};

    const auto sample = [&](std::size_t group0Active)
    {
        std::vector<CpuCounters> cores;
        (void) appendProcessorGroup(cores,
                                    std::span<const FakeProcessorEntry>(group0),
                                    group0Active * sizeof(FakeProcessorEntry),
                                    firstIds[0],
                                    fakeEntryToCounters);
        (void) appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group1), sizeof(group1), firstIds[1], fakeEntryToCounters);
        return cores;
    };

    const auto before = sample(4);
    const auto after = sample(5);
    ASSERT_EQ(before.size(), 7U);
    ASSERT_EQ(after.size(), 8U);
    // Group 1's first CPU (fake id 100) keeps coreId 6 in both samples.
    EXPECT_EQ(before[4].user, 100U);
    EXPECT_EQ(before[4].coreId, 6U);
    EXPECT_EQ(after[5].user, 100U);
    EXPECT_EQ(after[5].coreId, 6U);
    // The hot-added processor takes its own id, 4, not one of group 1's.
    EXPECT_EQ(after[4].coreId, 4U);
}

TEST(WindowsSystemProbeMathTest, ShortProcessorGroupAppendsOnlyTheReturnedEntries)
{
    // The buffer had room for 4 entries but the group reported 2 (fewer processors than sized for).
    const std::array<FakeProcessorEntry, 4> group{{{.id = 7}, {.id = 8}, {.id = 9}, {.id = 10}}};
    std::vector<CpuCounters> cores{CpuCounters{}};

    EXPECT_EQ(
        appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group), 2 * sizeof(FakeProcessorEntry), 0, fakeEntryToCounters),
        2U);
    ASSERT_EQ(cores.size(), 3U);
    EXPECT_EQ(cores[1].user, 7U);
    EXPECT_EQ(cores[2].user, 8U);
}

TEST(WindowsSystemProbeMathTest, ProcessorGroupReturnLengthIsClampedToTheBuffer)
{
    // A partial trailing entry is dropped, and a length past the buffer cannot read beyond it.
    const std::array<FakeProcessorEntry, 2> group{{{.id = 1}, {.id = 2}}};
    std::vector<CpuCounters> cores;

    EXPECT_EQ(
        appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group), sizeof(FakeProcessorEntry) + 1, 0, fakeEntryToCounters),
        1U);
    EXPECT_EQ(
        appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group), 64 * sizeof(FakeProcessorEntry), 0, fakeEntryToCounters),
        2U);
    EXPECT_EQ(appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group), 0, 0, fakeEntryToCounters), 0U);
    EXPECT_EQ(cores.size(), 3U);
}

// #1107 review: on a machine with several processor groups, Total is the sum of every group's
// cores, so an idle group and a busy group of equal size read about 50 %, not one group's figure.
TEST(SumCpuCountersTest, TotalIsTheSumOfEveryCoreInEveryGroup)
{
    std::vector<CpuCounters> cores(4);
    cores[0].user = 100; // Group 0: busy
    cores[1].user = 100;
    cores[2].idle = 100; // Group 1: idle
    cores[3].idle = 100;
    cores[3].irq = 7;

    const CpuCounters total = sumCpuCounters(cores);
    EXPECT_EQ(total.user, 200U);
    EXPECT_EQ(total.idle, 200U);
    EXPECT_EQ(total.irq, 7U);
    EXPECT_EQ(total.total(), 407U);
}

TEST(SumCpuCountersTest, NoCoresIsAllZero)
{
    EXPECT_EQ(sumCpuCounters({}).total(), 0U);
}

// #1107 review: a multi-group machine never reports a one-group Total. Before the first complete
// all-group read it reports a zeroed one; the first complete read then measures from zero (the
// average since boot) instead of comparing the all-group sum with a one-group baseline, which put
// the other groups' lifetime counters into one interval as a spike.
TEST(MultiGroupTotalTest, FirstReadFailureThenRecoveryDoesNotSpike)
{
    std::optional<CpuCounters> last;
    const CpuCounters failed = multiGroupTotal(std::nullopt, last);
    EXPECT_EQ(failed.total(), 0U);
    EXPECT_FALSE(last.has_value());

    CpuCounters allGroups; // Lifetime counters of every group: 25 % busy since boot
    allGroups.user = 250;
    allGroups.idle = 750;
    const CpuCounters recovered = multiGroupTotal(allGroups, last);
    ASSERT_TRUE(last.has_value());

    // The model's busy share over the recovery interval: the since-boot average, not a spike.
    const double busy = static_cast<double>(recovered.active() - failed.active());
    const double span = static_cast<double>(recovered.total() - failed.total());
    EXPECT_DOUBLE_EQ(100.0 * busy / span, 25.0);
}

TEST(MultiGroupTotalTest, ALaterFailureRepeatsTheLastAllGroupTotal)
{
    std::optional<CpuCounters> last;
    CpuCounters allGroups;
    allGroups.user = 40;
    allGroups.idle = 60;
    (void) multiGroupTotal(allGroups, last);

    const CpuCounters failed = multiGroupTotal(std::nullopt, last);
    EXPECT_EQ(failed.user, 40U);
    EXPECT_EQ(failed.idle, 60U);
}

TEST(WindowsSystemProbeMathTest, FilterRowsAreNotCountedWhateverTheirType)
{
    // Filter-module rows carry their adapter's type and counters; the flag alone excludes them.
    EXPECT_FALSE(isCountedNetworkRow(IF_TYPE_WIFI, true));
    EXPECT_FALSE(isCountedNetworkRow(IF_TYPE_ETHERNET, true));
    EXPECT_FALSE(isCountedNetworkRow(IF_TYPE_VIRTUAL, true));
}

TEST(WindowsSystemProbeMathTest, RealInterfacesCountEvenWithIdenticalCounters)
{
    // Two distinct non-filter adapters can legitimately report the same bytes (e.g. both received
    // the same broadcast and sent nothing); the decision depends only on type and the filter flag.
    for (const std::uint32_t type :
         {IF_TYPE_ETHERNET, IF_TYPE_WIFI, IF_TYPE_TUNNEL_LINK, IF_TYPE_PPP_LINK, IF_TYPE_VIRTUAL, IF_TYPE_WWAN_GSM, IF_TYPE_WWAN_CDMA})
    {
        EXPECT_TRUE(isCountedNetworkRow(type, false)) << type;
    }
}

TEST(WindowsSystemProbeMathTest, LoopbackAndOtherTypesAreNotCounted)
{
    EXPECT_FALSE(isCountedNetworkRow(IF_TYPE_LOOPBACK, false));
    EXPECT_FALSE(isCountedNetworkRow(1, false)); // IF_TYPE_OTHER
    EXPECT_FALSE(isCountedNetworkRow(0, false));
}

TEST(WindowsSystemProbeMathTest, MobileBroadbandIsCounted)
{
    // A WWAN modem can be a laptop's only uplink; its rows used to be dropped, so the Total read 0 (#1257).
    EXPECT_EQ(IF_TYPE_WWAN_GSM, 243U);
    EXPECT_EQ(IF_TYPE_WWAN_CDMA, 244U);
    EXPECT_TRUE(isCountedNetworkRow(IF_TYPE_WWAN_GSM, false));
    EXPECT_TRUE(isCountedNetworkRow(IF_TYPE_WWAN_CDMA, false));
    EXPECT_FALSE(isCountedNetworkRow(IF_TYPE_WWAN_GSM, true));
}

namespace
{
SystemCounters::InterfaceCounters makeInterface(std::uint64_t rx, std::uint64_t tx, bool isVirtual)
{
    SystemCounters::InterfaceCounters iface;
    iface.rxBytes = rx;
    iface.txBytes = tx;
    iface.isVirtual = isVirtual;
    return iface;
}
} // namespace

TEST(WindowsSystemProbeMathTest, TotalLeavesVirtualInterfacesOutWhenHardwareIsListed)
{
    // Wi-Fi plus a VPN tunnel and the WSL vEthernet adapter: the tunnel's and vEthernet's traffic
    // also crossed the Wi-Fi adapter, so only Wi-Fi counts (#1257).
    const std::vector<SystemCounters::InterfaceCounters> interfaces{
        makeInterface(1'000, 100, false),
        makeInterface(400, 40, true),
        makeInterface(300, 30, true),
    };
    const auto totals = sumCountedInterfaces(interfaces);
    EXPECT_EQ(totals.rxBytes, 1'000U);
    EXPECT_EQ(totals.txBytes, 100U);
}

TEST(WindowsSystemProbeMathTest, TotalSumsEveryHardwareInterface)
{
    const std::vector<SystemCounters::InterfaceCounters> interfaces{
        makeInterface(1'000, 100, false),
        makeInterface(2'000, 200, false),
        makeInterface(5, 5, true),
    };
    const auto totals = sumCountedInterfaces(interfaces);
    EXPECT_EQ(totals.rxBytes, 3'000U);
    EXPECT_EQ(totals.txBytes, 300U);
}

TEST(WindowsSystemProbeMathTest, TotalCountsEveryInterfaceWhenAllAreVirtual)
{
    // A VM or sandbox whose only adapter is virtual: the Total must not read 0.
    const std::vector<SystemCounters::InterfaceCounters> interfaces{
        makeInterface(400, 40, true),
        makeInterface(300, 30, true),
    };
    const auto totals = sumCountedInterfaces(interfaces);
    EXPECT_EQ(totals.rxBytes, 700U);
    EXPECT_EQ(totals.txBytes, 70U);
}

// ---- #1284: removed adapters, Bluetooth PAN, secondary Wi-Fi ports ----

namespace
{
constexpr std::uint32_t IF_OPER_STATUS_DOWN = 2;
constexpr std::uint32_t IF_OPER_STATUS_DORMANT = 5;
} // namespace

TEST(WindowsSystemProbeMathTest, NotPresentDecidesOnItsOwn)
{
    // A long-unplugged USB dongle, an unused Wi-Fi port, Teredo: whatever else is known about it.
    for (const DevicePresence presence : {DevicePresence::Unknown, DevicePresence::Present, DevicePresence::Absent})
    {
        EXPECT_TRUE(isNotPresentNetworkRow(IF_OPER_STATUS_NOT_PRESENT, presence));
    }
}

TEST(WindowsSystemProbeMathTest, ADownAdapterWhoseDeviceWasRemovedIsLeftOut)
{
    // An unplugged dock's "Ethernet 3": reported down, its device a phantom.
    EXPECT_TRUE(isNotPresentNetworkRow(IF_OPER_STATUS_DOWN, DevicePresence::Absent));
}

TEST(WindowsSystemProbeMathTest, ADownAdapterWhoseDeviceIsPresentStaysListed)
{
    // Disabled in Windows, cable unplugged, Wi-Fi with no network, dormant: present, so listed.
    EXPECT_FALSE(isNotPresentNetworkRow(IF_OPER_STATUS_DOWN, DevicePresence::Present));
    EXPECT_FALSE(isNotPresentNetworkRow(IF_OPER_STATUS_DORMANT, DevicePresence::Present));
    // No device to ask about (Teredo, 6to4) or the query failed: kept rather than guessed away.
    EXPECT_FALSE(isNotPresentNetworkRow(IF_OPER_STATUS_DOWN, DevicePresence::Unknown));
    EXPECT_FALSE(isNotPresentNetworkRow(IF_OPER_STATUS_UP, DevicePresence::Unknown));
}

TEST(WindowsSystemProbeMathTest, OnlyRowsThatAreNeitherUpNorNotPresentAreLookedUp)
{
    EXPECT_FALSE(needsDevicePresence(IF_OPER_STATUS_UP));
    EXPECT_FALSE(needsDevicePresence(IF_OPER_STATUS_NOT_PRESENT));
    EXPECT_TRUE(needsDevicePresence(IF_OPER_STATUS_DOWN));
    EXPECT_TRUE(needsDevicePresence(IF_OPER_STATUS_DORMANT));
}

TEST(WindowsSystemProbeMathTest, BluetoothPanIsAHardwareLink)
{
    // Windows reports the Bluetooth PAN adapter as Ethernet without HardwareInterface; a phone
    // tethered over it is the machine's own link, so it counts beside an idle Wi-Fi.
    EXPECT_TRUE(isHardwareNetworkRow(false, IF_TYPE_ETHERNET, NDIS_PHYSICAL_MEDIUM_BLUETOOTH));
}

TEST(WindowsSystemProbeMathTest, TheHardwareFlagDecidesForEverythingElse)
{
    constexpr std::uint32_t NDIS_PHYSICAL_MEDIUM_UNSPECIFIED = 0;
    constexpr std::uint32_t NDIS_PHYSICAL_MEDIUM_802_3 = 14;
    constexpr std::uint32_t NDIS_PHYSICAL_MEDIUM_NATIVE_802_11 = 9;
    EXPECT_FALSE(isHardwareNetworkRow(false, IF_TYPE_ETHERNET, NDIS_PHYSICAL_MEDIUM_UNSPECIFIED)); // vEthernet, WAN Miniport
    EXPECT_FALSE(isHardwareNetworkRow(false, IF_TYPE_ETHERNET, NDIS_PHYSICAL_MEDIUM_802_3));       // Kernel Debug adapter
    EXPECT_FALSE(isHardwareNetworkRow(false, IF_TYPE_TUNNEL_LINK, NDIS_PHYSICAL_MEDIUM_BLUETOOTH));
    EXPECT_TRUE(isHardwareNetworkRow(true, IF_TYPE_ETHERNET, NDIS_PHYSICAL_MEDIUM_802_3));
    EXPECT_TRUE(isHardwareNetworkRow(true, IF_TYPE_WIFI, NDIS_PHYSICAL_MEDIUM_NATIVE_802_11));
}

namespace
{
// What the report's Wi-Fi 7 laptop lists: every Wi-Fi port is the one PCI device, and the WLAN
// service lists only "Wi-Fi" as a station interface.
constexpr std::string_view WIFI_DEVICE = "PCI\\VEN_8086&DEV_7740&SUBSYS_40E08086&REV_00\\3&11583659&0&A3";
constexpr std::string_view SECOND_WIFI_DEVICE = "PCI\\VEN_8086&DEV_7740&SUBSYS_40E08086&REV_00\\4&22222222&0&E0";
constexpr std::string_view USB_ETHERNET_DEVICE = "USB\\VID_0BDA&PID_8156\\4013000001";

constexpr NetworkAdapterPort wifiPort(std::string_view device, bool isWlanStation)
{
    return {.ifType = IF_TYPE_WIFI, .hardware = true, .isWlanStation = isWlanStation, .deviceInstanceId = device};
}
} // namespace

TEST(WindowsSystemProbeMathTest, WifiPortsOfOneDeviceCountOnceThroughTheStation)
{
    const std::array<NetworkAdapterPort, 4> rows{{
        wifiPort(WIFI_DEVICE, true),  // Wi-Fi
        wifiPort(WIFI_DEVICE, false), // Wi-Fi 2
        wifiPort(WIFI_DEVICE, false), // Wi-Fi 3
        {.ifType = IF_TYPE_ETHERNET, .hardware = true, .isWlanStation = false, .deviceInstanceId = USB_ETHERNET_DEVICE},
    }};
    EXPECT_FALSE(isSecondaryWifiPort(rows[0], rows));
    EXPECT_TRUE(isSecondaryWifiPort(rows[1], rows));
    EXPECT_TRUE(isSecondaryWifiPort(rows[2], rows));
    EXPECT_FALSE(isSecondaryWifiPort(rows[3], rows));
}

TEST(WindowsSystemProbeMathTest, TheStationCountsWhereverItIsListed)
{
    // Interface order and LUIDs say nothing about which port is the station: a Wi-Fi Direct port
    // listed (or numbered) before it is still the one left out.
    const std::array<NetworkAdapterPort, 3> rows{{
        wifiPort(WIFI_DEVICE, false),
        wifiPort(WIFI_DEVICE, false),
        wifiPort(WIFI_DEVICE, true),
    }};
    EXPECT_TRUE(isSecondaryWifiPort(rows[0], rows));
    EXPECT_TRUE(isSecondaryWifiPort(rows[1], rows));
    EXPECT_FALSE(isSecondaryWifiPort(rows[2], rows));
}

TEST(WindowsSystemProbeMathTest, WithoutWlanInformationNoWifiPortIsLeftOut)
{
    // The WLAN service stopped or wlanapi.dll missing: no station is known, so all ports count.
    const std::array<NetworkAdapterPort, 3> rows{{
        wifiPort(WIFI_DEVICE, false),
        wifiPort(WIFI_DEVICE, false),
        wifiPort(WIFI_DEVICE, false),
    }};
    for (const NetworkAdapterPort& row : rows)
    {
        EXPECT_FALSE(isSecondaryWifiPort(row, rows));
    }
}

TEST(WindowsSystemProbeMathTest, ASecondCardOfTheSameModelIsADeviceOfItsOwn)
{
    // Described "<model>" and "<model> #2", but two PCI devices: both carry traffic and both count,
    // whether or not the second card's station is listed.
    const std::array<NetworkAdapterPort, 2> withStation{{wifiPort(WIFI_DEVICE, true), wifiPort(SECOND_WIFI_DEVICE, true)}};
    EXPECT_FALSE(isSecondaryWifiPort(withStation[0], withStation));
    EXPECT_FALSE(isSecondaryWifiPort(withStation[1], withStation));
    const std::array<NetworkAdapterPort, 2> withoutStation{{wifiPort(WIFI_DEVICE, true), wifiPort(SECOND_WIFI_DEVICE, false)}};
    EXPECT_FALSE(isSecondaryWifiPort(withoutStation[1], withoutStation));
}

TEST(WindowsSystemProbeMathTest, DeviceInstanceIdsMatchIgnoringCase)
{
    // The registry can hold an id with different case than the device tree.
    const std::array<NetworkAdapterPort, 2> rows{{
        wifiPort("PCI\\VEN_8086&DEV_7740\\3&ABCDEF&0&A3", true),
        wifiPort("pci\\ven_8086&dev_7740\\3&abcdef&0&a3", false),
    }};
    EXPECT_TRUE(isSecondaryWifiPort(rows[1], rows));
    EXPECT_TRUE(equalsIgnoringAsciiCase("Abc", "aBC"));
    EXPECT_FALSE(equalsIgnoringAsciiCase("abc", "abd"));
    EXPECT_FALSE(equalsIgnoringAsciiCase("abc", "abcd"));
}

TEST(WindowsSystemProbeMathTest, AWifiPortWhoseDeviceIsUnknownIsNeverLeftOut)
{
    // Without a device id there is no proof the rows are one adapter: count both.
    const std::array<NetworkAdapterPort, 2> rows{{wifiPort({}, true), wifiPort({}, false)}};
    EXPECT_FALSE(isSecondaryWifiPort(rows[0], rows));
    EXPECT_FALSE(isSecondaryWifiPort(rows[1], rows));
}

TEST(WindowsSystemProbeMathTest, OnlyHardwareWifiRowsAreSecondaryPorts)
{
    // Ports of one wired device are not this case; nor is a Wi-Fi port whose station is not a
    // hardware row.
    const std::array<NetworkAdapterPort, 4> rows{{
        {.ifType = IF_TYPE_ETHERNET, .hardware = true, .isWlanStation = false, .deviceInstanceId = USB_ETHERNET_DEVICE},
        {.ifType = IF_TYPE_ETHERNET, .hardware = true, .isWlanStation = false, .deviceInstanceId = USB_ETHERNET_DEVICE},
        {.ifType = IF_TYPE_WIFI, .hardware = false, .isWlanStation = true, .deviceInstanceId = WIFI_DEVICE},
        wifiPort(WIFI_DEVICE, false),
    }};
    EXPECT_FALSE(isSecondaryWifiPort(rows[1], rows));
    EXPECT_FALSE(isSecondaryWifiPort(rows[3], rows));
}

TEST(WindowsSystemProbeMathTest, TotalCountsBluetoothPanAndTheWifiAdapterOnce)
{
    // Wi-Fi idle, a phone tethered over Bluetooth PAN, the Wi-Fi's secondary ports marked
    // virtual: the Total is the PAN link's traffic plus Wi-Fi's, once.
    const std::vector<SystemCounters::InterfaceCounters> interfaces{
        makeInterface(1'000, 100, false), // Wi-Fi
        makeInterface(1'000, 100, true),  // Wi-Fi 2 mirroring Wi-Fi's counters
        makeInterface(5'000, 500, false), // Bluetooth Network Connection
        makeInterface(4'000, 400, true),  // vEthernet (WSL)
    };
    const auto totals = sumCountedInterfaces(interfaces);
    EXPECT_EQ(totals.rxBytes, 6'000U);
    EXPECT_EQ(totals.txBytes, 600U);
}

TEST(WindowsSystemProbeMathTest, TotalOfNoInterfacesIsZero)
{
    const auto totals = sumCountedInterfaces({});
    EXPECT_EQ(totals.rxBytes, 0U);
    EXPECT_EQ(totals.txBytes, 0U);
}

} // namespace Platform
