/// @file test_CgroupContainer.cpp
/// @brief CgroupContainer::containerFromCgroups() (#805): the container a /proc/[pid]/cgroup file
/// places a process in, for each runtime's cgroup layout.

#include "Platform/Linux/CgroupContainer.h"

#include <gtest/gtest.h>

#include <string>

namespace Platform
{
namespace
{

using CgroupContainer::containerFromCgroups;

// A 64-hex container id, as every runtime here makes one.
constexpr const char* ID = "1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e0f1a2b";

std::string v2(const std::string& path)
{
    return "0::" + path + "\n";
}

TEST(CgroupContainerTest, DockerSystemdDriverScope)
{
    EXPECT_EQ(containerFromCgroups(v2("/system.slice/docker-" + std::string(ID) + ".scope")), "docker:1a2b3c4d5e6f");
}

TEST(CgroupContainerTest, DockerCgroupfsDriverDirectory)
{
    EXPECT_EQ(containerFromCgroups(v2("/docker/" + std::string(ID))), "docker:1a2b3c4d5e6f");
}

TEST(CgroupContainerTest, PodmanRootlessScopeAndCgroupfsDirectory)
{
    EXPECT_EQ(
        containerFromCgroups(v2("/user.slice/user-1000.slice/user@1000.service/user.slice/libpod-" + std::string(ID) + ".scope/container")),
        "podman:1a2b3c4d5e6f");
    EXPECT_EQ(containerFromCgroups(v2("/libpod_parent/libpod-" + std::string(ID))), "podman:1a2b3c4d5e6f");
}

TEST(CgroupContainerTest, TheRuntimeMonitorIsNotInTheContainer)
{
    EXPECT_EQ(containerFromCgroups(v2("/machine.slice/libpod-conmon-" + std::string(ID) + ".scope")), "");
    EXPECT_EQ(containerFromCgroups(v2("/kubepods.slice/crio-conmon-" + std::string(ID) + ".scope")), "");
}

TEST(CgroupContainerTest, KubernetesPodsNameTheirRuntime)
{
    const std::string pod = "/kubepods.slice/kubepods-burstable.slice/kubepods-burstable-pod0f1e2d3c_4b5a.slice/";
    EXPECT_EQ(containerFromCgroups(v2(pod + "cri-containerd-" + ID + ".scope")), "containerd:1a2b3c4d5e6f");
    EXPECT_EQ(containerFromCgroups(v2(pod + "crio-" + ID + ".scope")), "cri-o:1a2b3c4d5e6f");
    // The cgroupfs driver doesn't name the runtime.
    EXPECT_EQ(containerFromCgroups(v2("/kubepods/burstable/pod0f1e2d3c-4b5a/" + std::string(ID))), "k8s:1a2b3c4d5e6f");
}

TEST(CgroupContainerTest, LxcAndMachinedGuestsByName)
{
    EXPECT_EQ(containerFromCgroups(v2("/lxc/web01")), "lxc:web01");
    EXPECT_EQ(containerFromCgroups(v2("/lxc.payload.web01/init.scope")), "lxc:web01");
    EXPECT_EQ(containerFromCgroups(v2("/machine.slice/systemd-nspawn@debian.service/payload")), "nspawn:debian");
    EXPECT_EQ(containerFromCgroups(v2("/machine.slice/machine-fedora.scope")), "machine:fedora");
}

TEST(CgroupContainerTest, TheInnermostContainerNamesANestedOne)
{
    const std::string inner = "2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e0f1a2b3c";
    EXPECT_EQ(containerFromCgroups(v2("/docker/" + std::string(ID) + "/docker/" + inner)), "docker:2b3c4d5e6f7a");
}

TEST(CgroupContainerTest, OrdinaryProcessesAreInNoContainer)
{
    EXPECT_EQ(containerFromCgroups(v2("/user.slice/user-1000.slice/session-2.scope")), "");
    EXPECT_EQ(containerFromCgroups(v2("/system.slice/docker.service")), ""); // the daemon itself
    EXPECT_EQ(containerFromCgroups(v2("/")), "");                            // inside a cgroup namespace
    EXPECT_EQ(containerFromCgroups(""), "");
}

TEST(CgroupContainerTest, IdsThatAreNotHexAreNotContainers)
{
    EXPECT_EQ(containerFromCgroups(v2("/docker/notacontainerid")), "");
    EXPECT_EQ(containerFromCgroups(v2("/system.slice/docker-1a2b3c.scope")), ""); // too short to be one
    EXPECT_EQ(containerFromCgroups(v2("/system.slice/docker-1A2B3C4D5E6F7A8B.scope")), "");
    EXPECT_EQ(containerFromCgroups(v2("/lxc/")), "");
    EXPECT_EQ(containerFromCgroups(v2("/machine.slice/machine-.scope")), "");
}

TEST(CgroupContainerTest, CgroupV1LinesWhenThereIsNoV2Path)
{
    const std::string v1 = "12:memory:/docker/" + std::string(ID) + "\n11:cpu,cpuacct:/docker/" + ID + "\n";
    EXPECT_EQ(containerFromCgroups(v1), "docker:1a2b3c4d5e6f");
    // Hybrid: the v2 line is systemd's alone; the container is in the v1 lines.
    EXPECT_EQ(containerFromCgroups("1:name=systemd:/docker/" + std::string(ID) + "\n0::/\n"), "docker:1a2b3c4d5e6f");
    // A malformed v1 line is skipped.
    EXPECT_EQ(containerFromCgroups("garbage\n3:pids\n"), "");
}

TEST(CgroupContainerTest, TheV2PathWinsWhenItNamesOne)
{
    const std::string other = "2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e0f1a2b3c";
    EXPECT_EQ(containerFromCgroups("4:memory:/docker/" + other + "\n0::/docker/" + ID + "\n"), "docker:1a2b3c4d5e6f");
}

} // namespace
} // namespace Platform
