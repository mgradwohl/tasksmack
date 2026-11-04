/// @file test_CgroupProcessType.cpp
/// @brief The Processes table's Type on Linux (#3430): kernel threads and the system's services are System Process, a
/// desktop app's scope is App, and the rest of the user's processes, a container's and a hidden path are Background
/// Process; cgroup v1's name=systemd line is read when there's no v2 line.

#include "Platform/Linux/CgroupProcessType.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

using CgroupProcessType::APP;
using CgroupProcessType::BACKGROUND;
using CgroupProcessType::classify;
using CgroupProcessType::SYSTEM;

TEST(CgroupProcessTypeTest, KernelThreadsAndTheSystemsServicesAreSystem)
{
    EXPECT_EQ(classify("0::/\n", 2, 0), SYSTEM);           // kthreadd
    EXPECT_EQ(classify("0::/\n", 87, 2), SYSTEM);          // a kernel thread
    EXPECT_EQ(classify("", 120, 2), SYSTEM);               // one whose cgroup can't be read
    EXPECT_EQ(classify("0::/init.scope\n", 1, 0), SYSTEM); // systemd itself
    EXPECT_EQ(classify("0::/system.slice/sshd.service\n", 811, 1), SYSTEM);
    EXPECT_EQ(classify("0::/system.slice/system-getty.slice/getty@tty1.service\n", 900, 1), SYSTEM);
}

TEST(CgroupProcessTypeTest, ADesktopAppsScopeIsAnApp)
{
    constexpr const char* USER = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/";
    EXPECT_EQ(classify(std::string(USER) + "app-gnome-firefox-4242.scope\n", 4242, 1500), APP);
    EXPECT_EQ(classify(std::string(USER) + "app-flatpak-org.gimp.GIMP-5151.scope\n", 5151, 1500), APP);
    EXPECT_EQ(classify(std::string(USER) + "snap.firefox.firefox-0a1b2c.scope\n", 6000, 1500), APP);
    // A process deeper inside the app's scope is still the app's.
    EXPECT_EQ(classify(std::string(USER) + "app-org.kde.konsole-777.scope/tab-2\n", 778, 777), APP);
}

TEST(CgroupProcessTypeTest, TheUsersOtherProcessesAContainersAndAHiddenPathAreBackground)
{
    constexpr const char* USER = "0::/user.slice/user-1000.slice/";
    EXPECT_EQ(classify(std::string(USER) + "user@1000.service/app.slice/pipewire.service\n", 1600, 1500), BACKGROUND);
    EXPECT_EQ(classify(std::string(USER) + "user@1000.service/session.slice/org.gnome.Shell@wayland.service\n", 1700, 1500), BACKGROUND);
    // An autostarted program runs as a service, not an app scope.
    EXPECT_EQ(
        classify(std::string(USER) + "user@1000.service/app.slice/app-gnome-evolution\\x2dalarm\\x2dnotify-1800.service\n", 1800, 1500),
        BACKGROUND);
    EXPECT_EQ(classify(std::string(USER) + "session-2.scope\n", 2000, 1999), BACKGROUND); // an ssh or tty login's shell
    EXPECT_EQ(classify("0::/machine.slice/libpod-0123abcd.scope/container\n", 3000, 2999), BACKGROUND);
    EXPECT_EQ(classify("0::/\n", 3100, 1), BACKGROUND); // a cgroup namespace hides the path (WSL, a container)
    EXPECT_EQ(classify("", 3200, 1), BACKGROUND);       // no readable cgroup
}

TEST(CgroupProcessTypeTest, CgroupV1ReadsSystemdsOwnHierarchyAndV2Wins)
{
    EXPECT_EQ(classify("12:cpu,cpuacct:/\n1:name=systemd:/system.slice/cron.service\n", 700, 1), SYSTEM);
    EXPECT_EQ(classify("1:name=systemd:/user.slice/user-1000.slice/user@1000.service/app.slice/app-gnome-gedit-9.scope\n", 9, 8), APP);
    EXPECT_EQ(classify("1:name=systemd:/system.slice/cron.service\n0::/user.slice/user-1000.slice/session-3.scope\n", 10, 1), BACKGROUND);
}

} // namespace
} // namespace Platform
