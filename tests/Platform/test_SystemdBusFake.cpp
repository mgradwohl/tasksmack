/// @file test_SystemdBusFake.cpp
/// @brief SystemdBus's readers (#1518) against fake services on a private bus (#1395, FakeSystemBus.h).
/// NetworkManager: the Wi-Fi link (SSID bytes made safe, frequency, bitrate), "not connected", an adapter
/// it doesn't know, the DHCPv4 lease from Options (skipping a non-string one), a device without DHCP, and
/// a bus where it isn't running. systemd-resolved: an adapter's DNS servers (IPv4 and IPv6) and search
/// domains (a routing-only one marked "~"), an unknown link, and resolved not running. systemd-networkd:
/// the DHCPv4 lease from a link's Describe() JSON, a link without DHCP, and an unknown link.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxNetworkAdapters.h"
#include "Platform/Linux/SystemdBus.h"

#include <gtest/gtest.h>

#ifdef TASKSMACK_HAS_SDBUS

#include "FakeSystemBus.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

using TestSupport::FakeSystemBus;
using TestSupport::MethodAnswer;
using TestSupport::ObjectPath;

constexpr const char* NM = "org.freedesktop.NetworkManager";
constexpr const char* NM_PATH = "/org/freedesktop/NetworkManager";
constexpr const char* WIRELESS = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* DEVICE = "org.freedesktop.NetworkManager.Device";
constexpr const char* ACCESS_POINT = "org.freedesktop.NetworkManager.AccessPoint";
constexpr const char* DHCP4 = "org.freedesktop.NetworkManager.DHCP4Config";

constexpr const char* JOINED_DEVICE = "/org/freedesktop/NetworkManager/Devices/3";
constexpr const char* IDLE_DEVICE = "/org/freedesktop/NetworkManager/Devices/4";
constexpr const char* ACCESS_POINT_PATH = "/org/freedesktop/NetworkManager/AccessPoint/7";
constexpr const char* DHCP_PATH = "/org/freedesktop/NetworkManager/DHCP4Config/1";

class SystemdBusFakeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!FakeSystemBus::available())
        {
            GTEST_SKIP() << "dbus-daemon isn't installed";
        }
    }

    /// A NetworkManager with wlan0 joined to "Café" plus a byte that isn't UTF-8 (an SSID is up to 32
    /// arbitrary bytes), and wlan1 up but not joined; wlan0 has a DHCPv4 lease, wlan1 none.
    void serveNetworkManager()
    {
        m_Bus.addStringMethod(NM_PATH,
                              NM,
                              "GetDeviceByIpIface",
                              [](const std::string& name) -> MethodAnswer
                              {
                                  if (name == "wlan0")
                                  {
                                      return {.path = ObjectPath{JOINED_DEVICE}, .errorName = {}};
                                  }
                                  if (name == "wlan1")
                                  {
                                      return {.path = ObjectPath{IDLE_DEVICE}, .errorName = {}};
                                  }
                                  return {.path = std::nullopt, .errorName = "org.freedesktop.NetworkManager.UnknownDevice"};
                              });
        auto& props = m_Bus.properties();
        props[JOINED_DEVICE][WIRELESS]["ActiveAccessPoint"] = ObjectPath{ACCESS_POINT_PATH};
        props[JOINED_DEVICE][WIRELESS]["Bitrate"] = std::uint32_t{866700};
        props[JOINED_DEVICE][DEVICE]["Dhcp4Config"] = ObjectPath{DHCP_PATH};
        props[IDLE_DEVICE][WIRELESS]["ActiveAccessPoint"] = ObjectPath{"/"};
        props[IDLE_DEVICE][DEVICE]["Dhcp4Config"] = ObjectPath{"/"};
        props[ACCESS_POINT_PATH][ACCESS_POINT]["Ssid"] = std::vector<std::uint8_t>{'C', 'a', 'f', 0xC3, 0xA9, ' ', 0xFF};
        props[ACCESS_POINT_PATH][ACCESS_POINT]["Frequency"] = std::uint32_t{5180};
        props[DHCP_PATH][DHCP4]["Options"] = std::map<std::string, TestSupport::DictValue>{
            {"dhcp_server_identifier", std::string("192.168.1.1")},
            {"dhcp_lease_time", std::string("86400")},
            {"expiry", std::string("1760000000")},
            {"domain_name", std::string("lan")},
            {"odd_number", std::uint32_t{7}}, // not a string: skipped
        };
        ASSERT_TRUE(m_Bus.start(NM)) << m_Bus.error();
    }

    FakeSystemBus m_Bus;
};

TEST_F(SystemdBusFakeTest, AJoinedAdapterGivesItsNetworkFrequencyAndRate)
{
    serveNetworkManager();
    const LinuxNetworkAdapters::WifiLinkReader read = SystemdBus::makeWifiLinkReader();
    const LinuxNetworkAdapters::WifiLinkRead wlan0 = read("wlan0");
    ASSERT_TRUE(wlan0.link.has_value()) << wlan0.error;
    const WifiLink link = wlan0.link.value_or(WifiLink{});
    EXPECT_TRUE(link.ssid.starts_with("Caf\xC3\xA9 ")) << link.ssid; // UTF-8 kept
    EXPECT_FALSE(link.ssid.contains('\xFF')) << link.ssid;           // the stray byte made safe to draw
    EXPECT_EQ(link.frequencyMhz, 5180U);
    EXPECT_EQ(link.bitrateKbps, 866700U);
}

TEST_F(SystemdBusFakeTest, AnAdapterThatHasntJoinedSaysNotConnected)
{
    serveNetworkManager();
    const LinuxNetworkAdapters::WifiLinkRead wlan1 = SystemdBus::makeWifiLinkReader()("wlan1");
    EXPECT_FALSE(wlan1.link.has_value());
    EXPECT_EQ(wlan1.error, "Not connected");
}

TEST_F(SystemdBusFakeTest, AnAdapterNetworkManagerDoesntKnowIsSaidSoAndOthersStillRead)
{
    serveNetworkManager();
    const LinuxNetworkAdapters::WifiLinkReader read = SystemdBus::makeWifiLinkReader();
    const LinuxNetworkAdapters::WifiLinkRead eth0 = read("eth0");
    EXPECT_FALSE(eth0.link.has_value());
    EXPECT_EQ(eth0.error, "NetworkManager doesn't manage this adapter");
    // That answer is about one adapter: the same reader still reads the next.
    EXPECT_TRUE(read("wlan0").link.has_value());
}

TEST_F(SystemdBusFakeTest, TheDhcpLeaseComesFromItsOptions)
{
    serveNetworkManager();
    const LinuxNetworkAdapters::DhcpLeaseRead wlan0 = SystemdBus::makeDhcpLeaseReader()("wlan0");
    ASSERT_TRUE(wlan0.lease.has_value()) << wlan0.error;
    const DhcpLease lease = wlan0.lease.value_or(DhcpLease{});
    EXPECT_EQ(lease.server, "192.168.1.1");
    EXPECT_EQ(lease.leaseSeconds, 86400U);
    EXPECT_EQ(lease.expiresUnix, 1760000000U);
}

TEST_F(SystemdBusFakeTest, AnAdapterWithoutDhcpHasNoLease)
{
    serveNetworkManager();
    const LinuxNetworkAdapters::DhcpLeaseRead wlan1 = SystemdBus::makeDhcpLeaseReader()("wlan1");
    EXPECT_FALSE(wlan1.lease.has_value());
    EXPECT_FALSE(wlan1.error.empty());
}

TEST_F(SystemdBusFakeTest, WithoutNetworkManagerEveryAdapterSaysSo)
{
    ASSERT_TRUE(m_Bus.start("")) << m_Bus.error(); // a bus nobody serves NetworkManager on
    const LinuxNetworkAdapters::WifiLinkReader read = SystemdBus::makeWifiLinkReader();
    EXPECT_EQ(read("wlan0").error, "NetworkManager isn't running");
    // Remembered for the reader's other adapters rather than asked again.
    EXPECT_EQ(read("wlan1").error, "NetworkManager isn't running");
    EXPECT_FALSE(SystemdBus::makeDhcpLeaseReader()("wlan0").lease.has_value());
}

constexpr const char* RESOLVE = "org.freedesktop.resolve1";
constexpr const char* RESOLVE_PATH = "/org/freedesktop/resolve1";
constexpr const char* RESOLVE_MANAGER = "org.freedesktop.resolve1.Manager";
constexpr const char* RESOLVE_LINK = "org.freedesktop.resolve1.Link";
constexpr const char* RESOLVE_LINK_PATH = "/org/freedesktop/resolve1/link/_32";

constexpr const char* NETWORKD = "org.freedesktop.network1";
constexpr const char* NETWORKD_PATH = "/org/freedesktop/network1";
constexpr const char* NETWORKD_MANAGER = "org.freedesktop.network1.Manager";
constexpr const char* NETWORKD_LINK = "org.freedesktop.network1.Link";
constexpr const char* DHCP_LINK_PATH = "/org/freedesktop/network1/link/_33";
constexpr const char* STATIC_LINK_PATH = "/org/freedesktop/network1/link/_34";

/// A trimmed systemd-networkd link description: a DHCPv4 address from 192.168.1.1, and the client's
/// lease (as in test_NetworkdLinkJson.cpp).
constexpr const char* DHCP_DESCRIPTION = R"({"Index": 3, "Name": "enp3s0",
  "Addresses": [{"Family": 2, "Address": [192,168,1,20], "PrefixLength": 24, "ConfigSource": "DHCPv4",
                 "ConfigProvider": [192,168,1,1], "ValidLifetimeUSec": 86412000000}],
  "DHCPv4Client": {"Lease": {"LeaseTimestampUSec": 12000000}}})";

TEST_F(SystemdBusFakeTest, ResolvedGivesAnAdaptersOwnServersAndDomains)
{
    m_Bus.addIntMethod(RESOLVE_PATH,
                       RESOLVE_MANAGER,
                       "GetLink",
                       [](std::int32_t ifindex) -> MethodAnswer
                       {
                           if (ifindex == 2)
                           {
                               return {.path = ObjectPath{RESOLVE_LINK_PATH}, .errorName = {}};
                           }
                           return {.path = std::nullopt, .errorName = "org.freedesktop.resolve1.NoSuchLink"};
                       });
    auto& props = m_Bus.properties();
    props[RESOLVE_LINK_PATH][RESOLVE_LINK]["DNS"] = TestSupport::DnsServers{
        {2, {192, 168, 1, 1}},
        {10, {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
    };
    props[RESOLVE_LINK_PATH][RESOLVE_LINK]["Domains"] = TestSupport::DnsDomains{{"lan", false}, {"corp.example", true}};
    ASSERT_TRUE(m_Bus.start(RESOLVE)) << m_Bus.error();

    const LinuxNetworkAdapters::LinkDnsReader read = SystemdBus::makeLinkDnsReader();
    const LinuxNetworkAdapters::LinkDnsRead link = read(2);
    ASSERT_TRUE(link.read) << link.error;
    EXPECT_EQ(link.servers, (std::vector<std::string>{"192.168.1.1", "2001:db8::1"}));
    EXPECT_EQ(link.domains, (std::vector<std::string>{"lan", "~corp.example"})); // "~": only routes lookups

    const LinuxNetworkAdapters::LinkDnsRead unknown = read(9);
    EXPECT_FALSE(unknown.read);
    EXPECT_EQ(unknown.error, "systemd-resolved doesn't know this adapter");
    EXPECT_TRUE(read(2).read); // about one adapter only
}

TEST_F(SystemdBusFakeTest, WithoutResolvedEveryAdapterSaysSo)
{
    ASSERT_TRUE(m_Bus.start("")) << m_Bus.error();
    const LinuxNetworkAdapters::LinkDnsReader read = SystemdBus::makeLinkDnsReader();
    EXPECT_EQ(read(2).error, "systemd-resolved isn't running");
    EXPECT_EQ(read(3).error, "systemd-resolved isn't running");
}

TEST_F(SystemdBusFakeTest, NetworkdGivesTheLeaseFromALinksDescription)
{
    m_Bus.addIntMethod(NETWORKD_PATH,
                       NETWORKD_MANAGER,
                       "GetLinkByIndex",
                       [](std::int32_t ifindex) -> MethodAnswer
                       {
                           if (ifindex == 3)
                           {
                               return {.path = ObjectPath{DHCP_LINK_PATH}, .errorName = {}, .name = "enp3s0"};
                           }
                           if (ifindex == 4)
                           {
                               return {.path = ObjectPath{STATIC_LINK_PATH}, .errorName = {}, .name = "enp4s0"};
                           }
                           return {.path = std::nullopt, .errorName = "org.freedesktop.network1.NoSuchLink"};
                       });
    m_Bus.addNoArgMethod(DHCP_LINK_PATH,
                         NETWORKD_LINK,
                         "Describe",
                         []() -> MethodAnswer { return {.path = std::nullopt, .errorName = {}, .text = DHCP_DESCRIPTION}; });
    m_Bus.addNoArgMethod(STATIC_LINK_PATH,
                         NETWORKD_LINK,
                         "Describe",
                         []() -> MethodAnswer
                         {
                             return {.path = std::nullopt,
                                     .errorName = {},
                                     .text = R"({"Addresses": [{"Family": 2, "Address": [10,0,0,5], "ConfigSource": "static"}]})"};
                         });
    ASSERT_TRUE(m_Bus.start(NETWORKD)) << m_Bus.error();

    const LinuxNetworkAdapters::IndexedDhcpLeaseReader read = SystemdBus::makeNetworkdLeaseReader();
    const LinuxNetworkAdapters::DhcpLeaseRead dhcp = read(3);
    ASSERT_TRUE(dhcp.lease.has_value()) << dhcp.error;
    const DhcpLease lease = dhcp.lease.value_or(DhcpLease{});
    EXPECT_EQ(lease.server, "192.168.1.1");
    EXPECT_EQ(lease.leaseSeconds, 86'400U); // valid until 86412 s after boot, obtained at 12 s

    EXPECT_EQ(read(4).error, "Not configured by DHCP");
    EXPECT_EQ(read(9).error, "systemd-networkd doesn't manage this adapter");
}

} // namespace
} // namespace Platform

#endif // TASKSMACK_HAS_SDBUS
