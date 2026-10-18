/// @file test_LinuxSystemInfoProbe.cpp
/// @brief The Linux System Information probe as a whole (#1395, #1548): every section read on this
/// machine, and under a fixture root, where the facts come from the root's files and the system-wide
/// sources (udisks2, the journal, NetworkManager, systemd) are left alone. The parsers behind each
/// section are tested on their own in test_LinuxOsInfo.cpp; this covers the probe that ties them together.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxSystemInfoProbe.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace Platform
{
namespace
{

/// A fresh, empty fixture root under the temp directory, removed (best effort) when the test ends.
class FixtureRoot
{
  public:
    FixtureRoot()
        : m_Path(std::filesystem::temp_directory_path() /
                 std::format("ts_sysinfo_probe_{}_{}", s_Counter.fetch_add(1), std::chrono::steady_clock::now().time_since_epoch().count()))
    {
        std::filesystem::create_directories(m_Path);
    }
    ~FixtureRoot() noexcept
    {
        try
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Path, ec); // best effort
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a destructor must not throw; the temp dir is left behind
        {}
    }
    FixtureRoot(const FixtureRoot&) = delete;
    FixtureRoot& operator=(const FixtureRoot&) = delete;
    FixtureRoot(FixtureRoot&&) = delete;
    FixtureRoot& operator=(FixtureRoot&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

    void write(std::string_view relative, std::string_view text) const
    {
        const std::filesystem::path file = m_Path / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

  private:
    std::filesystem::path m_Path;
    static inline std::atomic<int> s_Counter{0};
};

TEST(LinuxSystemInfoProbeTest, ReadsEverySectionOnThisMachine)
{
    LinuxSystemInfoProbe probe;
    EXPECT_TRUE(probe.capabilities().hasOs);
    EXPECT_TRUE(probe.capabilities().unavailableReason.empty());
    EXPECT_FALSE(probe.capabilities().wifiDetailsGated); // no location gating: the Wi-Fi rows stay automatic (#1782)

    const OsInfo os = probe.readOs();
    EXPECT_EQ(os.family, OsFamily::Linux);
    EXPECT_TRUE(os.kernel.starts_with("Linux ")) << os.kernel; // uname()
    EXPECT_FALSE(os.architecture.empty());
    EXPECT_FALSE(os.computerName.empty());

    EXPECT_TRUE(probe.readFirmware().available);
    EXPECT_TRUE(probe.readMemoryModules().available);
    EXPECT_TRUE(probe.readCommitPaging().available);
    EXPECT_TRUE(probe.readStorage().available);
    EXPECT_TRUE(probe.readGraphics().available);
    EXPECT_TRUE(probe.readPlatformSecurity().available);
    EXPECT_TRUE(probe.readSensors().available);
    EXPECT_TRUE(probe.readDevices().available);
    EXPECT_TRUE(probe.readDrivers().available);
    EXPECT_TRUE(probe.readCrashes().available);

    const NetworkAdaptersInfo network = probe.readNetworkAdapters(/*includeWifiDetails=*/false);
    EXPECT_TRUE(network.available);
    EXPECT_TRUE(network.listed); // /sys/class/net is always there
    for (const NetworkAdapter& adapter : network.adapters)
    {
        EXPECT_NE(adapter.name, "lo"); // left out
    }

    // systemd answers, or the section says why it can't (no system bus in a container, no libsystemd).
    const BootPerformanceInfo boot = probe.readBootPerformance();
    EXPECT_TRUE(boot.available);
    EXPECT_TRUE(boot.timingsRead || !boot.unavailableReason.empty());
}

// #2416: System Information's read and the crash history's worker read the crashes at once, each on its
// own probe; the user and group lookups behind the read are reentrant, so neither sees the other's entry.
// Run under TSan by the tsan preset. A bounded number of reads, no busy loop.
TEST(LinuxSystemInfoProbeTest, TwoProbesReadTheCrashesOnTwoThreadsAtOnce)
{
    constexpr int READS = 4;
    LinuxSystemInfoProbe first;
    LinuxSystemInfoProbe second;
    const CrashesInfo expected = first.readCrashes();

    std::array<std::vector<CrashesInfo>, 2> results;
    {
        const std::array<LinuxSystemInfoProbe*, 2> probes{&first, &second};
        std::array<std::jthread, 2> readers;
        for (std::size_t i = 0; i < readers.size(); ++i)
        {
            readers.at(i) = std::jthread(
                [&probe = *probes.at(i), &out = results.at(i)]
                {
                    for (int read = 0; read < READS; ++read)
                    {
                        out.push_back(probe.readCrashes());
                    }
                });
        }
    } // joined

    for (const std::vector<CrashesInfo>& reads : results)
    {
        ASSERT_EQ(reads.size(), static_cast<std::size_t>(READS));
        for (const CrashesInfo& crashes : reads)
        {
            EXPECT_TRUE(crashes.available);
            EXPECT_EQ(crashes.journalRead, expected.journalRead);
            EXPECT_EQ(crashes.otherUsersHidden, expected.otherUsersHidden) << "the group lookups agree on every thread";
        }
    }
}

TEST(LinuxSystemInfoProbeTest, AFixtureRootIsReadFromItsOwnFiles)
{
    const FixtureRoot root;
    root.write("etc/os-release", "PRETTY_NAME=\"Fixture Linux 1.0\"\nVERSION_ID=\"1.0\"\n");
    root.write("proc/1/comm", "fixture-init\n");
    LinuxSystemInfoProbe probe(root.path());

    const OsInfo os = probe.readOs();
    EXPECT_EQ(os.family, OsFamily::Linux);
    EXPECT_EQ(os.name, "Fixture Linux 1.0");
    EXPECT_EQ(os.version, "1.0");
    EXPECT_EQ(os.initSystem, "fixture-init");
    // uname() and the host name are the running system's whatever the root.
    EXPECT_TRUE(os.kernel.starts_with("Linux ")) << os.kernel;

    // Nothing else is in the root: every section is still read, and finds nothing.
    const NetworkAdaptersInfo network = probe.readNetworkAdapters(/*includeWifiDetails=*/false);
    EXPECT_TRUE(network.available);
    EXPECT_FALSE(network.listed); // no sys/class/net here
    EXPECT_TRUE(network.adapters.empty());
    EXPECT_FALSE(network.dnsRead);

    const SensorsInfo sensors = probe.readSensors();
    EXPECT_TRUE(sensors.available);
    EXPECT_TRUE(sensors.devices.empty());

    const DriversInfo drivers = probe.readDrivers();
    EXPECT_TRUE(drivers.available);
    EXPECT_TRUE(drivers.drivers.empty());

    // The system-wide sources belong to this machine, not the root, so they aren't asked.
    const CrashesInfo crashes = probe.readCrashes();
    EXPECT_TRUE(crashes.available);
    EXPECT_TRUE(crashes.events.empty());

    const StorageInfo storage = probe.readStorage();
    EXPECT_TRUE(storage.available);
    EXPECT_TRUE(storage.disks.empty());

    const BootPerformanceInfo boot = probe.readBootPerformance();
    EXPECT_TRUE(boot.available);
    EXPECT_FALSE(boot.timingsRead);
    EXPECT_EQ(boot.unavailableReason, "Not read under a test root");

    EXPECT_TRUE(probe.readFirmware().available);
    EXPECT_TRUE(probe.readMemoryModules().available);
    EXPECT_TRUE(probe.readCommitPaging().available);
    EXPECT_TRUE(probe.readGraphics().available);
    EXPECT_TRUE(probe.readPlatformSecurity().available);
    EXPECT_TRUE(probe.readDevices().available);
}

// #1782: Linux gates no Wi-Fi detail behind location access, so a read without the Wi-Fi details asked
// for reads a Wi-Fi adapter just as one with them does, and withholds nothing.
TEST(LinuxSystemInfoProbeTest, WifiRowsDontWaitForShowWifiDetails)
{
    const FixtureRoot root;
    root.write("sys/class/net/wlp3s0/address", "3c:22:fb:00:11:22\n");
    root.write("sys/class/net/wlp3s0/mtu", "1500\n");
    root.write("sys/class/net/wlp3s0/operstate", "up\n");
    std::filesystem::create_directories(root.path() / "sys/class/net/wlp3s0/wireless");
    root.write("proc/net/wireless", "Inter-| sta-|\n face | tus |\nwlp3s0: 0000   54.  -61.  -256 0 0 0 0 0 0\n");
    LinuxSystemInfoProbe probe(root.path());

    const NetworkAdaptersInfo automatic = probe.readNetworkAdapters(/*includeWifiDetails=*/false);
    const NetworkAdaptersInfo asked = probe.readNetworkAdapters(/*includeWifiDetails=*/true);
    ASSERT_EQ(automatic.adapters.size(), 1U);
    ASSERT_EQ(asked.adapters.size(), 1U);
    const NetworkAdapter& wifi = automatic.adapters[0];
    EXPECT_TRUE(wifi.wireless);
    EXPECT_FALSE(wifi.wifiDetailsWithheld);
    EXPECT_EQ(wifi.wifiSignalDbm, std::optional<int>(-61));
    EXPECT_EQ(wifi.wifiSignalDbm, asked.adapters[0].wifiSignalDbm);
    EXPECT_EQ(wifi.wifiLink.has_value(), asked.adapters[0].wifiLink.has_value());
    EXPECT_EQ(wifi.wifiLinkUnavailable, asked.adapters[0].wifiLinkUnavailable);
    EXPECT_FALSE(asked.adapters[0].wifiDetailsWithheld);
}

} // namespace
} // namespace Platform
