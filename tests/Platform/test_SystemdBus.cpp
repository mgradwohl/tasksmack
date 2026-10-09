/// @file test_SystemdBus.cpp
/// @brief Platform::SystemdBus (#1525) against this machine: with libsystemd and a system bus running
/// systemd, the boot timestamps read and userspace started after the kernel; otherwise the read says
/// why and the test is skipped.

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

} // namespace
} // namespace Platform
