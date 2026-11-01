/// @file test_SystemdBus.cpp
/// @brief Platform::SystemdBus (#1525) against this machine: with libsystemd and a system bus running
/// systemd, the boot timestamps read and userspace started after the kernel; otherwise the read says
/// why and the test is skipped. Likewise udisks2 (#1631) and NetworkManager (#1518).

#include "Platform/Linux/LinuxDiskSmart.h"
#include "Platform/Linux/LinuxNetworkAdapters.h"
#include "Platform/Linux/SystemdBus.h"

#ifdef TASKSMACK_HAS_SDBUS
#include "Platform/Linux/SdBusCall.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#endif

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

TEST(SystemdBusTest, ReadsThisMachinesBootTimestampsOrSaysWhyNot)
{
    const SystemdBus::BootTimestampsRead read = SystemdBus::readBootTimestamps();
    if (!read.ok)
    {
        EXPECT_FALSE(read.error.empty());
        GTEST_SKIP() << "systemd isn't reachable here: " << read.error;
    }
    EXPECT_TRUE(SystemdBus::built());
    EXPECT_GT(read.userspace, 0U); // systemd itself started after the kernel
    if (read.finish != 0)
    {
        EXPECT_GE(read.finish, read.userspace);
    }
}

// #1689: every system-bus call gives up well before sd-bus's own 25 s default, so a hung service can't
// hold the System Information read for minutes (several calls per disk).
TEST(SystemdBusTest, CallsGiveUpWithinSeconds)
{
    EXPECT_GT(SystemdBus::CALL_TIMEOUT_USEC, 0U);
    EXPECT_LE(SystemdBus::CALL_TIMEOUT_USEC, 5'000'000U);
}

// udisks2 (#1631): this machine's disks' SMART data, or why not (no udisks2 under WSL or in CI).
TEST(SystemdBusTest, ReadsAThisMachineDisksSmartDataOrSaysWhyNot)
{
    const LinuxDiskSmart::SmartReader reader = SystemdBus::makeDriveSmartReader();
    ASSERT_TRUE(reader);
    const LinuxDiskSmart::DriveSmartRead read = reader("sda");
    if (!read.smart.has_value())
    {
        EXPECT_FALSE(read.error.empty());
        GTEST_SKIP() << "udisks2 isn't reachable here: " << read.error;
    }
    EXPECT_TRUE(read.error.empty());
}

// NetworkManager (#1518): a Wi-Fi adapter's link on this machine, or why not (no NetworkManager under
// WSL or in CI). Either way the reader answers, and once NetworkManager is known to be absent it says
// so again without asking.
TEST(SystemdBusTest, ReadsAWifiLinkOrSaysWhyNot)
{
    const LinuxNetworkAdapters::WifiLinkReader reader = SystemdBus::makeWifiLinkReader();
    ASSERT_TRUE(reader);
    const LinuxNetworkAdapters::WifiLinkRead first = reader("wlan0");
    const LinuxNetworkAdapters::WifiLinkRead second = reader("wlan0");
    if (!first.link.has_value())
    {
        EXPECT_FALSE(first.error.empty());
        EXPECT_FALSE(second.link.has_value());
        EXPECT_FALSE(second.error.empty());
        GTEST_SKIP() << "No Wi-Fi link read here: " << first.error;
    }
    EXPECT_TRUE(first.error.empty());
}

// systemd-resolved (#1518): an adapter's own DNS settings on this machine, or why not. Index 1 is the
// loopback adapter, which resolved knows wherever it runs.
TEST(SystemdBusTest, ReadsAnAdaptersDnsSettingsOrSaysWhyNot)
{
    const LinuxNetworkAdapters::LinkDnsReader reader = SystemdBus::makeLinkDnsReader();
    ASSERT_TRUE(reader);
    const LinuxNetworkAdapters::LinkDnsRead read = reader(1);
    if (!read.read)
    {
        EXPECT_FALSE(read.error.empty());
        EXPECT_TRUE(read.servers.empty());
        GTEST_SKIP() << "systemd-resolved isn't reachable here: " << read.error;
    }
    EXPECT_TRUE(read.error.empty());
    // An index no adapter has: resolved says so, for that adapter only.
    const LinuxNetworkAdapters::LinkDnsRead missing = reader(999'999);
    EXPECT_FALSE(missing.read);
    EXPECT_FALSE(missing.error.empty());
    EXPECT_TRUE(reader(1).read);
}

// NetworkManager (#1518): an adapter's DHCP lease on this machine, or why not.
TEST(SystemdBusTest, ReadsADhcpLeaseOrSaysWhyNot)
{
    const LinuxNetworkAdapters::DhcpLeaseReader reader = SystemdBus::makeDhcpLeaseReader();
    ASSERT_TRUE(reader);
    const LinuxNetworkAdapters::DhcpLeaseRead read = reader("eth0");
    if (!read.lease.has_value())
    {
        EXPECT_FALSE(read.error.empty());
        GTEST_SKIP() << "No DHCP lease read here: " << read.error;
    }
    EXPECT_TRUE(read.error.empty());
}

// systemd-networkd (#1518): an adapter's lease from its link description, or why not. Asked without
// D-Bus auto-start, so a stopped networkd stays stopped.
TEST(SystemdBusTest, ReadsANetworkdLeaseOrSaysWhyNot)
{
    const LinuxNetworkAdapters::IndexedDhcpLeaseReader reader = SystemdBus::makeNetworkdLeaseReader();
    ASSERT_TRUE(reader);
    const LinuxNetworkAdapters::DhcpLeaseRead read = reader(1);
    if (!read.lease.has_value())
    {
        EXPECT_FALSE(read.error.empty());
        GTEST_SKIP() << "No networkd lease read here: " << read.error;
    }
    EXPECT_TRUE(read.error.empty());
}

#ifdef TASKSMACK_HAS_SDBUS

// #3338: the owners of what sd-bus hands back with malloc free it on every path, a throw while it is held included,
// and an owner filled twice frees the first. Only a leak detector sees a leak: the Sanitizers job's LeakSanitizer
// fails on one, while a plain debug run checks the owners hand back what was put in and don't crash.
TEST(SdBusOwnersTest, MallocdStringsAndArraysAreFreedWhenACopyThrows)
{
    const auto fillString = [](char** out)
    {
        *out = ::strdup("a remote host name longer than small-string storage");
    };
    const auto fillStrv = [](char*** out)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - an sd-bus-style strv
        auto** names = static_cast<char**>(std::calloc(3, sizeof(char*)));
        names[0] = ::strdup("a.service"); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - a C strv
        names[1] = ::strdup("b.service"); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - a C strv
        *out = names;
    };
    EXPECT_THROW(
        {
            SystemdBus::Detail::MallocString text;
            fillString(std::out_ptr(text));
            EXPECT_NE(text.get(), nullptr);
            throw std::bad_alloc(); // as copying it might
        },
        std::bad_alloc);
    EXPECT_THROW(
        {
            SystemdBus::Detail::MallocStrv names;
            fillStrv(names.out());
            fillStrv(names.out()); // filled again: the first array is freed
            EXPECT_NE(names.get(), nullptr);
            EXPECT_STREQ(names.get()[1], "b.service"); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - a C strv
            throw std::bad_alloc();                    // part way through copying its entries
        },
        std::bad_alloc);
    // Empty: the calls gave nothing, and there is nothing to free.
    const SystemdBus::Detail::MallocString none;
    EXPECT_EQ(none.get(), nullptr);
    const SystemdBus::Detail::MallocStrv noNames;
    EXPECT_EQ(noNames.get(), nullptr);
}

#endif

} // namespace
} // namespace Platform
