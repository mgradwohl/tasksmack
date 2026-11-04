/// @file test_CgroupProcessType.cpp
/// @brief The Processes table's Type on Linux (#3430): kernel threads and the system's services are System Process, a
/// desktop app's scope is App, and the rest of the user's processes, a container's and a hidden path are Background
/// Process; cgroup v1's name=systemd line is read when there's no v2 line.

#include "Platform/Linux/CgroupProcessType.h"

#include <gtest/gtest.h>

#include <string>

namespace Platform
{
namespace
{

using CgroupProcessType::APP;
using CgroupProcessType::BACKGROUND;
using CgroupProcessType::classify;
using CgroupProcessType::SYSTEM;

TEST(CgroupProcessTypeTest, KernelThreadsAreKnownByTheirFlagNotTheirPid)
{
    using CgroupProcessType::isKernelThread;
    EXPECT_TRUE(isKernelThread(0x00208040U));  // kthreadd's flags: PF_KTHREAD among others
    EXPECT_FALSE(isKernelThread(0x00400100U)); // an ordinary process's
    // Inside a container's PID namespace, PID 2 and its children are ordinary processes: their hidden "/" cgroup
    // reads Background, not System (the probe gives System only for the flag).
    EXPECT_EQ(classify("0::/\n", false), BACKGROUND);
}

TEST(CgroupProcessTypeTest, TheSystemsServicesAndInitAreSystem)
{
    EXPECT_EQ(classify("0::/init.scope\n", false), SYSTEM); // systemd itself
    EXPECT_EQ(classify("0::/system.slice/sshd.service\n", false), SYSTEM);
    EXPECT_EQ(classify("0::/system.slice/system-getty.slice/getty@tty1.service\n", false), SYSTEM);
    EXPECT_EQ(classify("0::/system.slice/docker.service\n", false), SYSTEM); // the host's Docker daemon
}

TEST(CgroupProcessTypeTest, ADesktopAppsScopeIsAnApp)
{
    const std::string user = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/";
    EXPECT_EQ(classify(user + "app-gnome-firefox-4242.scope\n", false), APP);
    EXPECT_EQ(classify(user + "app-flatpak-org.gimp.GIMP-5151.scope\n", false), APP);
    EXPECT_EQ(classify(user + "snap.firefox.firefox-0a1b2c.scope\n", false), APP);
    EXPECT_EQ(classify(user + "app-org.kde.konsole-777.scope/tab-2\n", false), APP); // deeper inside the app's scope
}

TEST(CgroupProcessTypeTest, TheUsersOtherProcessesContainersAndAHiddenPathAreBackground)
{
    const std::string user = "0::/user.slice/user-1000.slice/";
    EXPECT_EQ(classify(user + "user@1000.service/app.slice/pipewire.service\n", false), BACKGROUND);
    EXPECT_EQ(classify(user + "user@1000.service/session.slice/org.gnome.Shell@wayland.service\n", false), BACKGROUND);
    // An autostarted program runs as a service, not an app scope.
    EXPECT_EQ(classify(user + "user@1000.service/app.slice/app-gnome-evolution\\x2dalarm\\x2dnotify-1800.service\n", false), BACKGROUND);
    EXPECT_EQ(classify(user + "session-2.scope\n", false), BACKGROUND); // an ssh or tty login's shell
    EXPECT_EQ(classify("0::/machine.slice/libpod-0123abcd.scope/container\n", true), BACKGROUND);
    // Docker's systemd driver puts containers under system.slice: the container wins over the path.
    EXPECT_EQ(classify("0::/system.slice/docker-1a2b3c4d5e6f.scope\n", true), BACKGROUND);
    EXPECT_EQ(classify("0::/\n", false), BACKGROUND); // a cgroup namespace hides the path (WSL, a container)
    EXPECT_EQ(classify("", false), BACKGROUND);       // no readable cgroup
}

TEST(CgroupProcessTypeTest, CgroupV1ReadsSystemdsOwnHierarchyAndV2Wins)
{
    EXPECT_EQ(classify("12:cpu,cpuacct:/\n1:name=systemd:/system.slice/cron.service\n", false), SYSTEM);
    EXPECT_EQ(classify("1:name=systemd:/user.slice/user-1000.slice/user@1000.service/app.slice/app-gnome-gedit-9.scope\n", false), APP);
    EXPECT_EQ(classify("1:name=systemd:/system.slice/cron.service\n0::/user.slice/user-1000.slice/session-3.scope\n", false), BACKGROUND);
}

} // namespace
} // namespace Platform
