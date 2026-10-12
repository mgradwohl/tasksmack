/// @file test_SystemdBusFake.cpp
/// @brief SystemdBus's NetworkManager readers (#1518) against a fake NetworkManager on a private bus
/// (#1395, FakeSystemBus.h): the Wi-Fi link (SSID bytes made safe, frequency, bitrate), "not connected",
/// an adapter NetworkManager doesn't know, the DHCPv4 lease from Options (skipping a non-string one), a
/// device without DHCP, and a bus where NetworkManager isn't running at all.

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

} // namespace
} // namespace Platform

#endif // TASKSMACK_HAS_SDBUS
