/// @file test_CgroupService.cpp
/// @brief The systemd service a process runs under, from /proc/[pid]/cgroup (#1886): system and user
/// services, a sub-cgroup inside a service, session and app scopes (no service), the root and a hidden
/// namespace, cgroup v1's name=systemd hierarchy, and the v2 line winning over v1.

#include "Platform/Linux/CgroupService.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

using CgroupService::serviceFromCgroups;

TEST(CgroupServiceTest, AServiceIsTheDeepestUnitInTheV2Path)
{
    EXPECT_EQ(serviceFromCgroups("0::/system.slice/sshd.service\n"), "sshd.service");
    EXPECT_EQ(serviceFromCgroups("0::/system.slice/system-getty.slice/getty@tty1.service\n"), "getty@tty1.service");
    // A user service, by the user manager the system runs it under (#2458): the Services tab lists the system
    // manager's units, where pipewire.service is none, and the session bus's dbus.service isn't the system's.
    EXPECT_EQ(serviceFromCgroups("0::/user.slice/user-1000.slice/user@1000.service/session.slice/pipewire.service\n"), "user@1000.service");
    EXPECT_EQ(serviceFromCgroups("0::/user.slice/user-1000.slice/user@1000.service/session.slice/dbus.service\n"), "user@1000.service");
    // A service that puts its processes in sub-cgroups of its own (Docker's daemon, systemd's delegation).
    EXPECT_EQ(serviceFromCgroups("0::/system.slice/docker.service/payload\n"), "docker.service");
    EXPECT_EQ(serviceFromCgroups("0::/system.slice/sshd.service"), "sshd.service") << "no trailing newline";
}

// #2458: a container's own units are never matched to the host's same-named ones.
TEST(CgroupServiceTest, AContainersUnitsAreNamedByTheHostUnitThatRunsTheContainer)
{
    // A systemd-nspawn guest: its processes belong to the host's systemd-nspawn@deb.service.
    EXPECT_EQ(serviceFromCgroups("0::/machine.slice/systemd-nspawn@deb.service/payload/system.slice/ssh.service\n"),
              "systemd-nspawn@deb.service");
    // Systemd inside a Docker or Podman container: its scope is no service on the host.
    EXPECT_EQ(serviceFromCgroups("0::/system.slice/docker-0123abcd.scope/system.slice/systemd-journald.service\n"), "");
    EXPECT_EQ(serviceFromCgroups("0::/machine.slice/libpod-0123abcd.scope/container/system.slice/sshd.service\n"), "");
}

TEST(CgroupServiceTest, ScopesTheRootAndHiddenPathsAreNoService)
{
    // An app launched from the desktop, under the user manager: its scope, not user@1000.service.
    EXPECT_EQ(serviceFromCgroups("0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-gnome-firefox-1234.scope\n"), "");
    // A login session.
    EXPECT_EQ(serviceFromCgroups("0::/user.slice/user-1000.slice/session-2.scope\n"), "");
    // A kernel thread, and a cgroup namespace that hides everything above it.
    EXPECT_EQ(serviceFromCgroups("0::/\n"), "");
    EXPECT_EQ(serviceFromCgroups(""), "");
    // Not a unit name, only something ending in it.
    EXPECT_EQ(serviceFromCgroups("0::/system.slice/.service\n"), "");
}

TEST(CgroupServiceTest, CgroupV1UsesSystemdsOwnHierarchyAndV2Wins)
{
    EXPECT_EQ(serviceFromCgroups("12:pids:/system.slice/cron.service\n1:name=systemd:/system.slice/cron.service\n"), "cron.service");
    EXPECT_EQ(serviceFromCgroups("12:pids:/system.slice/cron.service\n4:cpu,cpuacct:/\n"), "") << "only systemd's hierarchy names units";
    // Hybrid: the v2 line is systemd's.
    EXPECT_EQ(serviceFromCgroups("1:name=systemd:/system.slice/old.service\n0::/system.slice/new.service\n"), "new.service");
}

} // namespace
} // namespace Platform
