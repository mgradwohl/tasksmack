/// @file test_RootCapabilities.cpp
/// @brief What an Administrator session keeps of root's capabilities (#3621).

#include "Platform/Linux/RootCapabilities.h"
#include "Platform/Linux/UdpByteCounter.h"
#include "Support/CiRealSystem.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

#include <linux/capability.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Platform::RootCapabilities
{
namespace
{

/// The CapEff line of /proc/self/status, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> ownEffective()
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
    {
        if (line.starts_with("CapEff:"))
        {
            return std::stoull(line.substr(7), nullptr, 16);
        }
    }
    return std::nullopt;
}

constexpr std::uint64_t bit(unsigned capability)
{
    return std::uint64_t{1} << capability;
}

TEST(RootCapabilitiesTest, TheBitNumbersAreTheKernels)
{
    EXPECT_EQ(CHOWN, static_cast<unsigned>(CAP_CHOWN));
    EXPECT_EQ(DAC_OVERRIDE, static_cast<unsigned>(CAP_DAC_OVERRIDE));
    EXPECT_EQ(DAC_READ_SEARCH, static_cast<unsigned>(CAP_DAC_READ_SEARCH));
    EXPECT_EQ(FOWNER, static_cast<unsigned>(CAP_FOWNER));
    EXPECT_EQ(KILL, static_cast<unsigned>(CAP_KILL));
    EXPECT_EQ(SETGID, static_cast<unsigned>(CAP_SETGID));
    EXPECT_EQ(SETUID, static_cast<unsigned>(CAP_SETUID));
    EXPECT_EQ(SYS_PTRACE, static_cast<unsigned>(CAP_SYS_PTRACE));
    EXPECT_EQ(SYS_NICE, static_cast<unsigned>(CAP_SYS_NICE));
    EXPECT_EQ(SYS_RESOURCE, static_cast<unsigned>(CAP_SYS_RESOURCE));
    EXPECT_EQ(SYSLOG, static_cast<unsigned>(CAP_SYSLOG));
#ifdef CAP_BPF
    EXPECT_EQ(PERFMON, static_cast<unsigned>(CAP_PERFMON));
    EXPECT_EQ(BPF, static_cast<unsigned>(CAP_BPF));
#endif
}

TEST(RootCapabilitiesTest, ANarrowedRootKeepsOnlyWhatTaskSmackUses)
{
    const std::uint64_t everything = (std::uint64_t{1} << 41U) - 1; // every capability through CAP_CHECKPOINT_RESTORE
    const Sets root{.effective = everything, .permitted = everything, .inheritable = 0};
    const Sets kept = narrowed(root);
    EXPECT_EQ(kept.effective, keptMask());
    EXPECT_EQ(kept.permitted, keptMask());
    EXPECT_EQ(kept.inheritable, 0U);

    EXPECT_NE(keptMask() & bit(CAP_SYS_PTRACE), 0U);
    EXPECT_NE(keptMask() & bit(CAP_KILL), 0U);
    EXPECT_NE(keptMask() & bit(CAP_SETUID), 0U);
    EXPECT_EQ(keptMask() & bit(CAP_SYS_ADMIN), 0U) << "dropped: older kernels lose UDP counting as root";
    EXPECT_EQ(keptMask() & bit(CAP_SYS_MODULE), 0U);
    EXPECT_EQ(keptMask() & bit(CAP_SYS_RAWIO), 0U);
    EXPECT_EQ(keptMask() & bit(CAP_SYS_BOOT), 0U);
    EXPECT_EQ(keptMask() & bit(CAP_SYS_TIME), 0U);
    EXPECT_EQ(keptMask() & bit(CAP_NET_ADMIN), 0U);
    EXPECT_EQ(keptMask() & bit(CAP_MKNOD), 0U);
    EXPECT_EQ(keptMask() & bit(CAP_SETFCAP), 0U);
    EXPECT_EQ(keptMask() & bit(CAP_SETPCAP), 0U);

    // Narrowing never adds one, and narrowing twice changes nothing more.
    const Sets some{.effective = bit(CAP_KILL) | bit(CAP_SYS_ADMIN), .permitted = bit(CAP_KILL), .inheritable = bit(CAP_SYS_ADMIN)};
    EXPECT_EQ(narrowed(some), (Sets{.effective = bit(CAP_KILL), .permitted = bit(CAP_KILL), .inheritable = bit(CAP_SYS_ADMIN)}));
    EXPECT_EQ(narrowed(narrowed(root)), kept);
}

TEST(RootCapabilitiesTest, NotRootNothingChanges)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "running as root: RootCapabilitiesRootTest covers that";
    }
    const std::optional<std::uint64_t> before = ownEffective();
    const DropResult result = dropUnused();
    EXPECT_FALSE(result.root);
    EXPECT_FALSE(result.dropped);
    EXPECT_EQ(ownEffective(), before);
}

// As root (the coverage job's root pass, tools/coverage.sh), in a child so the test binary keeps its own sets: the
// drop leaves exactly keptMask() effective, and a program the child then execs as root gets full root back.
TEST(RootCapabilitiesRootTest, RootKeepsOnlyTheUsedSetAndAProgramItExecsIsFullRoot)
{
    if (::geteuid() != 0)
    {
        GTEST_SKIP() << "needs root: the coverage job's root pass runs this suite under sudo";
    }
    const std::optional<std::uint64_t> rootBefore = ownEffective();
    ASSERT_TRUE(rootBefore.has_value());
    std::array<int, 2> pipeFds{-1, -1};
    ASSERT_EQ(::pipe(pipeFds.data()), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        ::close(pipeFds[0]);
        const DropResult result = dropUnused();
        const std::optional<std::uint64_t> after = ownEffective();
        const bool ok = result.root && result.dropped && after == keptMask() && result.after.effective == keptMask();
        if (!ok)
        {
            ::_exit(2);
        }
        // A root exec: /bin/sh reports its own CapEff on the pipe.
        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::execl("/bin/sh", "sh", "-c", "grep '^CapEff:' /proc/self/status", static_cast<char*>(nullptr));
        ::_exit(3);
    }
    ::close(pipeFds[1]);
    std::string out;
    std::array<char, 256> buffer{};
    for (ssize_t got = 0; (got = ::read(pipeFds[0], buffer.data(), buffer.size())) > 0;)
    {
        out.append(buffer.data(), static_cast<std::size_t>(got));
    }
    ::close(pipeFds[0]);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0) << "2: the drop didn't leave keptMask(); 3: exec failed";
    ASSERT_TRUE(out.starts_with("CapEff:")) << out;
    EXPECT_EQ(std::stoull(out.substr(7), nullptr, 16), *rootBefore) << "the exec'd program is full root again (D4)";
}

// #3621: what an Administrator session keeps is enough to load the eBPF UDP counter (#3366), which "Count UDP traffic
// per process" loads after the drop: CAP_BPF, CAP_PERFMON and CAP_SYS_RESOURCE, without CAP_SYS_ADMIN. In a child, so
// the test binary keeps its sets; a real-system test, as UdpByteCounterRootTest is.
TEST(RootCapabilitiesRootTest, TheNarrowedSetStillLoadsTheUdpCounter)
{
    if (::geteuid() != 0)
    {
        GTEST_SKIP() << "needs root: CI runs this suite under sudo";
    }
    ::TestSupport::requireCiRealSystem();
    if (::testing::Test::IsSkipped())
    {
        return;
    }
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        const DropResult result = dropUnused();
        if (!result.dropped)
        {
            ::_exit(2);
        }
        const auto counter = UdpByteCounter::load();
        ::_exit(counter.has_value() && counter->read().has_value() ? 0 : 3);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "2: the drop failed; 3: the counter didn't load or read with the narrowed set";
}

} // namespace
} // namespace Platform::RootCapabilities
