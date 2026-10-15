/// @file test_SystemdBusFake.cpp
/// @brief SystemdBus's readers (#1518) against fake services on a private bus (#1395, FakeSystemBus.h).
/// NetworkManager: the Wi-Fi link (SSID bytes made safe, frequency, bitrate), "not connected", an adapter
/// it doesn't know, the DHCPv4 lease from Options (skipping a non-string one), a device without DHCP, and
/// a bus where it isn't running. systemd-resolved: an adapter's DNS servers (IPv4 and IPv6) and search
/// domains (a routing-only one marked "~"), an unknown link, and resolved not running. systemd-networkd:
/// the DHCPv4 lease from a link's Describe() JSON, a link without DHCP, and an unknown link. udisks2: an
/// ATA disk's SMART status, an NVMe controller's health log (and a failed one), a block device with no
/// drive, one udisks2 doesn't list, and udisks2 not running.
/// And what each reader says without a system bus at all, without systemd-networkd, on an unexpected
/// error from networkd or resolved, and for objects missing the properties it reads.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxDiskSmart.h"
#include "Platform/Linux/LinuxNetworkAdapters.h"
#include "Platform/Linux/SystemdBus.h"

#include <gtest/gtest.h>

#ifdef TASKSMACK_HAS_SDBUS

#include "FakeSystemBus.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <stdlib.h> // NOLINT(modernize-deprecated-headers) - POSIX setenv()/unsetenv(), which <cstdlib> doesn't declare

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

constexpr const char* UDISKS = "org.freedesktop.UDisks2";
constexpr const char* UDISKS_BLOCK = "org.freedesktop.UDisks2.Block";
constexpr const char* UDISKS_ATA = "org.freedesktop.UDisks2.Drive.Ata";
constexpr const char* UDISKS_NVME = "org.freedesktop.UDisks2.NVMe.Controller";
constexpr const char* ATA_DRIVE = "/org/freedesktop/UDisks2/drives/Disk1";
constexpr const char* NVME_DRIVE = "/org/freedesktop/UDisks2/drives/NVMe1";
constexpr const char* FAILING_NVME_DRIVE = "/org/freedesktop/UDisks2/drives/NVMe2";

class SystemdBusFakeSmartTest : public SystemdBusFakeTest
{
  protected:
    /// sda: an ATA disk; nvme0n1 and nvme1n1: NVMe controllers, the second refusing its health log;
    /// sr0: a block device with no drive (an optical drive's empty slot reads "/").
    void serveUdisks()
    {
        auto& props = m_Bus.properties();
        props[LinuxDiskSmart::blockObjectPath("sda")][UDISKS_BLOCK]["Drive"] = ObjectPath{ATA_DRIVE};
        props[LinuxDiskSmart::blockObjectPath("nvme0n1")][UDISKS_BLOCK]["Drive"] = ObjectPath{NVME_DRIVE};
        props[LinuxDiskSmart::blockObjectPath("nvme1n1")][UDISKS_BLOCK]["Drive"] = ObjectPath{FAILING_NVME_DRIVE};
        props[LinuxDiskSmart::blockObjectPath("sr0")][UDISKS_BLOCK]["Drive"] = ObjectPath{"/"};

        auto& ata = props[ATA_DRIVE][UDISKS_ATA];
        ata["SmartSupported"] = true;
        ata["SmartEnabled"] = true;
        ata["SmartFailing"] = false;
        ata["SmartUpdated"] = std::uint64_t{1'760'000'000};
        ata["SmartTemperature"] = 308.15;
        ata["SmartPowerOnSeconds"] = std::uint64_t{3'600'000};
        ata["SmartNumBadSectors"] = std::int64_t{2};

        for (const char* drive : {NVME_DRIVE, FAILING_NVME_DRIVE})
        {
            auto& nvme = props[drive][UDISKS_NVME];
            nvme["SmartUpdated"] = std::uint64_t{1'760'000'100};
            nvme["SmartCriticalWarning"] = std::vector<std::string>{"spare"};
            nvme["SmartTemperature"] = std::uint16_t{310};
        }
        m_Bus.addNoArgMethod(NVME_DRIVE,
                             UDISKS_NVME,
                             "SmartGetAttributes",
                             []() -> MethodAnswer
                             {
                                 return {.path = std::nullopt,
                                         .errorName = {},
                                         .dict = std::map<std::string, TestSupport::DictValue>{
                                             {"percent_used", std::uint8_t{3}},
                                             {"avail_spare", std::uint8_t{100}},
                                             {"media_errors", std::uint64_t{0}},
                                             {"power_on_hours", std::uint64_t{1000}}, // not read: skipped
                                         }};
                             });
        m_Bus.addNoArgMethod(FAILING_NVME_DRIVE,
                             UDISKS_NVME,
                             "SmartGetAttributes",
                             []() -> MethodAnswer { return {.path = std::nullopt, .errorName = "org.freedesktop.UDisks2.Error.Failed"}; });
        ASSERT_TRUE(m_Bus.start(UDISKS)) << m_Bus.error();
    }
};

TEST_F(SystemdBusFakeSmartTest, AnAtaDisksSmartStatusIsRead)
{
    serveUdisks();
    const LinuxDiskSmart::DriveSmartRead read = SystemdBus::makeDriveSmartReader()("sda");
    ASSERT_TRUE(read.smart.has_value()) << read.error;
    const LinuxDiskSmart::DriveSmart smart = read.smart.value_or(LinuxDiskSmart::DriveSmart{});
    EXPECT_EQ(smart.kind, LinuxDiskSmart::DriveSmart::Kind::Ata);
    EXPECT_TRUE(smart.smartSupported);
    EXPECT_TRUE(smart.smartEnabled);
    EXPECT_FALSE(smart.failing);
    EXPECT_EQ(smart.updated, 1'760'000'000U);
    EXPECT_DOUBLE_EQ(smart.temperatureKelvin, 308.15);
    EXPECT_EQ(smart.powerOnSeconds, 3'600'000U);
    EXPECT_EQ(smart.badSectors, 2);
}

TEST_F(SystemdBusFakeSmartTest, AnNvmeControllersHealthLogIsRead)
{
    serveUdisks();
    const LinuxDiskSmart::DriveSmartRead read = SystemdBus::makeDriveSmartReader()("nvme0n1");
    ASSERT_TRUE(read.smart.has_value()) << read.error;
    const LinuxDiskSmart::DriveSmart smart = read.smart.value_or(LinuxDiskSmart::DriveSmart{});
    EXPECT_EQ(smart.kind, LinuxDiskSmart::DriveSmart::Kind::Nvme);
    EXPECT_EQ(smart.updated, 1'760'000'100U);
    EXPECT_EQ(smart.criticalWarnings, (std::vector<std::string>{"spare"}));
    EXPECT_EQ(smart.nvmeTemperatureKelvin, 310);
    EXPECT_TRUE(smart.attributesRead);
    EXPECT_EQ(smart.percentUsed, 3);
    EXPECT_EQ(smart.availableSpare, 100);
    EXPECT_EQ(smart.mediaErrors, 0U);
}

TEST_F(SystemdBusFakeSmartTest, AHealthLogThatFailsIsSaidSoAndTheRestStays)
{
    serveUdisks();
    const LinuxDiskSmart::DriveSmartRead read = SystemdBus::makeDriveSmartReader()("nvme1n1");
    ASSERT_TRUE(read.smart.has_value()) << read.error;
    const LinuxDiskSmart::DriveSmart smart = read.smart.value_or(LinuxDiskSmart::DriveSmart{});
    EXPECT_EQ(smart.kind, LinuxDiskSmart::DriveSmart::Kind::Nvme);
    EXPECT_FALSE(smart.attributesRead);
    EXPECT_FALSE(smart.attributesError.empty());
    EXPECT_EQ(smart.nvmeTemperatureKelvin, 310); // the properties still read
}

TEST_F(SystemdBusFakeSmartTest, ABlockDeviceWithoutADriveHasNoSmart)
{
    serveUdisks();
    const LinuxDiskSmart::DriveSmartRead read = SystemdBus::makeDriveSmartReader()("sr0");
    ASSERT_TRUE(read.smart.has_value()) << read.error;
    EXPECT_EQ(read.smart.value_or(LinuxDiskSmart::DriveSmart{}).kind, LinuxDiskSmart::DriveSmart::Kind::None);
}

TEST_F(SystemdBusFakeSmartTest, ADiskUdisksDoesntListIsSaidSo)
{
    serveUdisks();
    const LinuxDiskSmart::DriveSmartRead read = SystemdBus::makeDriveSmartReader()("sdz");
    EXPECT_FALSE(read.smart.has_value());
    EXPECT_EQ(read.error, "udisks2 doesn't list this disk");
}

TEST_F(SystemdBusFakeTest, WithoutUdisksSmartCantBeRead)
{
    ASSERT_TRUE(m_Bus.start("")) << m_Bus.error();
    const LinuxDiskSmart::DriveSmartRead read = SystemdBus::makeDriveSmartReader()("sda");
    EXPECT_FALSE(read.smart.has_value());
    EXPECT_EQ(read.error, "udisks2 isn't running, so SMART status can't be read");
}

TEST_F(SystemdBusFakeTest, AnAtaDiskMissingASmartPropertyIsAnError)
{
    // sdb's drive says SMART is supported but stops short of the bad-sector count: the read fails as a whole.
    constexpr const char* PARTIAL_ATA_DRIVE = "/org/freedesktop/UDisks2/drives/Disk2";
    auto& props = m_Bus.properties();
    props[LinuxDiskSmart::blockObjectPath("sdb")][UDISKS_BLOCK]["Drive"] = ObjectPath{PARTIAL_ATA_DRIVE};
    auto& ata = props[PARTIAL_ATA_DRIVE][UDISKS_ATA];
    ata["SmartSupported"] = true;
    ata["SmartEnabled"] = true;
    ata["SmartFailing"] = false;
    ata["SmartUpdated"] = std::uint64_t{1'760'000'000};
    ata["SmartTemperature"] = 308.15;
    ata["SmartPowerOnSeconds"] = std::uint64_t{3'600'000};
    ASSERT_TRUE(m_Bus.start(UDISKS)) << m_Bus.error();
    const LinuxDiskSmart::DriveSmartRead read = SystemdBus::makeDriveSmartReader()("sdb");
    EXPECT_FALSE(read.smart.has_value());
    // An unknown property isn't one of the errors put in the user's terms, so udisks2's own is passed on.
    EXPECT_TRUE(read.error.starts_with("udisks2 couldn't be asked: ")) << read.error;
}

// #1395: what each reader says when the bus, a service or an object isn't there as expected.

TEST(SystemdBusNoBusTest, WithoutASystemBusEveryReaderSaysSo)
{
    // A bus address nothing listens on: every reader's first call fails to connect.
    const char* saved = std::getenv("DBUS_SYSTEM_BUS_ADDRESS"); // NOLINT(concurrency-mt-unsafe) - single-threaded test
    const std::optional<std::string> savedAddress = saved != nullptr ? std::optional<std::string>(saved) : std::nullopt;
    ASSERT_EQ(setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/tasksmack-test-bus", 1), 0); // NOLINT(concurrency-mt-unsafe)

    const std::string unreachable = "The system bus couldn't be reached";
    EXPECT_FALSE(SystemdBus::readBootTimestamps().ok);
    EXPECT_FALSE(SystemdBus::readBootTimestamps().error.empty());
    const LinuxDiskSmart::DriveSmartRead smart = SystemdBus::makeDriveSmartReader()("sda");
    EXPECT_TRUE(smart.error.starts_with(unreachable)) << smart.error;
    const LinuxNetworkAdapters::WifiLinkRead wifi = SystemdBus::makeWifiLinkReader()("wlan0");
    EXPECT_TRUE(wifi.error.starts_with(unreachable)) << wifi.error;
    const LinuxNetworkAdapters::DhcpLeaseRead lease = SystemdBus::makeDhcpLeaseReader()("wlan0");
    EXPECT_TRUE(lease.error.starts_with(unreachable)) << lease.error;
    const LinuxNetworkAdapters::LinkDnsRead dns = SystemdBus::makeLinkDnsReader()(2);
    EXPECT_TRUE(dns.error.starts_with(unreachable)) << dns.error;
    const LinuxNetworkAdapters::IndexedDhcpLeaseReader networkd = SystemdBus::makeNetworkdLeaseReader();
    EXPECT_TRUE(networkd(3).error.starts_with(unreachable)) << networkd(3).error;
    EXPECT_TRUE(networkd(4).error.starts_with(unreachable));              // remembered for the reader's other adapters
    const SystemdBus::ServicesRead services = SystemdBus::readServices(); // #1891: the Services list too
    EXPECT_TRUE(services.error.starts_with(unreachable)) << services.error;

    if (savedAddress.has_value())
    {
        setenv("DBUS_SYSTEM_BUS_ADDRESS", savedAddress->c_str(), 1); // NOLINT(concurrency-mt-unsafe)
    }
    else
    {
        unsetenv("DBUS_SYSTEM_BUS_ADDRESS"); // NOLINT(concurrency-mt-unsafe)
    }
}

TEST_F(SystemdBusFakeTest, BootTimestampsMissingOneAreAnError)
{
    auto& manager = m_Bus.properties()["/org/freedesktop/systemd1"]["org.freedesktop.systemd1.Manager"];
    manager["FirmwareTimestampMonotonic"] = std::uint64_t{1'000'000};
    manager["LoaderTimestampMonotonic"] = std::uint64_t{2'000'000}; // and nothing after it
    ASSERT_TRUE(m_Bus.start("org.freedesktop.systemd1")) << m_Bus.error();
    const SystemdBus::BootTimestampsRead read = SystemdBus::readBootTimestamps();
    EXPECT_FALSE(read.ok);
    EXPECT_FALSE(read.error.empty());
}

TEST_F(SystemdBusFakeTest, WithoutNetworkdEveryAdapterSaysSo)
{
    ASSERT_TRUE(m_Bus.start("")) << m_Bus.error();
    const LinuxNetworkAdapters::IndexedDhcpLeaseReader read = SystemdBus::makeNetworkdLeaseReader();
    EXPECT_EQ(read(3).error, "systemd-networkd isn't running");
    EXPECT_EQ(read(4).error, "systemd-networkd isn't running");
}

TEST_F(SystemdBusFakeTest, AnUnexpectedErrorFromNetworkdIsPassedOn)
{
    m_Bus.addIntMethod(NETWORKD_PATH,
                       NETWORKD_MANAGER,
                       "GetLinkByIndex",
                       [](std::int32_t /*ifindex*/) -> MethodAnswer
                       { return {.path = std::nullopt, .errorName = "org.freedesktop.DBus.Error.AccessDenied"}; });
    ASSERT_TRUE(m_Bus.start(NETWORKD)) << m_Bus.error();
    const LinuxNetworkAdapters::DhcpLeaseRead lease = SystemdBus::makeNetworkdLeaseReader()(3);
    EXPECT_TRUE(lease.error.starts_with("systemd-networkd couldn't be asked: ")) << lease.error;
}

TEST_F(SystemdBusFakeTest, AnUnexpectedErrorFromResolvedIsPassedOn)
{
    m_Bus.addIntMethod(RESOLVE_PATH,
                       RESOLVE_MANAGER,
                       "GetLink",
                       [](std::int32_t /*ifindex*/) -> MethodAnswer
                       { return {.path = std::nullopt, .errorName = "org.freedesktop.DBus.Error.AccessDenied"}; });
    ASSERT_TRUE(m_Bus.start(RESOLVE)) << m_Bus.error();
    const LinuxNetworkAdapters::LinkDnsRead dns = SystemdBus::makeLinkDnsReader()(2);
    EXPECT_TRUE(dns.error.starts_with("systemd-resolved couldn't be asked: ")) << dns.error;
}

TEST_F(SystemdBusFakeTest, AnUnexpectedErrorFromNetworkManagerIsPassedOn)
{
    m_Bus.addStringMethod(NM_PATH,
                          NM,
                          "GetDeviceByIpIface",
                          [](const std::string& /*name*/) -> MethodAnswer
                          { return {.path = std::nullopt, .errorName = "org.freedesktop.DBus.Error.AccessDenied"}; });
    ASSERT_TRUE(m_Bus.start(NM)) << m_Bus.error();
    const LinuxNetworkAdapters::WifiLinkReader read = SystemdBus::makeWifiLinkReader();
    const LinuxNetworkAdapters::WifiLinkRead wlan0 = read("wlan0");
    EXPECT_FALSE(wlan0.link.has_value());
    EXPECT_TRUE(wlan0.error.starts_with("NetworkManager couldn't be asked: ")) << wlan0.error;
    // That answer is about one adapter: the next is still asked, not told NetworkManager stopped.
    EXPECT_TRUE(read("wlan1").error.starts_with("NetworkManager couldn't be asked: "));
}

// A service that doesn't answer in time: sd-bus reports it as NoReply (or Timeout), and each reader says
// so in the user's terms rather than passing the D-Bus error name on.
constexpr const char* NO_REPLY = "org.freedesktop.DBus.Error.NoReply";

TEST_F(SystemdBusFakeTest, ANetworkdThatDoesntAnswerInTimeSaysSo)
{
    m_Bus.addIntMethod(NETWORKD_PATH,
                       NETWORKD_MANAGER,
                       "GetLinkByIndex",
                       [](std::int32_t /*ifindex*/) -> MethodAnswer { return {.path = std::nullopt, .errorName = NO_REPLY}; });
    ASSERT_TRUE(m_Bus.start(NETWORKD)) << m_Bus.error();
    EXPECT_EQ(SystemdBus::makeNetworkdLeaseReader()(3).error, "systemd-networkd didn't answer in time");
}

TEST_F(SystemdBusFakeTest, AResolvedThatDoesntAnswerInTimeSaysSo)
{
    m_Bus.addIntMethod(RESOLVE_PATH,
                       RESOLVE_MANAGER,
                       "GetLink",
                       [](std::int32_t /*ifindex*/) -> MethodAnswer { return {.path = std::nullopt, .errorName = NO_REPLY}; });
    ASSERT_TRUE(m_Bus.start(RESOLVE)) << m_Bus.error();
    EXPECT_EQ(SystemdBus::makeLinkDnsReader()(2).error, "systemd-resolved didn't answer in time");
}

TEST_F(SystemdBusFakeTest, ANetworkManagerThatDoesntAnswerInTimeSaysSo)
{
    m_Bus.addStringMethod(NM_PATH,
                          NM,
                          "GetDeviceByIpIface",
                          [](const std::string& /*name*/) -> MethodAnswer { return {.path = std::nullopt, .errorName = NO_REPLY}; });
    ASSERT_TRUE(m_Bus.start(NM)) << m_Bus.error();
    const LinuxNetworkAdapters::WifiLinkRead wlan0 = SystemdBus::makeWifiLinkReader()("wlan0");
    EXPECT_FALSE(wlan0.link.has_value());
    EXPECT_EQ(wlan0.error, "NetworkManager didn't answer in time");
}

TEST_F(SystemdBusFakeTest, AResolvedLinkMissingItsServersOrDomainsIsAnError)
{
    constexpr const char* NO_DOMAINS = "/org/freedesktop/resolve1/link/_35";
    constexpr const char* NO_SERVERS = "/org/freedesktop/resolve1/link/_36";
    m_Bus.addIntMethod(RESOLVE_PATH,
                       RESOLVE_MANAGER,
                       "GetLink",
                       [=](std::int32_t ifindex) -> MethodAnswer
                       { return {.path = ObjectPath{ifindex == 5 ? NO_DOMAINS : NO_SERVERS}, .errorName = {}}; });
    auto& props = m_Bus.properties();
    props[NO_DOMAINS][RESOLVE_LINK]["DNS"] = TestSupport::DnsServers{{2, {192, 168, 1, 1}}};
    props[NO_SERVERS][RESOLVE_LINK]["Domains"] = TestSupport::DnsDomains{{"lan", false}};
    ASSERT_TRUE(m_Bus.start(RESOLVE)) << m_Bus.error();

    const LinuxNetworkAdapters::LinkDnsReader read = SystemdBus::makeLinkDnsReader();
    const LinuxNetworkAdapters::LinkDnsRead noDomains = read(5);
    EXPECT_FALSE(noDomains.read);
    EXPECT_FALSE(noDomains.error.empty());
    const LinuxNetworkAdapters::LinkDnsRead noServers = read(6);
    EXPECT_FALSE(noServers.read);
    EXPECT_FALSE(noServers.error.empty());
}

TEST_F(SystemdBusFakeTest, ADeviceMissingItsWifiOrDhcpPropertiesIsAnError)
{
    // wlan2: a device NetworkManager knows with none of the properties asked for; wlan3: a DHCPv4
    // config with no Options; wlan4: Options without the lease's fields.
    constexpr const char* BARE_DEVICE = "/org/freedesktop/NetworkManager/Devices/5";
    constexpr const char* NO_OPTIONS_DEVICE = "/org/freedesktop/NetworkManager/Devices/6";
    constexpr const char* EMPTY_OPTIONS_DEVICE = "/org/freedesktop/NetworkManager/Devices/7";
    constexpr const char* NO_OPTIONS_CONFIG = "/org/freedesktop/NetworkManager/DHCP4Config/2";
    constexpr const char* EMPTY_OPTIONS_CONFIG = "/org/freedesktop/NetworkManager/DHCP4Config/3";
    m_Bus.addStringMethod(NM_PATH,
                          NM,
                          "GetDeviceByIpIface",
                          [=](const std::string& name) -> MethodAnswer
                          {
                              if (name == "wlan2")
                              {
                                  return {.path = ObjectPath{BARE_DEVICE}, .errorName = {}};
                              }
                              return {.path = ObjectPath{name == "wlan3" ? NO_OPTIONS_DEVICE : EMPTY_OPTIONS_DEVICE}, .errorName = {}};
                          });
    auto& props = m_Bus.properties();
    props[BARE_DEVICE][DEVICE]["Interface"] = std::string("wlan2"); // served, but not the properties read
    props[NO_OPTIONS_DEVICE][DEVICE]["Dhcp4Config"] = ObjectPath{NO_OPTIONS_CONFIG};
    props[NO_OPTIONS_CONFIG][DHCP4]["Unrelated"] = std::string("x");
    props[EMPTY_OPTIONS_DEVICE][DEVICE]["Dhcp4Config"] = ObjectPath{EMPTY_OPTIONS_CONFIG};
    props[EMPTY_OPTIONS_CONFIG][DHCP4]["Options"] = std::map<std::string, TestSupport::DictValue>{{"domain_name", std::string("lan")}};
    ASSERT_TRUE(m_Bus.start(NM)) << m_Bus.error();

    EXPECT_FALSE(SystemdBus::makeWifiLinkReader()("wlan2").error.empty());
    const LinuxNetworkAdapters::DhcpLeaseReader lease = SystemdBus::makeDhcpLeaseReader();
    const LinuxNetworkAdapters::DhcpLeaseRead bare = lease("wlan2");
    EXPECT_FALSE(bare.lease.has_value());
    EXPECT_FALSE(bare.error.empty());
    const LinuxNetworkAdapters::DhcpLeaseRead noOptions = lease("wlan3");
    EXPECT_FALSE(noOptions.lease.has_value());
    EXPECT_FALSE(noOptions.error.empty());
    const LinuxNetworkAdapters::DhcpLeaseRead emptyOptions = lease("wlan4");
    EXPECT_FALSE(emptyOptions.lease.has_value());
    EXPECT_EQ(emptyOptions.error, "NetworkManager reported no lease details");
}

// #2006: systemd's services, as SystemdBus::readServices() reads them: ListUnits, ListUnitFiles, then each
// running service's MainPID.
constexpr const char* SYSTEMD = "org.freedesktop.systemd1";
constexpr const char* SYSTEMD_PATH = "/org/freedesktop/systemd1";
constexpr const char* SYSTEMD_MANAGER = "org.freedesktop.systemd1.Manager";
constexpr const char* SYSTEMD_SERVICE = "org.freedesktop.systemd1.Service";
constexpr const char* CRON_PATH = "/org/freedesktop/systemd1/unit/cron_2eservice";
constexpr const char* GETTY_PATH = "/org/freedesktop/systemd1/unit/getty_40tty1_2eservice";

/// One ListUnits row.
[[nodiscard]] TestSupport::UnitRow
unit(const char* name, const char* description, const char* activeState, const char* subState, const char* path)
{
    TestSupport::UnitRow row;
    row.name = name;
    row.description = description;
    row.activeState = activeState;
    row.subState = subState;
    row.path = path;
    return row;
}

class SystemdBusFakeServicesTest : public SystemdBusFakeTest
{
  protected:
    void serveSystemd()
    {
        m_Bus.addNoArgMethod(
            SYSTEMD_PATH,
            SYSTEMD_MANAGER,
            "ListUnits",
            []() -> MethodAnswer
            {
                return {.path = std::nullopt,
                        .errorName = {},
                        .units = std::vector<TestSupport::UnitRow>{
                            unit("cron.service", "Regular background program processing daemon", "active", "running", CRON_PATH),
                            unit("getty@tty1.service", "Getty on tty1", "active", "running", GETTY_PATH),
                            unit("apt-daily.service",
                                 "Daily apt download activities",
                                 "inactive",
                                 "dead",
                                 "/org/freedesktop/systemd1/unit/apt_2ddaily_2eservice"),
                            unit("dbus.socket", "D-Bus Socket", "active", "running", "/s"),
                        }};
            });
        m_Bus.addNoArgMethod(SYSTEMD_PATH,
                             SYSTEMD_MANAGER,
                             "ListUnitFiles",
                             []() -> MethodAnswer
                             {
                                 return {.path = std::nullopt,
                                         .errorName = {},
                                         .unitFiles = std::vector<std::pair<std::string, std::string>>{
                                             {"/usr/lib/systemd/system/cron.service", "enabled"},
                                             {"/usr/lib/systemd/system/getty@.service", "enabled"},
                                             {"/usr/lib/systemd/system/ssh.service", "disabled"},
                                             {"/usr/lib/systemd/system/dbus.socket", "static"},
                                         }};
                             });
        // try_emplace: a test may have set its own first.
        m_Bus.properties()[CRON_PATH][SYSTEMD_SERVICE].try_emplace("MainPID", std::uint32_t{161});
        m_Bus.properties()[GETTY_PATH][SYSTEMD_SERVICE].try_emplace("MainPID", std::uint32_t{208});
        ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    }
};

TEST_F(SystemdBusFakeServicesTest, TheServicesTheirFilesAndTheRunningOnesMainPidsAreRead)
{
    serveSystemd();
    const SystemdBus::ServicesRead read = SystemdBus::readServices();
    ASSERT_TRUE(read.ok) << read.error;
    ASSERT_EQ(read.listing.units.size(), 3U) << "the services, not the socket";
    EXPECT_EQ(read.listing.units[0].name, "cron.service");
    EXPECT_EQ(read.listing.units[0].description, "Regular background program processing daemon");
    EXPECT_EQ(read.listing.units[0].activeState, "active");
    EXPECT_EQ(read.listing.units[0].subState, "running");
    EXPECT_EQ(read.listing.units[0].objectPath, CRON_PATH);
    // Unit files by file name, the template kept (its instances take its enablement), the socket's not.
    const std::vector<std::pair<std::string, std::string>> files{
        {"cron.service", "enabled"}, {"getty@.service", "enabled"}, {"ssh.service", "disabled"}};
    EXPECT_EQ(read.listing.unitFiles, files);
    // Running services' main processes only: apt-daily is inactive, so it isn't asked.
    EXPECT_EQ(read.listing.mainPids.size(), 2U);
    EXPECT_EQ(read.listing.mainPids.at("cron.service"), 161U);
    EXPECT_EQ(read.listing.mainPids.at("getty@tty1.service"), 208U);
}

TEST_F(SystemdBusFakeTest, WithoutSystemdOnTheBusTheServicesSaySo)
{
    ASSERT_TRUE(m_Bus.start("")) << m_Bus.error();
    const SystemdBus::ServicesRead read = SystemdBus::readServices();
    EXPECT_FALSE(read.ok);
    EXPECT_EQ(read.error, "systemd isn't running");
}

TEST_F(SystemdBusFakeTest, AnUnexpectedErrorFromListUnitsIsPassedOn)
{
    m_Bus.addNoArgMethod(SYSTEMD_PATH,
                         SYSTEMD_MANAGER,
                         "ListUnits",
                         []() -> MethodAnswer { return {.path = std::nullopt, .errorName = "org.freedesktop.DBus.Error.AccessDenied"}; });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const SystemdBus::ServicesRead read = SystemdBus::readServices();
    EXPECT_FALSE(read.ok);
    EXPECT_TRUE(read.error.starts_with("systemd couldn't be asked: ")) << read.error;
}

TEST_F(SystemdBusFakeTest, AFailedListUnitFilesFailsTheRead)
{
    m_Bus.addNoArgMethod(SYSTEMD_PATH,
                         SYSTEMD_MANAGER,
                         "ListUnits",
                         []() -> MethodAnswer
                         { return {.path = std::nullopt, .errorName = {}, .units = std::vector<TestSupport::UnitRow>{}}; });
    m_Bus.addNoArgMethod(SYSTEMD_PATH,
                         SYSTEMD_MANAGER,
                         "ListUnitFiles",
                         []() -> MethodAnswer { return {.path = std::nullopt, .errorName = "org.freedesktop.DBus.Error.AccessDenied"}; });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const SystemdBus::ServicesRead read = SystemdBus::readServices();
    EXPECT_FALSE(read.ok);
    EXPECT_TRUE(read.error.starts_with("systemd couldn't be asked: ")) << read.error;
}

TEST_F(SystemdBusFakeTest, AListUnitsReplyThatIsntAUnitListFailsTheRead)
{
    m_Bus.addNoArgMethod(SYSTEMD_PATH,
                         SYSTEMD_MANAGER,
                         "ListUnits",
                         []() -> MethodAnswer { return {.path = std::nullopt, .errorName = {}, .text = "not a unit list"}; });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const SystemdBus::ServicesRead read = SystemdBus::readServices();
    EXPECT_FALSE(read.ok);
    EXPECT_TRUE(read.error.starts_with("systemd's unit list couldn't be read: ")) << read.error;
}

TEST_F(SystemdBusFakeTest, AListUnitFilesReplyThatIsntAFileListFailsTheRead)
{
    m_Bus.addNoArgMethod(SYSTEMD_PATH,
                         SYSTEMD_MANAGER,
                         "ListUnits",
                         []() -> MethodAnswer
                         { return {.path = std::nullopt, .errorName = {}, .units = std::vector<TestSupport::UnitRow>{}}; });
    m_Bus.addNoArgMethod(SYSTEMD_PATH,
                         SYSTEMD_MANAGER,
                         "ListUnitFiles",
                         []() -> MethodAnswer { return {.path = std::nullopt, .errorName = {}, .text = "not a file list"}; });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const SystemdBus::ServicesRead read = SystemdBus::readServices();
    EXPECT_FALSE(read.ok);
    EXPECT_TRUE(read.error.starts_with("systemd's unit files couldn't be read: ")) << read.error;
}

TEST_F(SystemdBusFakeServicesTest, ARunningServiceWithoutAMainPidIsListedWithoutOne)
{
    // cron's MainPID isn't a number, so it can't be read; getty's is 0, no main process. Both are still listed.
    m_Bus.properties()[CRON_PATH][SYSTEMD_SERVICE]["MainPID"] = std::string("161");
    m_Bus.properties()[GETTY_PATH][SYSTEMD_SERVICE]["MainPID"] = std::uint32_t{0};
    serveSystemd();
    const SystemdBus::ServicesRead read = SystemdBus::readServices();
    ASSERT_TRUE(read.ok) << read.error;
    EXPECT_EQ(read.listing.units.size(), 3U);
    EXPECT_TRUE(read.listing.mainPids.empty());
}

TEST_F(SystemdBusFakeTest, ASystemdThatDoesntAnswerInTimeSaysSo)
{
    m_Bus.addNoArgMethod(
        SYSTEMD_PATH, SYSTEMD_MANAGER, "ListUnits", []() -> MethodAnswer { return {.path = std::nullopt, .errorName = NO_REPLY}; });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const SystemdBus::ServicesRead read = SystemdBus::readServices();
    EXPECT_FALSE(read.ok);
    EXPECT_EQ(read.error, "systemd didn't answer in time");
}

} // namespace
} // namespace Platform

#endif // TASKSMACK_HAS_SDBUS
