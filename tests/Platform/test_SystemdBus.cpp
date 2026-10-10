/// @file test_SystemdBus.cpp
/// @brief Platform::SystemdBus (#1525) against this machine: with libsystemd and a system bus running
/// systemd, the boot timestamps read and userspace started after the kernel; otherwise the read says
/// why and the test is skipped.

#include "Platform/Linux/LinuxDiskSmart.h"
#include "Platform/Linux/SystemdBus.h"

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

} // namespace
} // namespace Platform
