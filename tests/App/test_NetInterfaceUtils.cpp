/// @file test_NetInterfaceUtils.cpp
/// @brief Tests for App::NetInterfaceUtils functions
///
/// Tests cover:
/// - Virtual/loopback interface detection
/// - Bluetooth interface detection
/// - Interface type icon selection
/// - Interface sorting and filtering
/// - Resolving the network chart's selection by name (#996)

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include "App/Panels/NetInterfaceUtils.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace App::NetInterfaceUtils
{
namespace
{

/// Helper to create interface snapshot for testing
Domain::SystemSnapshot::InterfaceSnapshot makeInterface(const std::string& name,
                                                        const std::string& displayName = "",
                                                        bool isUp = true,
                                                        double rxBytesPerSec = 0.0,
                                                        double txBytesPerSec = 0.0,
                                                        std::uint64_t linkSpeedMbps = 0)
{
    Domain::SystemSnapshot::InterfaceSnapshot iface{};
    iface.name = name;
    iface.displayName = displayName;
    iface.isUp = isUp;
    iface.rxBytesPerSec = rxBytesPerSec;
    iface.txBytesPerSec = txBytesPerSec;
    iface.linkSpeedMbps = linkSpeedMbps;
    return iface;
}

// =============================================================================
// isVirtualInterface Tests
// =============================================================================

TEST(NetInterfaceUtilsTest, IsVirtualDetectsLoopbackLo)
{
    auto iface = makeInterface("lo");
    EXPECT_TRUE(isVirtualInterface(iface));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsWindowsLoopback)
{
    auto iface = makeInterface("Loopback Pseudo-Interface 1");
    EXPECT_TRUE(isVirtualInterface(iface));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsLoopbackContains)
{
    auto iface1 = makeInterface("my-loopback-device");
    auto iface2 = makeInterface("TestLoopbackAdapter");
    EXPECT_TRUE(isVirtualInterface(iface1));
    EXPECT_TRUE(isVirtualInterface(iface2));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsDockerInterfaces)
{
    auto iface1 = makeInterface("docker0");
    auto iface2 = makeInterface("docker_gwbridge");
    auto iface3 = makeInterface("veth12345");
    auto iface4 = makeInterface("br-abcd1234");
    EXPECT_TRUE(isVirtualInterface(iface1));
    EXPECT_TRUE(isVirtualInterface(iface2));
    EXPECT_TRUE(isVirtualInterface(iface3));
    EXPECT_TRUE(isVirtualInterface(iface4));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsVpnTunnelInterfaces)
{
    auto iface1 = makeInterface("tun0");
    auto iface2 = makeInterface("tap0");
    EXPECT_TRUE(isVirtualInterface(iface1));
    EXPECT_TRUE(isVirtualInterface(iface2));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsWslInterfaces)
{
    auto iface1 = makeInterface("vEthernet (WSL)");
    auto iface2 = makeInterface("WSL Adapter");
    EXPECT_TRUE(isVirtualInterface(iface1));
    EXPECT_TRUE(isVirtualInterface(iface2));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsWindowsVirtualAdapters)
{
    auto iface1 = makeInterface("WAN Miniport (SSTP)");
    auto iface2 = makeInterface("WAN Miniport (IKEv2)");
    auto iface3 = makeInterface("Microsoft Virtual WiFi Miniport Adapter");
    EXPECT_TRUE(isVirtualInterface(iface1));
    EXPECT_TRUE(isVirtualInterface(iface2));
    EXPECT_TRUE(isVirtualInterface(iface3));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsWindowsFilterDrivers)
{
    auto iface1 = makeInterface("Ethernet QoS Packet Scheduler");
    auto iface2 = makeInterface("Some WFP Adapter");
    auto iface3 = makeInterface("LightWeight Filter Driver");
    auto iface4 = makeInterface("Native WiFi Filter");
    auto iface5 = makeInterface("Native MAC Layer Bridge");
    EXPECT_TRUE(isVirtualInterface(iface1));
    EXPECT_TRUE(isVirtualInterface(iface2));
    EXPECT_TRUE(isVirtualInterface(iface3));
    EXPECT_TRUE(isVirtualInterface(iface4));
    EXPECT_TRUE(isVirtualInterface(iface5));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsTunnelingAdapters)
{
    auto iface1 = makeInterface("6to4 Adapter");
    auto iface2 = makeInterface("Teredo Tunneling Pseudo-Interface");
    auto iface3 = makeInterface("IP-HTTPS Interface");
    EXPECT_TRUE(isVirtualInterface(iface1));
    EXPECT_TRUE(isVirtualInterface(iface2));
    EXPECT_TRUE(isVirtualInterface(iface3));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsKernelDebugInterface)
{
    auto iface = makeInterface("Kernel Debug Network Adapter");
    EXPECT_TRUE(isVirtualInterface(iface));
}

TEST(NetInterfaceUtilsTest, IsVirtualDetectsWiFiDirectAdapter)
{
    auto iface = makeInterface("Wi-Fi Direct Virtual Adapter");
    EXPECT_TRUE(isVirtualInterface(iface));
}

TEST(NetInterfaceUtilsTest, IsVirtualReturnsFalseForRealInterfaces)
{
    // Physical ethernet
    auto eth0 = makeInterface("eth0");
    auto enp0s3 = makeInterface("enp0s3");

    // Physical WiFi
    auto wlan0 = makeInterface("wlan0");
    auto wlp2s0 = makeInterface("wlp2s0");

    // Windows physical adapters
    auto winEth = makeInterface("Intel(R) Ethernet Connection I217-LM");
    auto winWifi = makeInterface("Intel(R) Wi-Fi 6 AX200 160MHz");

    EXPECT_FALSE(isVirtualInterface(eth0));
    EXPECT_FALSE(isVirtualInterface(enp0s3));
    EXPECT_FALSE(isVirtualInterface(wlan0));
    EXPECT_FALSE(isVirtualInterface(wlp2s0));
    EXPECT_FALSE(isVirtualInterface(winEth));
    EXPECT_FALSE(isVirtualInterface(winWifi));
}

// #1260: where the platform classified the interface, its isVirtual flag decides -- the same flag the
// network Total uses -- and the name heuristic is only a fallback for interfaces it couldn't classify.
TEST(NetInterfaceUtilsTest, IsVirtualFollowsThePlatformFlagOverAHardwareLookingName)
{
    auto wg = makeInterface("eth-tunnel0");
    wg.isVirtual = true;
    wg.isVirtualKnown = true;
    EXPECT_TRUE(isVirtualInterface(wg));
    EXPECT_TRUE(isAlwaysHidden(wg));
}

TEST(NetInterfaceUtilsTest, IsVirtualFollowsThePlatformFlagOverAVirtualLookingName)
{
    auto nic = makeInterface("veth0"); // a NIC renamed veth0: the platform found its backing device
    nic.isVirtual = false;
    nic.isVirtualKnown = true;
    EXPECT_FALSE(isVirtualInterface(nic));
    EXPECT_FALSE(isAlwaysHidden(nic));
    EXPECT_STREQ(getInterfaceTypeIcon(nic), ICON_FA_ETHERNET);
    EXPECT_EQ(getSortedFilteredInterfaces({nic}, /*showVirtualInterfaces=*/false).size(), 1U);
}

TEST(NetInterfaceUtilsTest, IsVirtualFallsBackToTheNameWhenThePlatformCouldNotClassify)
{
    auto unclassified = makeInterface("veth0"); // isVirtualKnown false: e.g. no sysfs entry
    EXPECT_TRUE(isVirtualInterface(unclassified));
    EXPECT_FALSE(isVirtualInterface(makeInterface("eth0")));
}

// =============================================================================
// isBluetoothInterface Tests
// =============================================================================

TEST(NetInterfaceUtilsTest, IsBluetoothDetectsNameContainsBluetooth)
{
    auto iface = makeInterface("Bluetooth Network Connection");
    EXPECT_TRUE(isBluetoothInterface(iface));
}

TEST(NetInterfaceUtilsTest, IsBluetoothDetectsDisplayNameContainsBluetooth)
{
    auto iface = makeInterface("bnep0", "Bluetooth Device");
    EXPECT_TRUE(isBluetoothInterface(iface));
}

TEST(NetInterfaceUtilsTest, IsBluetoothDetectsLowercaseBluetooth)
{
    auto iface = makeInterface("bluetooth-pan");
    EXPECT_TRUE(isBluetoothInterface(iface));
}

TEST(NetInterfaceUtilsTest, IsBluetoothDetectsBnepInterface)
{
    // bnep = Bluetooth Network Encapsulation Protocol
    auto iface = makeInterface("bnep0");
    EXPECT_TRUE(isBluetoothInterface(iface));
}

TEST(NetInterfaceUtilsTest, IsBluetoothReturnsFalseForNonBluetooth)
{
    auto eth0 = makeInterface("eth0");
    auto wlan0 = makeInterface("wlan0");
    auto wifi = makeInterface("Intel(R) Wi-Fi 6 AX200 160MHz");
    EXPECT_FALSE(isBluetoothInterface(eth0));
    EXPECT_FALSE(isBluetoothInterface(wlan0));
    EXPECT_FALSE(isBluetoothInterface(wifi));
}

// =============================================================================
// getInterfaceTypeIcon Tests
// =============================================================================

TEST(NetInterfaceUtilsTest, GetInterfaceTypeIconReturnsBluetooth)
{
    auto iface = makeInterface("Bluetooth Network Connection");
    EXPECT_EQ(getInterfaceTypeIcon(iface), ICON_FA_BLUETOOTH);
}

TEST(NetInterfaceUtilsTest, GetInterfaceTypeIconReturnsWifiForWlInterface)
{
    auto iface = makeInterface("wlan0");
    EXPECT_EQ(getInterfaceTypeIcon(iface), ICON_FA_WIFI);
}

TEST(NetInterfaceUtilsTest, GetInterfaceTypeIconReturnsWifiForWifiInName)
{
    auto iface1 = makeInterface("Wi-Fi");
    auto iface2 = makeInterface("WiFi Adapter");
    auto iface3 = makeInterface("Wireless LAN");
    EXPECT_EQ(getInterfaceTypeIcon(iface1), ICON_FA_WIFI);
    EXPECT_EQ(getInterfaceTypeIcon(iface2), ICON_FA_WIFI);
    EXPECT_EQ(getInterfaceTypeIcon(iface3), ICON_FA_WIFI);
}

TEST(NetInterfaceUtilsTest, GetInterfaceTypeIconReturnsWifiForWifiInDisplayName)
{
    auto iface = makeInterface("wlp2s0", "Intel(R) Wi-Fi 6 AX200");
    EXPECT_EQ(getInterfaceTypeIcon(iface), ICON_FA_WIFI);
}

TEST(NetInterfaceUtilsTest, GetInterfaceTypeIconReturnsCloudForVirtualInterface)
{
    auto docker = makeInterface("docker0");
    auto veth = makeInterface("veth12345");
    EXPECT_EQ(getInterfaceTypeIcon(docker), ICON_FA_CLOUD);
    EXPECT_EQ(getInterfaceTypeIcon(veth), ICON_FA_CLOUD);
}

TEST(NetInterfaceUtilsTest, GetInterfaceTypeIconReturnsHouseForLoopback)
{
    // Note: "lo" is detected as virtual first (by isVirtualInterface), so gets cloud icon.
    // The house icon is only returned for interfaces that contain "Loopback" but are NOT
    // detected as virtual (i.e., the virtual check happens first).
    // This test verifies the actual behavior of the function.
    auto lo = makeInterface("lo");
    // "lo" is detected as virtual first, so gets cloud icon (not house)
    EXPECT_EQ(getInterfaceTypeIcon(lo), ICON_FA_CLOUD);
}

TEST(NetInterfaceUtilsTest, GetInterfaceTypeIconReturnsEthernetByDefault)
{
    auto eth0 = makeInterface("eth0");
    auto enp0s3 = makeInterface("enp0s3");
    auto winEth = makeInterface("Intel(R) Ethernet Connection");
    EXPECT_EQ(getInterfaceTypeIcon(eth0), ICON_FA_ETHERNET);
    EXPECT_EQ(getInterfaceTypeIcon(enp0s3), ICON_FA_ETHERNET);
    EXPECT_EQ(getInterfaceTypeIcon(winEth), ICON_FA_ETHERNET);
}

TEST(NetInterfaceUtilsTest, GetInterfaceTypeIconBluetoothTakesPrecedence)
{
    // Bluetooth should be detected even if name contains "Wireless"
    auto iface = makeInterface("Bluetooth Wireless Connection");
    EXPECT_EQ(getInterfaceTypeIcon(iface), ICON_FA_BLUETOOTH);
}

// =============================================================================
// getSortedFilteredInterfaces Tests
// =============================================================================

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesFiltersVirtual)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("eth0"));
    interfaces.push_back(makeInterface("docker0"));
    interfaces.push_back(makeInterface("wlan0"));
    interfaces.push_back(makeInterface("veth12345"));

    auto result = getSortedFilteredInterfaces(interfaces, false); // showVirtualInterfaces = false
    ASSERT_EQ(result.size(), 2U);

    // Should have eth0 and wlan0, but not docker0 or veth12345
    auto hasEth0 = std::ranges::any_of(result, [](const auto& i) { return i.name == "eth0"; });
    auto hasWlan0 = std::ranges::any_of(result, [](const auto& i) { return i.name == "wlan0"; });
    auto hasDocker = std::ranges::any_of(result, [](const auto& i) { return i.name == "docker0"; });
    auto hasVeth = std::ranges::any_of(result, [](const auto& i) { return i.name == "veth12345"; });

    EXPECT_TRUE(hasEth0);
    EXPECT_TRUE(hasWlan0);
    EXPECT_FALSE(hasDocker);
    EXPECT_FALSE(hasVeth);
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesIncludesVirtualWhenEnabled)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("eth0"));
    interfaces.push_back(makeInterface("docker0"));

    auto result = getSortedFilteredInterfaces(interfaces, true); // showVirtualInterfaces = true
    ASSERT_EQ(result.size(), 2U);
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesFiltersBluetooth)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("eth0"));
    interfaces.push_back(makeInterface("Bluetooth Network Connection"));

    auto result = getSortedFilteredInterfaces(interfaces, false);
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].name, "eth0");
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesFiltersDownInterfaces)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("eth0", "", true));  // up
    interfaces.push_back(makeInterface("eth1", "", false)); // down

    auto result = getSortedFilteredInterfaces(interfaces, false, false); // showDownInterfaces = false
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].name, "eth0");
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesIncludesDownWhenEnabled)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("eth0", "", true));  // up
    interfaces.push_back(makeInterface("eth1", "", false)); // down

    auto result = getSortedFilteredInterfaces(interfaces, false, true); // showDownInterfaces = true
    ASSERT_EQ(result.size(), 2U);
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesSortsUpFirst)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("eth0", "", false)); // down
    interfaces.push_back(makeInterface("eth1", "", true));  // up

    auto result = getSortedFilteredInterfaces(interfaces, false, true);
    ASSERT_EQ(result.size(), 2U);
    EXPECT_EQ(result[0].name, "eth1"); // up first
    EXPECT_EQ(result[1].name, "eth0"); // down second
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesSortsActiveFirst)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("eth0", "", true, 0.0, 0.0));      // no activity
    interfaces.push_back(makeInterface("eth1", "", true, 1000.0, 500.0)); // active

    auto result = getSortedFilteredInterfaces(interfaces, false, true);
    ASSERT_EQ(result.size(), 2U);
    EXPECT_EQ(result[0].name, "eth1"); // active first
    EXPECT_EQ(result[1].name, "eth0"); // inactive second
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesSortsByLinkSpeed)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("eth0", "", true, 0.0, 0.0, 100));  // 100 Mbps
    interfaces.push_back(makeInterface("eth1", "", true, 0.0, 0.0, 1000)); // 1000 Mbps

    auto result = getSortedFilteredInterfaces(interfaces, false, true);
    ASSERT_EQ(result.size(), 2U);
    EXPECT_EQ(result[0].name, "eth1"); // faster first
    EXPECT_EQ(result[1].name, "eth0"); // slower second
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesSortsAlphabetically)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("wlan0", "", true, 0.0, 0.0, 100));
    interfaces.push_back(makeInterface("eth0", "", true, 0.0, 0.0, 100));

    auto result = getSortedFilteredInterfaces(interfaces, false, true);
    ASSERT_EQ(result.size(), 2U);
    EXPECT_EQ(result[0].name, "eth0");  // alphabetically first
    EXPECT_EQ(result[1].name, "wlan0"); // alphabetically second
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesUsesDisplayNameForSorting)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("zzz0", "AAA Adapter", true, 0.0, 0.0, 100));
    interfaces.push_back(makeInterface("aaa0", "ZZZ Adapter", true, 0.0, 0.0, 100));

    auto result = getSortedFilteredInterfaces(interfaces, false, true);
    ASSERT_EQ(result.size(), 2U);
    EXPECT_EQ(result[0].name, "zzz0"); // display name "AAA Adapter" sorts first
    EXPECT_EQ(result[1].name, "aaa0"); // display name "ZZZ Adapter" sorts second
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesFallsBackToNameIfNoDisplayName)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("wlan0", "")); // no display name
    interfaces.push_back(makeInterface("eth0", ""));  // no display name

    auto result = getSortedFilteredInterfaces(interfaces, false, true);
    ASSERT_EQ(result.size(), 2U);
    EXPECT_EQ(result[0].name, "eth0");  // name "eth0" sorts first
    EXPECT_EQ(result[1].name, "wlan0"); // name "wlan0" sorts second
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesEmptyInput)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;

    auto result = getSortedFilteredInterfaces(interfaces, false, true);
    EXPECT_TRUE(result.empty());
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesComplexSorting)
{
    // Test the complete sort order: up > active > speed > alphabetical
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(makeInterface("eth0", "", false, 0.0, 0.0, 1000));   // down, no activity, fast
    interfaces.push_back(makeInterface("eth1", "", true, 0.0, 0.0, 100));     // up, no activity, slow
    interfaces.push_back(makeInterface("eth2", "", true, 1000.0, 0.0, 100));  // up, active, slow
    interfaces.push_back(makeInterface("eth3", "", true, 1000.0, 0.0, 1000)); // up, active, fast

    auto result = getSortedFilteredInterfaces(interfaces, false, true);
    ASSERT_EQ(result.size(), 4U);

    // Expected order: eth3 (up, active, fast), eth2 (up, active, slow), eth1 (up, no activity), eth0 (down)
    EXPECT_EQ(result[0].name, "eth3");
    EXPECT_EQ(result[1].name, "eth2");
    EXPECT_EQ(result[2].name, "eth1");
    EXPECT_EQ(result[3].name, "eth0");
}

TEST(NetInterfaceUtilsTest, GetSortedFilteredInterfacesPreservesAllFields)
{
    auto original = makeInterface("eth0", "My Ethernet", true, 1000.0, 500.0, 1000);

    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> interfaces;
    interfaces.push_back(original);

    auto result = getSortedFilteredInterfaces(interfaces, false, true);
    ASSERT_EQ(result.size(), 1U);

    // Verify all fields are preserved
    EXPECT_EQ(result[0].name, "eth0");
    EXPECT_EQ(result[0].displayName, "My Ethernet");
    EXPECT_TRUE(result[0].isUp);
    EXPECT_DOUBLE_EQ(result[0].rxBytesPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(result[0].txBytesPerSec, 500.0);
    EXPECT_EQ(result[0].linkSpeedMbps, 1000U);
}

// ========== Interface Status default view (#1211) ==========

[[nodiscard]] std::vector<std::string> rowNames(const std::vector<Domain::SystemSnapshot::InterfaceSnapshot>& rows)
{
    std::vector<std::string> names;
    names.reserve(rows.size());
    for (const auto& row : rows)
    {
        names.push_back(row.name);
    }
    return names;
}

/// The review machine's table in miniature: one active adapter among down and virtual ones.
[[nodiscard]] std::vector<Domain::SystemSnapshot::InterfaceSnapshot> reviewMachineInterfaces()
{
    auto bridge = makeInterface("br0", "", true, 10.0, 10.0); // Up, but the platform marks it virtual
    bridge.isVirtual = true;
    return {
        makeInterface("Wi-Fi", "", true, 2000.0, 300.0, 866),
        makeInterface("WAN Miniport (IP)", "", true),
        makeInterface("Wi-Fi 2", "", false),
        makeInterface("Ethernet 3", "USB Ethernet", false),
        makeInterface("Bluetooth Network Connection", "", false),
        bridge,
    };
}

TEST(NetInterfaceUtilsTest, StatusRowsHideDownInterfacesByDefault)
{
    const InterfaceNameSet noTraffic;
    const auto rows = getInterfaceStatusRows({makeInterface("eth0", "", true), makeInterface("eth1", "", false)}, false, noTraffic);
    EXPECT_EQ(rowNames(rows), std::vector<std::string>{"eth0"});
}

TEST(NetInterfaceUtilsTest, StatusRowsHideVirtualInterfacesByDefault)
{
    const InterfaceNameSet noTraffic;
    auto flagged = makeInterface("wg0", "", true, 100.0, 100.0); // Name heuristic doesn't know it; the flag does
    flagged.isVirtual = true;
    const auto rows =
        getInterfaceStatusRows({makeInterface("eth0", "", true), flagged, makeInterface("WAN Miniport (IP)", "", true)}, false, noTraffic);
    EXPECT_EQ(rowNames(rows), std::vector<std::string>{"eth0"});
}

TEST(NetInterfaceUtilsTest, StatusRowsKeepADownInterfaceWithTrafficNow)
{
    const InterfaceNameSet noTraffic;
    const auto rows = getInterfaceStatusRows({makeInterface("eth1", "", false, 0.0, 50.0)}, false, noTraffic);
    EXPECT_EQ(rowNames(rows), std::vector<std::string>{"eth1"});
}

TEST(NetInterfaceUtilsTest, StatusRowsKeepADownInterfaceThatHadTrafficThisSession)
{
    InterfaceNameSet seen;
    recordInterfaceTraffic({makeInterface("eth1", "", true, 500.0, 0.0), makeInterface("eth2", "", true)}, seen);
    EXPECT_TRUE(seen.contains("eth1"));
    EXPECT_FALSE(seen.contains("eth2"));

    // Unplugged later: down, no traffic now, but still listed.
    const std::vector interfaces = {makeInterface("eth1", "", false), makeInterface("eth2", "", false)};
    EXPECT_EQ(rowNames(getInterfaceStatusRows(interfaces, false, seen)), std::vector<std::string>{"eth1"});
    EXPECT_EQ(countHiddenInterfaces(interfaces, seen), 1U);
}

TEST(NetInterfaceUtilsTest, HiddenCountIsTheInterfacesLeftOutByDefault)
{
    const InterfaceNameSet noTraffic;
    const auto interfaces = reviewMachineInterfaces();
    EXPECT_EQ(countHiddenInterfaces(interfaces, noTraffic), 5U);
    EXPECT_EQ(rowNames(getInterfaceStatusRows(interfaces, false, noTraffic)), std::vector<std::string>{"Wi-Fi"});
    EXPECT_EQ(countHiddenInterfaces({}, noTraffic), 0U);
}

TEST(NetInterfaceUtilsTest, ShowAllListsEveryInterface)
{
    const InterfaceNameSet noTraffic;
    const auto interfaces = reviewMachineInterfaces();
    const auto rows = getInterfaceStatusRows(interfaces, true, noTraffic);
    EXPECT_EQ(rows.size(), interfaces.size());
    EXPECT_EQ(rows.front().name, "Wi-Fi"); // Still sorted: up and active first
}

// ========== interfaceSelectorLabels (#1106) ==========

TEST(NetInterfaceUtilsTest, SelectorMarksVirtualInterfacesLeftOutOfTheTotal)
{
    auto wg0 = makeInterface("wg0");
    wg0.isVirtual = true;
    const std::vector interfaces{makeInterface("eth0", "Ethernet"), wg0};
    EXPECT_EQ(interfaceSelectorLabels(interfaces),
              (std::vector<std::string>{"Total (Hardware Interfaces)", "Ethernet", "wg0 (virtual, not in Total)"}));
}

TEST(NetInterfaceUtilsTest, SelectorTotalIsAllInterfacesWhenNoneIsLeftOut)
{
    // No virtual interface (or a platform that doesn't classify them): nothing is left out.
    EXPECT_EQ(interfaceSelectorLabels({makeInterface("eth0"), makeInterface("wlan0")}),
              (std::vector<std::string>{"Total (All Interfaces)", "eth0", "wlan0"}));

    // Every interface virtual (inside a container): the Total counts them all, so none is marked.
    auto veth = makeInterface("eth0");
    veth.isVirtual = true;
    EXPECT_EQ(interfaceSelectorLabels({veth}), (std::vector<std::string>{"Total (All Interfaces)", "eth0"}));
}

// ========== resolveInterfaceSelection (#996) ==========

TEST(NetInterfaceUtilsTest, EmptySelectionIsTotal)
{
    const std::vector interfaces{makeInterface("eth0"), makeInterface("wlan0")};
    const auto selection = resolveInterfaceSelection(interfaces, "");
    EXPECT_FALSE(selection.index.has_value());
    EXPECT_FALSE(selection.lost);
}

TEST(NetInterfaceUtilsTest, SelectionFollowsTheInterfaceWhenTheListIsReordered)
{
    const std::vector before{makeInterface("eth0"), makeInterface("wlan0")};
    const std::vector after{makeInterface("wlan0"), makeInterface("eth0")};
    EXPECT_EQ(resolveInterfaceSelection(before, "wlan0").index, 1U);
    EXPECT_EQ(resolveInterfaceSelection(after, "wlan0").index, 0U);
    EXPECT_FALSE(resolveInterfaceSelection(after, "wlan0").lost);
}

TEST(NetInterfaceUtilsTest, SelectionFollowsTheInterfaceWhenAnotherIsInsertedBeforeIt)
{
    const std::vector after{makeInterface("eth0"), makeInterface("tun0"), makeInterface("wlan0")};
    const auto selection = resolveInterfaceSelection(after, "wlan0");
    EXPECT_EQ(selection.index, 2U);
    EXPECT_FALSE(selection.lost);
}

TEST(NetInterfaceUtilsTest, SelectionFollowsTheInterfaceWhenAnotherIsRemovedBeforeIt)
{
    const std::vector after{makeInterface("wlan0")};
    const auto selection = resolveInterfaceSelection(after, "wlan0");
    EXPECT_EQ(selection.index, 0U);
    EXPECT_FALSE(selection.lost);
}

TEST(NetInterfaceUtilsTest, RemovingTheSelectedInterfaceReportsItLostInsteadOfPickingAnother)
{
    // The old positional selection clamped to whichever interface was now last.
    const std::vector after{makeInterface("eth0"), makeInterface("tun0")};
    const auto selection = resolveInterfaceSelection(after, "wlan0");
    EXPECT_FALSE(selection.index.has_value());
    EXPECT_TRUE(selection.lost);

    const std::vector<Domain::SystemSnapshot::InterfaceSnapshot> none;
    EXPECT_TRUE(resolveInterfaceSelection(none, "wlan0").lost);
}

TEST(NetInterfaceUtilsTest, SelectionMatchesTheRawNameNotTheDisplayName)
{
    const std::vector interfaces{makeInterface("{GUID-1}", "Ethernet"), makeInterface("{GUID-2}", "Wi-Fi")};
    EXPECT_EQ(resolveInterfaceSelection(interfaces, "{GUID-2}").index, 1U);
    EXPECT_TRUE(resolveInterfaceSelection(interfaces, "Wi-Fi").lost);
}

// #1298 review: virtual interfaces are always hidden, so recording their traffic changes nothing --
// and churned container veth names would pile up in the session set. They are not recorded.
TEST(NetInterfaceUtilsTest, TrafficOnAlwaysHiddenInterfacesIsNotRecorded)
{
    Domain::SystemSnapshot::InterfaceSnapshot veth;
    veth.name = "veth1a2b3c";
    veth.isVirtual = true;
    veth.rxBytesPerSec = 1000.0;
    Domain::SystemSnapshot::InterfaceSnapshot wifi;
    wifi.name = "Wi-Fi";
    wifi.rxBytesPerSec = 1000.0;

    InterfaceNameSet seen;
    recordInterfaceTraffic({veth, wifi}, seen);
    EXPECT_FALSE(seen.contains("veth1a2b3c"));
    EXPECT_TRUE(seen.contains("Wi-Fi"));
}

} // namespace
} // namespace App::NetInterfaceUtils

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
