/// @file test_LinuxProcessProbe.cpp
/// @brief Integration tests for Platform::LinuxProcessProbe
///
/// This file contains two kinds of coverage:
///   - Integration tests that interact with the real /proc filesystem, verifying
///     that the probe correctly reads and parses process information.
///   - Error-path / injection tests that use a synthetic proc root (ScopedTempDir)
///     to exercise missing-file and malformed-input handling without touching /proc.

#include <gtest/gtest.h>

// Gate Linux-only integration tests by header availability + target platform.
// This avoids Windows setups that may have partial POSIX headers.
#if defined(__linux__) && __has_include(<unistd.h>)
#define TASKSMACK_HAS_UNISTD 1
#else
#define TASKSMACK_HAS_UNISTD 0
#endif

#if TASKSMACK_HAS_UNISTD

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessProbe.h"
#include "Platform/Linux/ElfArchitecture.h"
#include "Platform/Linux/LinuxProcessProbe.h"
#include "Platform/Linux/ProcFdScan.h"
#include "Platform/Linux/ProcPrivileges.h"
#include "Platform/PlatformConfig.h"
#include "Platform/ProcessArchitecture.h"
#include "Platform/ProcessTypes.h"
#include "Platform/ScopedTempDir.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "Domain/SocketTrafficAccumulator.h"
#include "Platform/Linux/NetlinkSocketStats.h"
#include "Platform/NetlinkTestUtils.h"

#include <memory>
#include <vector>

#include <sys/socket.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <latch>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <linux/prctl.h>
#include <sched.h>
// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides kill(), csignal does not
#include <signal.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#else

TEST(LinuxProcessProbeTest, SkippedOnNonLinux)
{
    GTEST_SKIP() << "LinuxProcessProbe tests require Linux (/proc, unistd.h)";
}

#endif

#if TASKSMACK_HAS_UNISTD

namespace Platform
{
namespace
{

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(LinuxProcessProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ LinuxProcessProbe probe; });
}

TEST(LinuxProcessProbeTest, CapabilitiesReportedCorrectly)
{
    LinuxProcessProbe probe;
    auto caps = probe.capabilities();

    // Linux should support most capabilities
    EXPECT_TRUE(caps.hasUserSystemTime);
    EXPECT_TRUE(caps.hasStartTime);
    EXPECT_TRUE(caps.hasThreadCount);
    // Shared memory comes from /proc/[pid]/statm; the process details chart draws it only when
    // this is set (#1035).
    EXPECT_TRUE(caps.hasSharedMemory);
    // Peak RSS is the kernel's VmHWM (#1184), and the page-fault count doesn't wrap.
    EXPECT_TRUE(caps.hasPeakRss);
    EXPECT_EQ(caps.pageFaultCountBits, 64U);
}

TEST(LinuxProcessProbeTest, ReducedPrivilegesMatchesEuidAndEffectiveCapabilities)
{
    const LinuxProcessProbe probe;
    const auto caps = probe.capabilities();

    // Reading other users' data needs CAP_SYS_PTRACE + CAP_DAC_READ_SEARCH (or CAP_DAC_OVERRIDE); acting
    // on them CAP_KILL and CAP_SYS_NICE (#2303). Root with its normal capabilities has them all.
    std::ifstream statusFile("/proc/self/status");
    const std::string status{std::istreambuf_iterator<char>(statusFile), std::istreambuf_iterator<char>()};
    const std::optional<std::uint64_t> capEff = ProcPrivileges::parseCapEff(status);
    const std::uint8_t readAndAct = caps.missingPrivileges & static_cast<std::uint8_t>(~MissingPrivilege::PROCESSES_HIDDEN);
    EXPECT_EQ(readAndAct, ProcPrivileges::missingPrivileges(geteuid() == 0, capEff, std::nullopt, false));
    EXPECT_EQ(caps.hasReducedPrivileges, caps.missingPrivileges != 0);
}

// #2303: the /proc mount's hidepid= and gid= from mountinfo, its words and numbers; the last /proc wins.
TEST(ProcPrivilegesTest, ParsesTheProcMountsHidepidAndGid)
{
    constexpr std::string_view PLAIN = "22 28 0:21 / /proc rw,nosuid,nodev,noexec,relatime shared:12 - proc proc rw\n";
    ASSERT_TRUE(ProcPrivileges::parseProcMount(PLAIN).has_value());
    EXPECT_EQ(ProcPrivileges::parseProcMount(PLAIN)->hidepid, 0);
    EXPECT_FALSE(ProcPrivileges::parseProcMount(PLAIN)->gid.has_value());

    constexpr std::string_view HIDDEN = "1 0 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                        "22 28 0:21 / /proc rw,relatime shared:12 - proc proc rw,hidepid=invisible,gid=1001\n";
    const auto hidden = ProcPrivileges::parseProcMount(HIDDEN);
    ASSERT_TRUE(hidden.has_value());
    EXPECT_EQ(hidden->hidepid, 2);
    EXPECT_EQ(hidden->gid, std::optional<std::uint32_t>{1001});

    EXPECT_EQ(ProcPrivileges::parseProcMount("5 1 0:5 / /proc rw - proc proc rw,hidepid=2\n")->hidepid, 2);
    EXPECT_EQ(ProcPrivileges::parseProcMount("5 1 0:5 / /proc rw - proc proc rw,hidepid=ptraceable\n")->hidepid, 4);
    EXPECT_EQ(ProcPrivileges::parseProcMount("5 1 0:5 / /proc rw - proc proc rw,hidepid=noaccess\n")->hidepid, 1);
    // A later mount of /proc hides the earlier one.
    EXPECT_EQ(ProcPrivileges::parseProcMount(std::string(PLAIN) + "9 1 0:9 / /proc rw - proc proc rw,hidepid=2\n")->hidepid, 2);
    // Not /proc, or not a proc filesystem there: no answer.
    EXPECT_FALSE(ProcPrivileges::parseProcMount("1 0 8:1 / / rw - ext4 /dev/sda1 rw\n").has_value());
    EXPECT_FALSE(ProcPrivileges::parseProcMount("3 1 0:3 / /proc rw - tmpfs tmpfs rw\n").has_value());
    EXPECT_FALSE(ProcPrivileges::parseProcMount("").has_value());
}

// #2303: each missing permission is its own bit.
TEST(ProcPrivilegesTest, MissingPrivilegesHasABitForEachPermission)
{
    constexpr std::uint64_t DAC_READ_SEARCH = std::uint64_t{1} << ProcPrivileges::CAP_DAC_READ_SEARCH_BIT;
    constexpr std::uint64_t SYS_PTRACE = std::uint64_t{1} << ProcPrivileges::CAP_SYS_PTRACE_BIT;
    constexpr std::uint64_t KILL = std::uint64_t{1} << ProcPrivileges::CAP_KILL_BIT;
    constexpr std::uint64_t SYS_NICE = std::uint64_t{1} << ProcPrivileges::CAP_SYS_NICE_BIT;
    namespace Missing = MissingPrivilege;
    const auto missing = [](bool root, std::optional<std::uint64_t> capEff)
    {
        return ProcPrivileges::missingPrivileges(root, capEff, std::nullopt, false);
    };

    EXPECT_EQ(missing(true, 0x1ffffffffffULL), 0); // root with its normal capabilities
    EXPECT_EQ(missing(false, DAC_READ_SEARCH | SYS_PTRACE | KILL | SYS_NICE), 0);
    // The FAQ's old setcap line (read access only) can't act on other users' processes.
    EXPECT_EQ(missing(false, DAC_READ_SEARCH | SYS_PTRACE), Missing::SIGNAL_OTHERS | Missing::RAISE_PRIORITY);
    EXPECT_EQ(missing(false, KILL | SYS_NICE), Missing::READ_OTHERS);
    EXPECT_EQ(missing(true, 0), Missing::READ_OTHERS | Missing::SIGNAL_OTHERS | Missing::RAISE_PRIORITY); // dropped
    // Unknown capabilities: root has them all, anyone else none.
    EXPECT_EQ(missing(true, std::nullopt), 0);
    EXPECT_EQ(missing(false, std::nullopt), Missing::READ_OTHERS | Missing::SIGNAL_OTHERS | Missing::RAISE_PRIORITY);

    // hidepid=invisible hides other users' processes, except from its gid= group and CAP_SYS_PTRACE.
    const ProcPrivileges::ProcMount invisible{.hidepid = 2, .gid = 1001};
    constexpr std::uint64_t ALL = DAC_READ_SEARCH | SYS_PTRACE | KILL | SYS_NICE;
    EXPECT_EQ(ProcPrivileges::missingPrivileges(false, KILL | SYS_NICE, invisible, false) & Missing::PROCESSES_HIDDEN,
              Missing::PROCESSES_HIDDEN);
    EXPECT_EQ(ProcPrivileges::missingPrivileges(false, KILL | SYS_NICE, invisible, true) & Missing::PROCESSES_HIDDEN, 0);
    EXPECT_EQ(ProcPrivileges::missingPrivileges(false, ALL, invisible, false), 0);
    // hidepid=noaccess lists them but refuses opening their directories, so they can't be shown either
    // (#2399): hidden too, except from its gid= group and CAP_SYS_PTRACE.
    const ProcPrivileges::ProcMount noaccess{.hidepid = 1, .gid = std::nullopt};
    EXPECT_EQ(ProcPrivileges::missingPrivileges(false, KILL | SYS_NICE, noaccess, false), Missing::READ_OTHERS | Missing::PROCESSES_HIDDEN);
    EXPECT_EQ(ProcPrivileges::missingPrivileges(false, KILL | SYS_NICE, noaccess, true) & Missing::PROCESSES_HIDDEN, 0);
    EXPECT_EQ(ProcPrivileges::missingPrivileges(false, ALL, noaccess, false), 0);
}

// #1327 review: the privilege notice fired for every non-root process, including one granted the
// capabilities docs/guide/faq.md recommends. CapEff (hex) in /proc/self/status decides instead.
// #2400 (#2479): a user in more groups than a fixed buffer holds (more than 256, common under LDAP/SSSD)
// still has every group read: the list is sized by asking first.
TEST(ProcPrivilegesTest, EverySupplementaryGroupIsReadHoweverMany)
{
    constexpr int GROUPS = 300;
    const auto fakeGetgroups = [](int size, gid_t* list) -> int
    {
        if (size == 0)
        {
            return GROUPS;
        }
        if (size < GROUPS)
        {
            errno = EINVAL;
            return -1;
        }
        for (int i = 0; i < GROUPS; ++i)
        {
            list[i] = static_cast<gid_t>(1000 + i); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - getgroups' C array
        }
        return GROUPS;
    };
    const std::vector<gid_t> groups = ProcPrivileges::supplementaryGroups(fakeGetgroups);
    ASSERT_EQ(groups.size(), static_cast<std::size_t>(GROUPS));
    EXPECT_NE(std::ranges::find(groups, static_cast<gid_t>(1299)), groups.end()) << "the last group, past any fixed buffer";

    const auto unreadable = [](int /*size*/, gid_t* /*list*/) -> int
    {
        errno = EPERM;
        return -1;
    };
    EXPECT_TRUE(ProcPrivileges::supplementaryGroups(unreadable).empty());
}

TEST(ProcPrivilegesTest, ParsesCapEffFromStatus)
{
    constexpr std::string_view STATUS = "Name:\tTaskSmack\nCapInh:\t0000000000000000\nCapPrm:\t0000000000080004\n"
                                        "CapEff:\t0000000000080004\nCapBnd:\t000001ffffffffff\n";
    EXPECT_EQ(ProcPrivileges::parseCapEff(STATUS), std::optional<std::uint64_t>{0x80004});
    EXPECT_EQ(ProcPrivileges::parseCapEff("CapEff:\t000001ffffffffff"), std::optional<std::uint64_t>{0x1ffffffffff});
    EXPECT_EQ(ProcPrivileges::parseCapEff("CapEff:\t0000000000000000\n"), std::optional<std::uint64_t>{0});
}

TEST(ProcPrivilegesTest, MissingOrMalformedCapEffIsUnknown)
{
    EXPECT_EQ(ProcPrivileges::parseCapEff(""), std::nullopt);
    EXPECT_EQ(ProcPrivileges::parseCapEff("Name:\tx\nCapPrm:\t0000000000080004\n"), std::nullopt);
    EXPECT_EQ(ProcPrivileges::parseCapEff("CapEff:\n"), std::nullopt);
    EXPECT_EQ(ProcPrivileges::parseCapEff("CapEff:\tzz00000000000000\n"), std::nullopt);
    EXPECT_EQ(ProcPrivileges::parseCapEff("CapEff:\t00000000000800g4\n"), std::nullopt);
    // Only a whole "CapEff:" key at a line start counts.
    EXPECT_EQ(ProcPrivileges::parseCapEff("XCapEff:\t0000000000080004\n"), std::nullopt);
}

TEST(ProcPrivilegesTest, KnownCapabilitiesDecideForRootTooAndAnUnknownSetFallsBackToTheEuid)
{
    constexpr std::uint64_t DAC_READ_SEARCH = std::uint64_t{1} << 2;
    constexpr std::uint64_t SYS_PTRACE = std::uint64_t{1} << 19;
    constexpr std::uint64_t DAC_OVERRIDE = std::uint64_t{1} << 1;

    // Root with an unreadable status falls back to the EUID; root with its full set is not reduced.
    EXPECT_FALSE(ProcPrivileges::hasReducedPrivileges(true, std::nullopt));
    EXPECT_FALSE(ProcPrivileges::hasReducedPrivileges(true, 0x1ffffffffffULL));
    // Root with its capabilities dropped (a container, a hardened service) is reduced like anyone else.
    EXPECT_TRUE(ProcPrivileges::hasReducedPrivileges(true, 0));
    EXPECT_TRUE(ProcPrivileges::hasReducedPrivileges(true, SYS_PTRACE));
    // CAP_DAC_OVERRIDE covers CAP_DAC_READ_SEARCH for these reads.
    EXPECT_FALSE(ProcPrivileges::hasReducedPrivileges(false, DAC_OVERRIDE | SYS_PTRACE));
    EXPECT_FALSE(ProcPrivileges::hasReducedPrivileges(false, DAC_READ_SEARCH | SYS_PTRACE));
    EXPECT_FALSE(ProcPrivileges::hasReducedPrivileges(false, 0x1ffffffffffULL));

    // Anything less than both is reduced: CAP_DAC_READ_SEARCH alone restores FD counts, not I/O or network.
    EXPECT_TRUE(ProcPrivileges::hasReducedPrivileges(false, DAC_READ_SEARCH));
    EXPECT_TRUE(ProcPrivileges::hasReducedPrivileges(false, SYS_PTRACE));
    EXPECT_TRUE(ProcPrivileges::hasReducedPrivileges(false, 0));
    EXPECT_TRUE(ProcPrivileges::hasReducedPrivileges(false, std::nullopt));
}

TEST(LinuxProcessProbeTest, TicksPerSecondIsPositive)
{
    LinuxProcessProbe probe;
    auto ticks = probe.ticksPerSecond();

    // Common values are 100 (older systems) or 250+ (modern systems)
    EXPECT_GT(ticks, 0);
    EXPECT_LE(ticks, 10000); // Sanity check
}

TEST(LinuxProcessProbeTest, TotalCpuTimeIsPositive)
{
    LinuxProcessProbe probe;
    auto totalCpu = probe.totalCpuTime();

    // System should have accumulated some CPU time
    EXPECT_GT(totalCpu, 0ULL);
}

TEST(LinuxProcessProbeTest, TotalCpuTimeIncreases)
{
    LinuxProcessProbe probe;
    auto time1 = probe.totalCpuTime();

    // Do some work to consume CPU
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    volatile std::uint64_t sum = 0; // unsigned, so the busy loop's wrap-around is defined (#1090)
    for (std::uint64_t i = 0; i < 1000000; ++i)
    {
        sum += i;
    }

    auto time2 = probe.totalCpuTime();

    // Total CPU time should increase
    EXPECT_GE(time2, time1);
}

TEST(LinuxProcessProbeTest, SystemTotalMemoryIsPositive)
{
    LinuxProcessProbe probe;
    auto totalMem = probe.systemTotalMemory();

    // Should report some amount of memory (at least 128 MB for modern systems)
    EXPECT_GT(totalMem, 128ULL * 1024 * 1024);
}

// =============================================================================
// Process Enumeration Tests
// =============================================================================

TEST(LinuxProcessProbeTest, EnumerateReturnsProcesses)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    // Should find at least a few processes (init, kernel threads, this test, etc.)
    EXPECT_GT(processes.size(), 0ULL);
}

/// A child process for one test, killed and reaped when it goes out of scope.
class ChildProcess
{
  public:
    explicit ChildProcess(pid_t pid) : m_Pid(pid)
    {}
    ~ChildProcess()
    {
        if (m_Pid > 0)
        {
            ::kill(m_Pid, SIGKILL);
            int status = 0;
            ::waitpid(m_Pid, &status, 0);
        }
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    [[nodiscard]] pid_t pid() const noexcept
    {
        return m_Pid;
    }

  private:
    pid_t m_Pid = -1;
};

/// Not UTF-8: a lead byte without its continuation, a stray continuation and 0xFF (#1648). NUL-terminated
/// for prctl(), and as a view for the comparisons.
constexpr const char* NOT_UTF8_CSTR = "bad\xC3(\x80\xFFname";
constexpr std::string_view NOT_UTF8 = NOT_UTF8_CSTR;

[[nodiscard]] std::optional<ProcessCounters> countersFor(LinuxProcessProbe& probe, pid_t pid)
{
    for (ProcessCounters& counters : probe.enumerate())
    {
        if (counters.pid == static_cast<std::int32_t>(pid))
        {
            return std::move(counters);
        }
    }
    return std::nullopt;
}

// #1648: a real process whose /proc/<pid>/comm isn't UTF-8 -- it renamed itself with prctl(PR_SET_NAME) --
// reaches the snapshot byte for byte, as the mocks in test_NonAsciiRoundTrips.cpp assume.
TEST(LinuxProcessProbeTest, ARealProcessNameThatIsntUtf8ArrivesByteForByte)
{
    std::array<int, 2> ready{};
    ASSERT_EQ(::pipe(ready.data()), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0)
    {
        // The child: async-signal-safe calls only (the test binary has other threads).
        ::prctl(PR_SET_NAME, NOT_UTF8_CSTR, 0, 0, 0); // NOLINT(cppcoreguidelines-pro-type-vararg) - prctl is variadic
        const char byte = 1;
        static_cast<void>(::write(ready[1], &byte, 1));
        while (true)
        {
            ::pause();
        }
    }
    const ChildProcess child(pid);
    ::close(ready[1]);
    char byte = 0;
    ASSERT_EQ(::read(ready[0], &byte, 1), 1); // renamed
    ::close(ready[0]);

    LinuxProcessProbe probe;
    const std::optional<ProcessCounters> counters = countersFor(probe, child.pid());
    ASSERT_TRUE(counters.has_value());
    EXPECT_EQ(counters.value_or(ProcessCounters{}).name, NOT_UTF8);
}

// #1648: a real process whose command line isn't UTF-8 (argv[0]) reaches the snapshot byte for byte.
TEST(LinuxProcessProbeTest, ARealCommandLineThatIsntUtf8ArrivesByteForByte)
{
    constexpr const char* SLEEP = "/bin/sleep";
    if (::access(SLEEP, X_OK) != 0)
    {
        GTEST_SKIP() << SLEEP << " isn't here";
    }
    std::string argv0(NOT_UTF8);
    std::string seconds = "30";
    std::array<char*, 3> argv{argv0.data(), seconds.data(), nullptr};
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0)
    {
        ::execv(SLEEP, argv.data());
        ::_exit(127);
    }
    const ChildProcess child(pid);

    // Wait for the exec: until then /proc/<pid>/cmdline is the test binary's, or empty mid-exec (the
    // probe then shows "[sleep]"). On a loaded machine that can take seconds (#2052), so wait on a
    // deadline and say so if the exec never shows, rather than compare against the pre-exec reading.
    constexpr auto EXEC_DEADLINE = std::chrono::seconds(15);
    LinuxProcessProbe probe;
    std::optional<ProcessCounters> counters;
    bool execSeen = false;
    const auto giveUpAt = std::chrono::steady_clock::now() + EXEC_DEADLINE;
    while (std::chrono::steady_clock::now() < giveUpAt)
    {
        counters = countersFor(probe, child.pid());
        if (counters.has_value() && counters.value_or(ProcessCounters{}).command.starts_with(NOT_UTF8))
        {
            execSeen = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(execSeen) << "the child's exec of " << SLEEP << " wasn't seen within " << EXEC_DEADLINE.count() << " s";
    const ProcessCounters found = counters.value_or(ProcessCounters{});
    EXPECT_EQ(found.command, std::string(NOT_UTF8) + " 30");
    EXPECT_FALSE(found.name.empty()); // "sleep" from comm, or the command line's -- either way, not lost
}

TEST(LinuxProcessProbeTest, EnumerateFindsOurOwnProcess)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    pid_t ourPid = getpid();
    auto it = std::find_if(
        processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == static_cast<int32_t>(ourPid); });

    ASSERT_NE(it, processes.end()) << "Should find our own process (PID " << ourPid << ")";

    // Verify our process has reasonable data
    EXPECT_GT(it->name.size(), 0ULL);
    EXPECT_GT(it->rssBytes, 0ULL);
    EXPECT_GT(it->virtualBytes, 0ULL);
    EXPECT_GE(it->userTime, 0ULL);
    EXPECT_GE(it->systemTime, 0ULL);
    EXPECT_GT(it->startTimeTicks, 0ULL);
    EXPECT_GE(it->threadCount, 1); // At least one thread (main)
    // #3086: the scheduling policy is read from the same stat line; this test runs under the default
    // (or SCHED_BATCH / SCHED_IDLE where a CI harness lowers it), never as unknown.
    EXPECT_NE(it->schedPolicy, SchedPolicy::Unknown);
    EXPECT_TRUE(probe.capabilities().hasSchedPolicy);

    // Verify handle count (file descriptors) is populated for our own process
    // We can read our own /proc/[pid]/fd directory
    EXPECT_GT(it->handleCount, 0);

    // Verify start time epoch is populated and reasonable
    // Should not be before 2020-01-01 (guard against obviously invalid timestamps)
    constexpr std::uint64_t jan2020 = 1577836800; // 2020-01-01 00:00:00 UTC
    EXPECT_GT(it->startTimeEpoch, jan2020) << "Start time epoch should be a reasonable modern timestamp";
}

TEST(LinuxProcessProbeTest, EnumerateFindsInitProcess)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    // PID 1 should be init/systemd
    auto it = std::find_if(processes.begin(), processes.end(), [](const ProcessCounters& p) { return p.pid == 1; });

    ASSERT_NE(it, processes.end()) << "Should find init process (PID 1)";

    // Verify init has reasonable data
    EXPECT_GT(it->name.size(), 0ULL);
    EXPECT_EQ(it->parentPid, 0); // init has no parent
}

TEST(LinuxProcessProbeTest, ProcessNamesAreNonEmpty)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    for (const auto& proc : processes)
    {
        EXPECT_GT(proc.name.size(), 0ULL) << "Process " << proc.pid << " should have a name";
    }
}

TEST(LinuxProcessProbeTest, ProcessPidsArePositive)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    for (const auto& proc : processes)
    {
        EXPECT_GT(proc.pid, 0) << "Process PIDs should be positive";
    }
}

TEST(LinuxProcessProbeTest, ProcessParentPidsAreValid)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    for (const auto& proc : processes)
    {
        // Parent PID should be non-negative (0 for init, positive for others)
        EXPECT_GE(proc.parentPid, 0) << "Process " << proc.pid << " has invalid parent PID";
    }
}

TEST(LinuxProcessProbeTest, MemoryValuesAreReasonable)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    for (const auto& proc : processes)
    {
        // RSS should be <= virtual memory
        // Note: Some processes may have very small or zero RSS/virtual (kernel threads)
        if (proc.rssBytes > 0 && proc.virtualBytes > 0)
        {
            EXPECT_LE(proc.rssBytes, proc.virtualBytes) << "Process " << proc.pid << " RSS should be <= virtual memory";
        }

        // Virtual memory can be very large for some processes (Java, etc.)
        // that reserve large address spaces, so we don't enforce an upper limit
    }
}

TEST(LinuxProcessProbeTest, StartTimeTicksAreNonZero)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    for (const auto& proc : processes)
    {
        EXPECT_GT(proc.startTimeTicks, 0ULL) << "Process " << proc.pid << " should have non-zero start time";
    }
}

TEST(LinuxProcessProbeTest, ThreadCountsArePositive)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    for (const auto& proc : processes)
    {
        EXPECT_GE(proc.threadCount, 1) << "Process " << proc.pid << " should have at least 1 thread";
    }
}

TEST(LinuxProcessProbeTest, StateIsValid)
{
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    // Valid Linux process states: R, S, D, Z, T, t, W, X, x, K, P, I
    // 'I' is Idle kernel thread (since Linux 4.14)
    const std::string validStates = "RSDZTtWXxKPI?";

    for (const auto& proc : processes)
    {
        // State is a char, not a string
        char state = proc.state;
        EXPECT_NE(validStates.find(state), std::string::npos) << "Process " << proc.pid << " has invalid state: " << state;
    }
}

// =============================================================================
// Consistency Tests
// =============================================================================

TEST(LinuxProcessProbeTest, MultipleEnumerationsAreConsistent)
{
    LinuxProcessProbe probe;

    auto processes1 = probe.enumerate();
    auto processes2 = probe.enumerate();

    // Process counts might differ slightly due to short-lived processes,
    // but should be in the same ballpark
    EXPECT_NEAR(
        static_cast<double>(processes1.size()), static_cast<double>(processes2.size()), static_cast<double>(processes1.size()) * 0.2)
        << "Multiple enumerations should return similar process counts";
}

TEST(LinuxProcessProbeTest, OwnProcessDataIsStable)
{
    LinuxProcessProbe probe;
    pid_t ourPid = getpid();

    auto findOurProcess = [ourPid](const std::vector<ProcessCounters>& processes)
    {
        auto it = std::find_if(
            processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == static_cast<int32_t>(ourPid); });
        return it != processes.end() ? *it : ProcessCounters{};
    };

    auto proc1 = findOurProcess(probe.enumerate());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto proc2 = findOurProcess(probe.enumerate());

    // PID should be the same
    EXPECT_EQ(proc1.pid, proc2.pid);

    // Name should be stable
    EXPECT_EQ(proc1.name, proc2.name);

    // Start time should be stable
    EXPECT_EQ(proc1.startTimeTicks, proc2.startTimeTicks);

    // Parent PID should be stable
    EXPECT_EQ(proc1.parentPid, proc2.parentPid);
}

TEST(LinuxProcessProbeTest, CpuTimeIncreasesBetweenSamples)
{
    LinuxProcessProbe probe;
    pid_t ourPid = getpid();

    auto findOurProcess = [ourPid](const std::vector<ProcessCounters>& processes)
    {
        auto it = std::find_if(
            processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == static_cast<int32_t>(ourPid); });
        return it != processes.end() ? *it : ProcessCounters{};
    };

    auto proc1 = findOurProcess(probe.enumerate());

    // Do significant CPU work to ensure measurable time increase
    // Use multiple iterations and sleep to ensure CPU time is captured
    volatile std::uint64_t sum = 0; // unsigned, so the busy loop's wrap-around is defined (#1090)
    for (int iteration = 0; iteration < 5; ++iteration)
    {
        for (std::uint64_t i = 0; i < 10000000; ++i)
        {
            sum += i;
        }
    }

    auto proc2 = findOurProcess(probe.enumerate());

    // CPU time should have increased (allow for rounding/measurement variance)
    uint64_t totalTime1 = proc1.userTime + proc1.systemTime;
    uint64_t totalTime2 = proc2.userTime + proc2.systemTime;
    EXPECT_GE(totalTime2, totalTime1) << "CPU time should not decrease after doing work";
}

// =============================================================================
// Edge Cases and Error Handling
// =============================================================================

TEST(LinuxProcessProbeTest, HandlesMissingProcesses)
{
    // Processes may disappear between directory listing and reading stats
    // The probe should handle this gracefully by skipping missing processes
    LinuxProcessProbe probe;

    // Just verify enumeration doesn't crash
    EXPECT_NO_THROW({
        for (int i = 0; i < 10; ++i)
        {
            auto processes = probe.enumerate();
            (void) processes; // Suppress unused variable warning
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

TEST(LinuxProcessProbeTest, HandlesRapidEnumeration)
{
    LinuxProcessProbe probe;

    // Rapidly enumerate many times - should not crash or leak
    EXPECT_NO_THROW({
        for (int i = 0; i < 100; ++i)
        {
            auto processes = probe.enumerate();
            EXPECT_GT(processes.size(), 0ULL);
        }
    });
}

// =============================================================================
// Multithreading Tests
// =============================================================================

namespace
{
/// Four threads enumerating through @p probe at once (its documented contract, LinuxProcessProbe.h): none
/// may throw, crash or come back empty, and every one must finish its work. Each does a fixed number
/// of enumerations, checked after joining, so a slow or oversubscribed runner only makes the test take
/// longer; it has no sleep to outlast (#2283, as the Windows twin's #2259). A real hang is caught by
/// the suite's ctest TIMEOUT. Under the tsan preset, a race on the probe's shared caches fails it (#2412).
void expectConcurrentEnumerationWorks(LinuxProcessProbe& probe)
{
    constexpr std::size_t THREAD_COUNT = 4;
    constexpr int ITERATIONS_PER_THREAD = 2;

    std::array<int, THREAD_COUNT> successes{};
    std::array<int, THREAD_COUNT> exceptions{};

    // The threads all start enumerating together, so the enumerations overlap rather than running one
    // after another as the threads happen to start.
    std::latch startTogether{static_cast<std::ptrdiff_t>(THREAD_COUNT)};

    {
        std::vector<std::jthread> threads;
        threads.reserve(THREAD_COUNT);
        for (std::size_t i = 0; i < THREAD_COUNT; ++i)
        {
            // Each thread writes only its own slot; join() publishes the writes to this thread.
            threads.emplace_back(
                [&probe, &startTogether, &success = successes.at(i), &thrown = exceptions.at(i)]()
                {
                    startTogether.arrive_and_wait();
                    for (int iteration = 0; iteration < ITERATIONS_PER_THREAD; ++iteration)
                    {
                        try
                        {
                            if (!probe.enumerate().empty())
                            {
                                ++success;
                            }
                        }
                        catch (...)
                        {
                            ++thrown;
                        }
                    }
                });
        }
    } // jthreads join here

    for (std::size_t i = 0; i < THREAD_COUNT; ++i)
    {
        EXPECT_EQ(exceptions.at(i), 0) << "thread " << i << ": enumerate() threw";
        EXPECT_EQ(successes.at(i), ITERATIONS_PER_THREAD) << "thread " << i << ": not every enumeration returned processes";
    }
}
} // namespace

// #2412: one probe shared by every thread, as the sampler and Process Details share it. Separate probes
// share no state, so they would test no concurrency at all.
TEST(LinuxProcessProbeTest, ConcurrentEnumeration)
{
    LinuxProcessProbe probe;
    expectConcurrentEnumerationWorks(probe);
}

// #2412: as above, with the inode-to-PID map and the command-line cache expiring at once, so every
// enumeration rebuilds them and the threads race on the claim-under-lock rebuild, not just on reads.
TEST(LinuxProcessProbeTest, ConcurrentEnumerationRebuildingItsCaches)
{
    LinuxProcessProbe probe;
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    probe.setInodeMapTtlForTesting(std::chrono::milliseconds(0));
#endif
    probe.setCmdlineCacheTtlForTesting(std::chrono::milliseconds(0));
    expectConcurrentEnumerationWorks(probe);
}

// #2633: exit() runs static destructors while the sampler thread is still enumerating (Xlib calls it
// from the render loop when the X connection drops). The child exits under four enumerating threads;
// a destroyed username cache made the next getUsername() die of SIGSEGV at a null bucket array. The
// probe is leaked, as the app's survives exit(): only static storage is destroyed.
TEST(LinuxProcessProbeDeathTest, ExitWhileOtherThreadsEnumerate)
{
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) ||                                                                       \
    (defined(__has_feature) && (__has_feature(thread_sanitizer) || __has_feature(address_sanitizer)))
    // exit() also destroys spdlog's registry under the threads' log calls: a race TSan reports and a
    // heap-use-after-free ASan reports (#2657)
    GTEST_SKIP() << "exit() under running threads also frees spdlog's registry under them (#2657)";
#endif
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            constexpr int THREAD_COUNT = 4;
            auto* const probe = new LinuxProcessProbe(); // NOLINT(cppcoreguidelines-owning-memory) - outlives exit(), as the app's does
            std::latch enumerated{THREAD_COUNT};
            for (int i = 0; i < THREAD_COUNT; ++i)
            {
                std::thread(
                    [probe, &enumerated]()
                    {
                        static_cast<void>(probe->enumerate());
                        enumerated.count_down();
                        while (true)
                        {
                            static_cast<void>(probe->enumerate());
                        }
                    })
                    .detach();
            }
            enumerated.wait();
            std::exit(0); // NOLINT(concurrency-mt-unsafe) - exiting under the other threads is the test
        },
        ::testing::ExitedWithCode(0),
        "");
}

// =============================================================================
// I/O Counter Tests
// =============================================================================

TEST(LinuxProcessProbeTest, IoCountersCapabilityReported)
{
    LinuxProcessProbe probe;
    auto caps = probe.capabilities();

    // Determine whether the current process can actually read /proc/self/io
    bool canReadSelfIo = false;
    {
        std::ifstream ioFile("/proc/self/io");
        canReadSelfIo = ioFile.is_open();
    }

    // Capability flag should reflect whether /proc/self/io is readable
    EXPECT_EQ(caps.hasIoCounters, canReadSelfIo);
}

TEST(LinuxProcessProbeTest, IoCountersForSelfProcess)
{
    LinuxProcessProbe probe;
    auto caps = probe.capabilities();

    // Only test if I/O counters are available
    if (!caps.hasIoCounters)
    {
        GTEST_SKIP() << "/proc/self/io is unreadable (likely a procfs restriction, e.g. a sandbox or hidepid mount)";
    }

    auto processes = probe.enumerate();
    const pid_t selfPid = getpid();

    // Find our own process
    auto selfProc = std::find_if(processes.begin(), processes.end(), [selfPid](const ProcessCounters& p) { return p.pid == selfPid; });

    ASSERT_NE(selfProc, processes.end()) << "Could not find self process in enumeration";

    // I/O counters should be populated (at least non-negative)
    EXPECT_GE(selfProc->readBytes, 0ULL);
    EXPECT_GE(selfProc->writeBytes, 0ULL);
}

TEST(LinuxProcessProbeTest, ACachedSocketReadingKeepsItsReadTime)
{
    // #1063 review: ProcessModel takes network rates over the time between the probe's socket reads
    // (and stamps that time on every process, see SocketTrafficAccumulator::apply()). Within the
    // socket cache's TTL the same read must come back with its original time, not a new one: a fresh
    // time on a cache hit would make ProcessModel treat it as a new reading (rates of 0 between real
    // reads, then inflated ones when fresh counters arrive).
    LinuxProcessProbe probe;
    if (!probe.capabilities().hasNetworkCounters)
    {
        GTEST_SKIP() << "Per-process network counters not available (Netlink INET_DIAG)";
    }
    // Long enough that both reads below hit the same cached socket query.
    probe.setSocketStatsCacheTtl(std::chrono::minutes{10});

    const auto first = probe.readSocketTraffic();
    EXPECT_NE(first.sampleTimeNs, 0U);
    const auto second = probe.readSocketTraffic();
    EXPECT_EQ(second.sampleTimeNs, first.sampleTimeNs);
    EXPECT_EQ(second.sockets.size(), first.sockets.size());
}

TEST(LinuxProcessProbeTest, IoCountersIncreaseWithActivity)
{
    LinuxProcessProbe probe;
    auto caps = probe.capabilities();

    if (!caps.hasIoCounters)
    {
        GTEST_SKIP() << "/proc/self/io is unreadable (likely a procfs restriction, e.g. a sandbox or hidepid mount)";
    }

    const pid_t selfPid = getpid();

    // First measurement
    auto processes1 = probe.enumerate();
    auto selfProc1 = std::find_if(processes1.begin(), processes1.end(), [selfPid](const ProcessCounters& p) { return p.pid == selfPid; });
    ASSERT_NE(selfProc1, processes1.end());
    const uint64_t writeBytes1 = selfProc1->writeBytes;

    // Do some I/O activity (write to a temporary file)
    std::filesystem::path tempFilePath;
    {
        // Use system temp directory and create unique filename
        std::string filename = "tasksmack_io_test_" + std::to_string(selfPid) + ".tmp";
        tempFilePath = std::filesystem::temp_directory_path() / filename;
        std::ofstream tempFile(tempFilePath);
        tempFile << "Test data for I/O counter verification\n";
        tempFile.flush();
        // Use fsync to ensure data is flushed to disk
        if (tempFile.is_open())
        {
            tempFile.close();
            // Note: fsync requires file descriptor, so we rely on flush() and close()
        }
    }

    // Second measurement
    auto processes2 = probe.enumerate();
    auto selfProc2 = std::find_if(processes2.begin(), processes2.end(), [selfPid](const ProcessCounters& p) { return p.pid == selfPid; });
    ASSERT_NE(selfProc2, processes2.end());
    const uint64_t writeBytes2 = selfProc2->writeBytes;

    // Write bytes should have increased (we wrote to a file)
    EXPECT_GE(writeBytes2, writeBytes1) << "Write bytes should increase after file write";

    // Clean up
    std::filesystem::remove(tempFilePath);
}

// =============================================================================
// Process Status and Cmdline Tests (covers additional parsing branches)
// =============================================================================

TEST(LinuxProcessProbeTest, EnumerateHandlesKernelThreadsWithNullCmdline)
{
    // Verify the empty-cmdline fallback branch by finding a real process whose
    // /proc/[pid]/cmdline is empty and asserting the probe formats command as "[name]".
    //
    // Only kernel threads (kthreadd, PID 2, and its children) are considered. Any other process
    // with an empty cmdline at the moment of this check is one that exited after the probe read
    // it -- a zombie's cmdline reads as empty -- and its earlier, non-empty command makes the
    // assertion fail. Under a parallel ctest run, tests that fork short-lived children make that
    // race likely. Where kernel threads are not visible (WSL, some containers) the test skips.
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    const auto it = std::find_if(processes.begin(),
                                 processes.end(),
                                 [](const ProcessCounters& proc)
                                 {
                                     if (proc.pid != 2 && proc.parentPid != 2)
                                     {
                                         return false;
                                     }
                                     const auto cmdlinePath = std::filesystem::path("/proc") / std::to_string(proc.pid) / "cmdline";
                                     std::ifstream cmdlineFile(cmdlinePath, std::ios::binary);
                                     if (!cmdlineFile.is_open())
                                     {
                                         return false;
                                     }

                                     return (cmdlineFile.peek() == std::ifstream::traits_type::eof());
                                 });

    if (it == processes.end())
    {
        GTEST_SKIP() << "No process with an empty /proc/<pid>/cmdline was visible in this environment";
    }

    EXPECT_FALSE(it->name.empty()) << "Process " << it->pid << " should always have a name";
    EXPECT_EQ(it->command, ("[" + it->name + "]")) << "Processes with empty cmdline should use the [name] fallback";
}

TEST(LinuxProcessProbeTest, EnumerateHandlesZombieProcesses)
{
    // Create a real zombie: fork a child that exits immediately, delay waitpid() so the
    // probe can observe it in state 'Z', then reap it.
    const pid_t childPid = fork();
    ASSERT_NE(childPid, -1) << "fork() failed: " << strerror(errno);

    if (childPid == 0)
    {
        // Child: exit immediately without cleanup so the parent's fork() bookkeeping is intact.
        _exit(0);
    }

    // Give the kernel a moment to transition the child to zombie state before enumeration.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    // Find our zombie child in the enumerated list.
    const auto it = std::find_if(
        processes.begin(), processes.end(), [childPid](const ProcessCounters& p) { return p.pid == static_cast<std::int32_t>(childPid); });

    if (it != processes.end())
    {
        EXPECT_EQ(it->state, 'Z') << "Child process should be in zombie state";
    }
    // (The zombie may already have been reaped by the OS in rare CI edge cases; not finding
    // it is acceptable, but if found it must be 'Z'.)

    // Always reap to avoid leaking a zombie process.
    // Retry on EINTR; accept ECHILD if the environment auto-reaps (SA_NOCLDWAIT / SIGCHLD ignored).
    int status = 0;
    pid_t ret = 0;
    do
    {
        ret = waitpid(childPid, &status, 0);
    } while (ret == -1 && errno == EINTR);

    EXPECT_TRUE(ret == childPid || (ret == -1 && errno == ECHILD)) << "waitpid failed unexpectedly: " << strerror(errno);
}

TEST(LinuxProcessProbeTest, EnumerateOwnProcessHasNonEmptyName)
{
    // Verify that our own process always has a non-empty name returned by the probe.
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();
    const pid_t selfPid = getpid();

    auto it = std::find_if(processes.begin(), processes.end(), [selfPid](const ProcessCounters& p) { return p.pid == selfPid; });

    ASSERT_NE(it, processes.end()) << "Should find our own process";
    EXPECT_FALSE(it->name.empty()) << "Our process should have a non-empty name";
}

TEST(LinuxProcessProbeTest, EnumerateReturnsReasonableThreadCounts)
{
    // Thread count must be >= 1. Multi-threaded processes (like this test binary) should report > 1.
    LinuxProcessProbe probe;
    const pid_t selfPid = getpid();
    auto processes = probe.enumerate();

    auto it = std::find_if(processes.begin(), processes.end(), [selfPid](const ProcessCounters& p) { return p.pid == selfPid; });

    ASSERT_NE(it, processes.end()) << "Should find our own process";
    // This test binary uses multiple threads (gtest runs tests on the main thread but jthread
    // tests may have created background threads); the thread count must be at least 1.
    EXPECT_GE(it->threadCount, 1) << "Thread count must be >= 1";
}

TEST(LinuxProcessProbeTest, CapabilitiesHasThreadCount)
{
    // LinuxProcessProbe always reports thread counts.
    LinuxProcessProbe probe;
    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasThreadCount);
}

TEST(LinuxProcessProbeTest, UserFieldIsPopulatedForOwnProcess)
{
    LinuxProcessProbe probe;
    const pid_t selfPid = getpid();
    auto processes = probe.enumerate();

    auto it = std::find_if(processes.begin(), processes.end(), [selfPid](const ProcessCounters& p) { return p.pid == selfPid; });
    ASSERT_NE(it, processes.end()) << "Should find our own process";

    // The user field should be populated (at minimum as a UID string).
    EXPECT_FALSE(it->user.empty()) << "User field should not be empty for own process";
}

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
TEST(LinuxProcessProbeTest, SetSocketStatsCacheTtl_DoesNotCrash)
{
    LinuxProcessProbe probe;

    // Changing the TTL should not crash regardless of whether Netlink is available.
    EXPECT_NO_THROW(probe.setSocketStatsCacheTtl(std::chrono::milliseconds(500)));
    EXPECT_NO_THROW(probe.setSocketStatsCacheTtl(std::chrono::milliseconds(0)));
    EXPECT_NO_THROW(probe.setSocketStatsCacheTtl(std::chrono::milliseconds(10000)));
}

// Regression test for a data race where setSocketStatsCacheTtl() reassigned the
// NetlinkSocketStats instance with no synchronization while enumerate() (on another
// thread, as it would be from the background sampler) concurrently dereferenced it.
// Under TSan this reliably flagged a race before the mutex-guarded shared_ptr fix;
// without TSan it exercises the same interleaving without asserting on timing.
TEST(LinuxProcessProbeTest, SetSocketStatsCacheTtlDoesNotRaceWithEnumerate)
{
    LinuxProcessProbe probe;
    std::atomic<bool> stop{false};
    std::atomic<int> enumerateCount{0};

    std::thread enumerateThread(
        [&probe, &stop, &enumerateCount]
        {
            while (!stop.load(std::memory_order_relaxed))
            {
                [[maybe_unused]] const auto processes = probe.enumerate();
                enumerateCount.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    for (int i = 0; i < 50; ++i)
    {
        probe.setSocketStatsCacheTtl(std::chrono::milliseconds(1 + (i % 20)));
    }

    stop.store(true, std::memory_order_relaxed);
    enumerateThread.join();

    // Guards against the loop above racing past the enumerate thread's first scheduling
    // window and the test silently passing without exercising the interleaving it exists
    // to test.
    EXPECT_GT(enumerateCount.load(), 0) << "enumerate() thread should have run at least once";
}
#endif

// ========== Error Path / Injection Tests ==========

using Platform::TestSupport::ScopedTempDir;

// #1103: per-process power is offered only when the RAPL counter is readable, not merely present.
// energy_uj is root-only on current kernels; detection used to accept it by existence and then
// report 0 W for every process to a normal user.
void writeFile(const std::filesystem::path& path, std::string_view content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << content;
}

TEST(LinuxProcessProbeTest, PeakRssIsVmHwmFromStatus)
{
    // #1184: the peak is the kernel's high-water mark over the process's whole life, from the status
    // file enumerate() already reads -- not just the largest RSS TaskSmack happened to sample.
    ScopedTempDir proc("ts_test_proc_vmhwm");
    const auto writeProcess = [&proc](int pid, std::string_view status, std::string_view statm)
    {
        const auto dir = proc.path / std::to_string(pid);
        writeFile(dir / "stat",
                  std::to_string(pid) + " (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
                                        "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
        writeFile(dir / "status", status);
        writeFile(dir / "statm", statm);
    };
    const long pageSize = sysconf(_SC_PAGESIZE);
    ASSERT_GT(pageSize, 0);
    const auto page = static_cast<std::uint64_t>(pageSize);

    // An ordinary process: VmHWM 8192 kB, RSS 100 pages.
    writeProcess(
        4242, "Name:\tapp\nUid:\t0\t0\t0\t0\nVmPeak:\t  20000 kB\nVmHWM:\t    8192 kB\nVmRSS:\t    4096 kB\n", "500 100 10 1 0 50 0\n");
    // A kernel thread has no Vm* lines: unknown, left to Domain's own tracking.
    writeProcess(4343, "Name:\tkworker/0:1\nUid:\t0\t0\t0\t0\n", "0 0 0 0 0 0 0\n");
    // A peak a moment behind the RSS statm reported (batched per-CPU counters) is raised to it.
    writeProcess(4444, "Name:\tapp\nUid:\t0\t0\t0\t0\nVmHWM:\t       4 kB\n", "500 100 10 1 0 50 0\n");

    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    const auto peakOf = [&processes](std::int32_t pid) -> std::uint64_t
    {
        const auto it = std::ranges::find_if(processes, [pid](const ProcessCounters& p) { return p.pid == pid; });
        return it == processes.end() ? std::numeric_limits<std::uint64_t>::max() : it->peakRssBytes;
    };
    EXPECT_EQ(peakOf(4242), 8192U * 1024U);
    EXPECT_EQ(peakOf(4343), 0U);
    EXPECT_EQ(peakOf(4444), 100U * page);
}

TEST(LinuxProcessProbeTest, UserIsTheEffectiveUid)
{
    // #2401: a setuid-root program run by another user runs as root, as ps and top show it -- the second
    // field of Uid:, not the first (the real UID).
    ScopedTempDir proc("ts_test_proc_effective_uid");
    const auto writeProcess = [&proc](int pid, std::string_view status)
    {
        const auto dir = proc.path / std::to_string(pid);
        writeFile(dir / "stat",
                  std::to_string(pid) + " (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
                                        "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
        writeFile(dir / "status", status);
    };
    writeProcess(4242, "Name:\tpasswd\nUid:\t65534\t0\t0\t0\n");
    // A line cut short after the real UID still names that user.
    writeProcess(4343, "Name:\tapp\nUid:\t0\n");

    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    const auto userOf = [&processes](std::int32_t pid) -> std::string
    {
        const auto it = std::ranges::find_if(processes, [pid](const ProcessCounters& p) { return p.pid == pid; });
        return it == processes.end() ? std::string{"<missing>"} : it->user;
    };
    EXPECT_EQ(userOf(4242), "root");
    EXPECT_EQ(userOf(4343), "root");

    // #2475: who started it, when that isn't who it runs as; nobody otherwise.
    const auto realUserOf = [&processes](std::int32_t pid) -> std::string
    {
        const auto it = std::ranges::find_if(processes, [pid](const ProcessCounters& p) { return p.pid == pid; });
        return it == processes.end() ? std::string{"<missing>"} : it->realUser;
    };
    EXPECT_FALSE(realUserOf(4242).empty());
    EXPECT_NE(realUserOf(4242), "root");
    EXPECT_EQ(realUserOf(4343), "");
}

TEST(LinuxProcessProbeTest, PrivateAndSwapAreRssAnonAndVmSwapFromStatus)
{
    // #1885: the Private and Swap columns, from the status file enumerate() already reads.
    ScopedTempDir proc("ts_test_proc_private_swap");
    const auto writeProcess = [&proc](int pid, std::string_view status)
    {
        const auto dir = proc.path / std::to_string(pid);
        writeFile(dir / "stat",
                  std::to_string(pid) + " (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
                                        "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
        writeFile(dir / "status", status);
        writeFile(dir / "statm", "500 100 10 1 0 50 0\n");
    };
    // Lines in the kernel's order: RssAnon in the Rss* block after VmRSS, VmSwap after the page tables.
    writeProcess(4242,
                 "Name:\tapp\nUid:\t0\t0\t0\t0\nVmHWM:\t    8192 kB\nVmRSS:\t    4096 kB\nRssAnon:\t    3000 kB\n"
                 "RssFile:\t    1000 kB\nRssShmem:\t      96 kB\nVmPTE:\t      48 kB\nVmSwap:\t     512 kB\n"
                 "Cpus_allowed_list:\t0-3\n");
    // A kernel thread has no Vm* or Rss* lines: none of either.
    writeProcess(4343, "Name:\tkworker/0:1\nUid:\t0\t0\t0\t0\nCpus_allowed_list:\t0-3\n");
    // A file cut off inside VmSwap's line (no newline): its number may be cut too, so it is not used.
    writeProcess(4444, "Name:\tapp\nUid:\t0\t0\t0\t0\nRssAnon:\t      12 kB\nVmSwap:\t     12");

    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    const auto find = [&processes](std::int32_t pid) -> const ProcessCounters*
    {
        const auto it = std::ranges::find_if(processes, [pid](const ProcessCounters& p) { return p.pid == pid; });
        return it == processes.end() ? nullptr : &*it;
    };
    const ProcessCounters* app = find(4242);
    const ProcessCounters* kernel = find(4343);
    const ProcessCounters* cut = find(4444);
    ASSERT_NE(app, nullptr);
    ASSERT_NE(kernel, nullptr);
    ASSERT_NE(cut, nullptr);
    EXPECT_EQ(app->privateBytes, 3000U * 1024U);
    EXPECT_EQ(app->swapBytes, 512U * 1024U);
    EXPECT_EQ(kernel->privateBytes, 0U);
    EXPECT_EQ(kernel->swapBytes, 0U);
    EXPECT_EQ(cut->privateBytes, 12U * 1024U);
    EXPECT_EQ(cut->swapBytes, 0U);

    const ProcessCapabilities caps = probe.capabilities();
    EXPECT_TRUE(caps.hasPrivateBytes);
    EXPECT_TRUE(caps.hasSwapBytes);
}

TEST(LinuxProcessProbeTest, PssAndUssAreReadFromSmapsRollupForOneProcess)
{
    // #1885: smaps_rollup, read on demand for one process; its presence under self says the kernel has it.
    ScopedTempDir proc("ts_test_proc_smaps_rollup");
    const LinuxProcessProbe without(proc.path);
    EXPECT_FALSE(without.capabilities().hasMemoryRollup); // No self/smaps_rollup: a kernel before 4.14
    EXPECT_FALSE(without.readMemoryRollup(4242, 12345).has_value());

    writeFile(proc.path / "self" / "smaps_rollup", "Pss: 1 kB\nPrivate_Clean: 0 kB\nPrivate_Dirty: 1 kB\n");
    // Started at tick 12345 (stat field 22).
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    writeFile(proc.path / "4242" / "smaps_rollup",
              "00400000-7ffd0000 ---p 00000000 00:00 0  [rollup]\nRss: 4096 kB\nPss: 2500 kB\nPrivate_Clean: 100 kB\n"
              "Private_Dirty: 1900 kB\nSwapPss: 64 kB\n");
    const LinuxProcessProbe probe(proc.path);
    EXPECT_TRUE(probe.capabilities().hasMemoryRollup);
    const MemoryRollupRead rollup = probe.readMemoryRollup(4242, 12345);
    ASSERT_TRUE(rollup.has_value());
    EXPECT_EQ(rollup.value_or(MemoryRollup{}).pssBytes, 2500U * 1024U);
    EXPECT_EQ(rollup.value_or(MemoryRollup{}).ussBytes, 2000U * 1024U);
    EXPECT_EQ(rollup.value_or(MemoryRollup{}).swapPssBytes, 64U * 1024U);
    // The PID now names a process started at another time (reused): not that process's figures.
    EXPECT_FALSE(probe.readMemoryRollup(4242, 999).has_value());
    EXPECT_FALSE(probe.readMemoryRollup(4343, 12345).has_value()); // Gone, or never there
    EXPECT_FALSE(probe.readMemoryRollup(0, 0).has_value());
}

// #2417: an smaps_rollup that isn't a rollup (or was cut short) gives no figures rather than wrong ones.
TEST(LinuxProcessProbeTest, AnSmapsRollupThatDoesntParseIsUnavailable)
{
    ScopedTempDir proc("ts_test_proc_smaps_garbled");
    writeFile(proc.path / "self" / "smaps_rollup", "Pss: 1 kB\nPrivate_Clean: 0 kB\nPrivate_Dirty: 1 kB\n");
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    writeFile(proc.path / "4242" / "smaps_rollup", "Rss: 4096 kB\nPss: 2500 kB\n"); // no Private lines
    const LinuxProcessProbe probe(proc.path);
    const MemoryRollupRead garbled = probe.readMemoryRollup(4242, 12345);
    ASSERT_FALSE(garbled.has_value());
    EXPECT_EQ(garbled.error(), MemoryRollupFailure::Unavailable);
}

// #2309: an smaps_rollup the kernel won't open for this user (another user's process) is a refusal,
// told apart from one that is just absent.
TEST(LinuxProcessProbeTest, ARefusedSmapsRollupIsPermissionDenied)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root opens a file of mode 000 anyway";
    }
    ScopedTempDir proc("ts_test_proc_smaps_refused");
    writeFile(proc.path / "self" / "smaps_rollup", "Pss: 1 kB\nPrivate_Clean: 0 kB\nPrivate_Dirty: 1 kB\n");
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    writeFile(proc.path / "4242" / "smaps_rollup", "Pss: 2500 kB\nPrivate_Clean: 100 kB\nPrivate_Dirty: 1900 kB\n");
    std::filesystem::permissions(proc.path / "4242" / "smaps_rollup", std::filesystem::perms::none);
    const LinuxProcessProbe probe(proc.path);
    const MemoryRollupRead refused = probe.readMemoryRollup(4242, 12345);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), MemoryRollupFailure::PermissionDenied);
    const MemoryRollupRead gone = probe.readMemoryRollup(4343, 12345);
    ASSERT_FALSE(gone.has_value());
    EXPECT_EQ(gone.error(), MemoryRollupFailure::Unavailable);
}

TEST(LinuxProcessProbeTest, ThisProcessOwnPssAndUssReadFromTheRealProc)
{
    LinuxProcessProbe probe;
    if (!probe.capabilities().hasMemoryRollup)
    {
        GTEST_SKIP() << "This kernel has no smaps_rollup";
    }
    const auto self = static_cast<std::int32_t>(::getpid());
    const auto processes = probe.enumerate();
    const auto me = std::ranges::find_if(processes, [self](const ProcessCounters& p) { return p.pid == self; });
    ASSERT_NE(me, processes.end());
    const MemoryRollupRead rollup = probe.readMemoryRollup(self, me->startTimeTicks);
    ASSERT_TRUE(rollup.has_value());
    EXPECT_GT(rollup.value_or(MemoryRollup{}).pssBytes, 0U);
    EXPECT_GT(rollup.value_or(MemoryRollup{}).ussBytes, 0U);
    EXPECT_LE(rollup.value_or(MemoryRollup{}).ussBytes, rollup.value_or(MemoryRollup{}).pssBytes); // USS is within PSS
}

// #1891: a process whose cmdline reads empty is a kernel thread, shown as "[name]" -- the fixture's own,
// so this holds wherever the suite runs (the real-proc test above skips where none is visible).
TEST(LinuxProcessProbeTest, AnEmptyCmdlineIsAKernelThreadShownInBrackets)
{
    ScopedTempDir proc("ts_test_proc_kthread");
    const auto writeProcess = [&proc](int pid, std::string_view name, std::string_view cmdline)
    {
        const auto dir = proc.path / std::to_string(pid);
        writeFile(dir / "stat",
                  std::to_string(pid) + " (" + std::string(name) +
                      ") S 2 0 0 0 -1 69238880 0 0 0 0 0 1 0 0 20 0 1 0 50 0 0 "
                      "18446744073709551615 0 0 0 0 0 0 0 2147483647 0 0 0 0 17 0 0 0 0 0 0\n");
        writeFile(dir / "status", "Name:\t" + std::string(name) + "\nUid:\t0\t0\t0\t0\nCpus_allowed_list:\t0-3\n");
        writeFile(dir / "statm", "0 0 0 0 0 0 0\n");
        writeFile(dir / "cmdline", std::string(cmdline));
    };
    writeProcess(5151, "kworker/0:1", "");
    writeProcess(5252, "app", std::string("/usr/bin/app\0--flag\0", 20));
    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    const auto commandOf = [&processes](std::int32_t pid)
    {
        const auto it = std::ranges::find_if(processes, [pid](const ProcessCounters& p) { return p.pid == pid; });
        return it == processes.end() ? std::string("<missing>") : it->command;
    };
    EXPECT_EQ(commandOf(5151), "[kworker/0:1]");
    EXPECT_EQ(commandOf(5252), "/usr/bin/app --flag");
}

// #2728: a process hidepid hides from TaskSmack arrives as the helper's text of its /proc/<pid> files;
// processFromFiles() builds the record enumerate() would have built from them, field for field.
TEST(LinuxProcessProbeTest, ARecordFromTheFilesTextIsTheOneEnumerateBuilds)
{
    ScopedTempDir proc("ts_test_proc_from_files");
    const ProcFileTexts files{
        .stat = "6161 (worker) S 1 6161 6161 0 -1 4194304 7 0 2 0 120 30 0 0 20 5 3 0 4242 104857600 512 "
                "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
        .statm = "25600 600 200 0 0 0 0\n",
        .status = "Name:\tworker\nUid:\t0\t0\t0\t0\nVmHWM:\t   4096 kB\nRssAnon:\t   1024 kB\nVmSwap:\t      8 kB\n"
                  "Cpus_allowed_list:\t0\n",
        .cmdline = std::string("/usr/libexec/worker\0--serve\0", 28),
        .io = "rchar: 1\nwchar: 2\nsyscr: 3\nsyscw: 4\nread_bytes: 4096\nwrite_bytes: 8192\ncancelled_write_bytes: 0\n",
        .cgroup = "0::/system.slice/worker.service\n",
    };
    const auto dir = proc.path / "6161";
    writeFile(dir / "stat", files.stat);
    writeFile(dir / "statm", files.statm);
    writeFile(dir / "status", files.status);
    writeFile(dir / "cmdline", *files.cmdline);
    writeFile(dir / "io", files.io);
    writeFile(dir / "cgroup", files.cgroup);
    writeFile(proc.path / "self" / "io", files.io); // so enumerate() reads I/O at all
    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    const auto enumerated = std::ranges::find_if(processes, [](const ProcessCounters& p) { return p.pid == 6161; });
    ASSERT_NE(enumerated, processes.end());

    const std::optional<ProcessCounters> built = probe.processFromFiles(6161, files);
    ASSERT_TRUE(built.has_value());
    EXPECT_EQ(built->name, enumerated->name);
    EXPECT_EQ(built->command, "/usr/libexec/worker --serve");
    EXPECT_EQ(built->command, enumerated->command);
    EXPECT_EQ(built->user, enumerated->user);
    EXPECT_EQ(built->parentPid, enumerated->parentPid);
    EXPECT_EQ(built->state, enumerated->state);
    EXPECT_EQ(built->startTimeTicks, enumerated->startTimeTicks);
    EXPECT_EQ(built->startTimeEpoch, enumerated->startTimeEpoch);
    EXPECT_EQ(built->userTime, enumerated->userTime);
    EXPECT_EQ(built->systemTime, enumerated->systemTime);
    EXPECT_EQ(built->nice, enumerated->nice);
    EXPECT_EQ(built->threadCount, enumerated->threadCount);
    EXPECT_EQ(built->pageFaultCount, enumerated->pageFaultCount);
    EXPECT_EQ(built->virtualBytes, enumerated->virtualBytes);
    EXPECT_EQ(built->rssBytes, enumerated->rssBytes);
    EXPECT_EQ(built->sharedBytes, enumerated->sharedBytes);
    EXPECT_EQ(built->peakRssBytes, enumerated->peakRssBytes);
    EXPECT_EQ(built->privateBytes, enumerated->privateBytes);
    EXPECT_EQ(built->swapBytes, enumerated->swapBytes);
    EXPECT_EQ(built->cpuAffinity, enumerated->cpuAffinity);
    EXPECT_TRUE(built->ioCountersAvailable);
    EXPECT_EQ(built->readBytes, enumerated->readBytes);
    EXPECT_EQ(built->writeBytes, enumerated->writeBytes);
    EXPECT_EQ(built->services, enumerated->services);
    EXPECT_EQ(built->container, enumerated->container);
    EXPECT_EQ(built->status, enumerated->status);
    // What the files can't say: the helper's counters read fills the FD count and network counters in.
    EXPECT_FALSE(built->handleCountAvailable);
    EXPECT_FALSE(built->networkCountersAvailable);
}

TEST(LinuxProcessProbeTest, ARecordFromFilesNeedsItsStatAndTreatsAZombieOrAnUnreadCommandLineAsEnumerateDoes)
{
    const LinuxProcessProbe probe(std::filesystem::temp_directory_path());
    EXPECT_FALSE(probe.processFromFiles(6161, {}).has_value()) << "no stat, no record";
    EXPECT_FALSE(probe.processFromFiles(0, {.stat = "0 (x) S 1 0 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 1 0 0\n"}).has_value());

    const std::string zombieStat = "6262 (gone) Z 1 0 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 77 0 0\n";
    const auto zombie = probe.processFromFiles(6262, {.stat = zombieStat, .cmdline = std::string{}});
    ASSERT_TRUE(zombie.has_value());
    EXPECT_EQ(zombie->command, "gone <defunct>") << "not the kernel-thread label an empty cmdline would give";

    const std::string stat = "6363 (app) S 1 0 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 88 0 0\n";
    const auto unread = probe.processFromFiles(6363, {.stat = stat, .cmdline = std::nullopt});
    ASSERT_TRUE(unread.has_value());
    EXPECT_TRUE(unread->command.empty()) << "unread: not called a kernel thread";
    const auto kernelThread = probe.processFromFiles(6363, {.stat = stat, .cmdline = std::string{}});
    ASSERT_TRUE(kernelThread.has_value());
    EXPECT_EQ(kernelThread->command, "[app]");
    EXPECT_FALSE(kernelThread->ioCountersAvailable) << "no io text: its I/O is unknown, not 0";
}

void writeVmHwmProcess(const std::filesystem::path& procRoot, std::string_view status)
{
    const auto dir = procRoot / "4242";
    writeFile(dir / "stat",
              "4242 (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    writeFile(dir / "status", status);
}

[[nodiscard]] std::uint64_t onlyPeakRss(const std::filesystem::path& procRoot)
{
    LinuxProcessProbe probe(procRoot);
    const auto processes = probe.enumerate();
    EXPECT_EQ(processes.size(), 1U);
    return processes.empty() ? std::numeric_limits<std::uint64_t>::max() : processes[0].peakRssBytes;
}

// The probe's stack read of /proc/[pid]/status (LinuxProcessProbe::parseProcessStatus).
constexpr std::size_t STATUS_STACK_READ_SIZE = 8192;

TEST(LinuxProcessProbeTest, VmHwmPastTheStackBufferIsReadInFull)
{
    // The stack buffer (8 KiB) ends inside VmHWM's digits; the rest of the file is readable, so the
    // whole file is read and the complete peak is used -- not the truncated "123".
    ScopedTempDir proc("ts_test_proc_vmhwm_cut");
    const std::string head = "Name:\tapp\nUid:\t0\t0\t0\t0\nGroups:\t";
    const std::string vmHwm = "\nVmHWM:\t 123";
    writeVmHwmProcess(proc.path, head + std::string(STATUS_STACK_READ_SIZE - head.size() - vmHwm.size(), '1') + vmHwm + "456789 kB\n");
    EXPECT_EQ(onlyPeakRss(proc.path), 123456789ULL * 1024ULL);
}

TEST(LinuxProcessProbeTest, VmHwmAndCpusAllowedListPastTheStackBufferComeFromOneFullRead)
{
    // Either line cut off by the stack read sends the probe to the full file, and both come from it.
    // cutVmHwm: the buffer ends inside VmHWM's digits, with a Cpus_allowed_list wholly after it.
    // cutAfterList: a (here out-of-order) Cpus_allowed_list fits whole, VmHWM is cut -- the re-read
    // happens for VmHWM alone.
    const std::string head = "Name:\tapp\nUid:\t0\t0\t0\t0\nGroups:\t";
    const std::string vmHwm = "\nVmHWM:\t 123";

    ScopedTempDir cutVmHwm("ts_test_proc_status_both_cut");
    writeVmHwmProcess(cutVmHwm.path,
                      head + std::string(STATUS_STACK_READ_SIZE - head.size() - vmHwm.size(), '1') + vmHwm +
                          "456789 kB\nCpus_allowed_list:\t0-3,64-127\nMems_allowed_list:\t0\n");

    const std::string listFirst = "Name:\tapp\nUid:\t0\t0\t0\t0\nCpus_allowed_list:\t0-3,64-127\nGroups:\t";
    ScopedTempDir cutAfterList("ts_test_proc_status_list_first");
    writeVmHwmProcess(cutAfterList.path,
                      listFirst + std::string(STATUS_STACK_READ_SIZE - listFirst.size() - vmHwm.size(), '1') + vmHwm + "456789 kB\n");

    const auto expectedAffinity = CpuAffinity::fromCpuList("0-3,64-127").value_or(CpuAffinity{});
    for (const auto* root : {&cutVmHwm.path, &cutAfterList.path})
    {
        // No CPU sysfs online list: the raw Cpus_allowed_list is used (#1384).
        const auto absent = *root / "absent";
        LinuxProcessProbe probe(*root, absent, absent, absent);
        const auto processes = probe.enumerate();
        ASSERT_EQ(processes.size(), 1U) << *root;
        EXPECT_EQ(processes[0].peakRssBytes, 123456789ULL * 1024ULL) << *root;
        EXPECT_EQ(processes[0].cpuAffinity, expectedAffinity) << *root;
    }
}

TEST(LinuxProcessProbeTest, VmHwmAfterALongGroupsListIsFound)
{
    // A user in many supplementary groups: Groups: alone is past the 8 KiB stack read, and VmHWM follows it.
    ScopedTempDir proc("ts_test_proc_vmhwm_groups");
    std::string status = "Name:\tapp\nUid:\t1000\t1000\t1000\t1000\nGroups:\t";
    for (int gid = 100000; gid < 101500; ++gid)
    {
        status += std::to_string(gid) + ' ';
    }
    ASSERT_GT(status.size(), 8192U);
    status += "\nVmPeak:\t  20000 kB\nVmHWM:\t    8192 kB\nVmRSS:\t    4096 kB\n";
    writeVmHwmProcess(proc.path, status);
    EXPECT_EQ(onlyPeakRss(proc.path), 8192U * 1024U);
}

TEST(LinuxProcessProbeTest, IncompleteVmHwmLineIsIgnored)
{
    // A VmHWM line with no newline (the file ends mid-line) could carry a truncated number: it is not
    // used, whether the file is short or longer than the stack buffer.
    ScopedTempDir shortProc("ts_test_proc_vmhwm_eof_short");
    writeVmHwmProcess(shortProc.path, "Name:\tapp\nUid:\t0\t0\t0\t0\nVmHWM:\t 123");
    EXPECT_EQ(onlyPeakRss(shortProc.path), 0U);

    ScopedTempDir longProc("ts_test_proc_vmhwm_eof_long");
    const std::string head = "Name:\tapp\nUid:\t0\t0\t0\t0\nGroups:\t";
    writeVmHwmProcess(longProc.path, head + std::string(STATUS_STACK_READ_SIZE + 2000, '1') + "\nVmHWM:\t 123");
    EXPECT_EQ(onlyPeakRss(longProc.path), 0U);
}

TEST(LinuxProcessProbeTest, ReadableRaplCounterEnablesPowerUsage)
{
    ScopedTempDir proc("ts_test_proc_rapl_ok");
    ScopedTempDir powercap("ts_test_powercap_ok");
    writeFile(powercap.path / "intel-rapl:0" / "energy_uj", "123456\n");
    writeFile(powercap.path / "intel-rapl:0" / "max_energy_range_uj", "262143328850\n");

    // user nice system idle iowait irq softirq steal: busy is user + nice + system.
    writeFile(proc.path / "stat", "cpu  100 20 30 400 50 6 7 8 0 0\ncpu0 100 20 30 400 50 6 7 8 0 0\n");

    const LinuxProcessProbe probe(proc.path, powercap.path);
    EXPECT_TRUE(probe.capabilities().hasPowerUsage);

    // The raw reading ProcessModel shares out per interval (#1093): a regression dropping the
    // wrap range or miscounting busy ticks would otherwise pass the model tests, which inject them.
    const auto reading = probe.readPackageEnergy();
    ASSERT_TRUE(reading.has_value());
    EXPECT_EQ(reading->energyUj, 123456U);
    EXPECT_EQ(reading->maxRangeUj, 262143328850U);
    ASSERT_TRUE(reading->busyCpuTicks.has_value());
    EXPECT_EQ(*reading->busyCpuTicks, 150U);
}

// #2444: a multi-socket machine's other packages are read with the first, since the busy ticks are every
// package's CPUs'; psys (the whole platform) and a package's subzones are not, as they'd count twice.
TEST(LinuxProcessProbeTest, EveryCpuPackagesCounterIsRead)
{
    ScopedTempDir proc("ts_test_proc_rapl_sockets");
    ScopedTempDir powercap("ts_test_powercap_sockets");
    const std::filesystem::path zones = powercap.path / "intel-rapl";
    writeFile(zones / "intel-rapl:0" / "name", "package-0\n");
    writeFile(zones / "intel-rapl:0" / "energy_uj", "1000\n");
    writeFile(zones / "intel-rapl:0" / "max_energy_range_uj", "262143328850\n");
    writeFile(zones / "intel-rapl:0" / "intel-rapl:0:0" / "name", "core\n");
    writeFile(zones / "intel-rapl:0" / "intel-rapl:0:0" / "energy_uj", "500\n");
    writeFile(zones / "intel-rapl:2" / "name", "package-2\n"); // listed in zone order, whatever the directory's
    writeFile(zones / "intel-rapl:2" / "energy_uj", "3000\n");
    writeFile(zones / "intel-rapl:1" / "name", "package-1\n");
    writeFile(zones / "intel-rapl:1" / "energy_uj", "2000\n");
    writeFile(zones / "intel-rapl:1" / "max_energy_range_uj", "65532610987\n");
    writeFile(zones / "intel-rapl:3" / "name", "psys\n");
    writeFile(zones / "intel-rapl:3" / "energy_uj", "9000\n");
    writeFile(zones / "intel-rapl:0:1" / "name", "uncore\n");
    writeFile(zones / "intel-rapl:0:1" / "energy_uj", "700\n");
    writeFile(proc.path / "stat", "cpu  100 20 30 400 50 6 7 8 0 0\n");

    const LinuxProcessProbe probe(proc.path, powercap.path);
    ASSERT_TRUE(probe.capabilities().hasPowerUsage);
    const PackageEnergyReading reading = probe.readPackageEnergy().value_or(PackageEnergyReading{});
    EXPECT_EQ(reading.energyUj, 1000U);
    ASSERT_EQ(reading.otherPackages.size(), 2U);
    EXPECT_EQ(reading.otherPackages[0].energyUj, 2000U);
    EXPECT_EQ(reading.otherPackages[0].maxRangeUj, 65532610987U);
    EXPECT_EQ(reading.otherPackages[1].energyUj, 3000U);
    EXPECT_EQ(reading.otherPackages[1].maxRangeUj, 0U);

    // One package unread: the sample is unread, not one package's energy shared out over all of them.
    writeFile(zones / "intel-rapl:2" / "energy_uj", "");
    const PackageEnergyReading partial = probe.readPackageEnergy().value_or(PackageEnergyReading{});
    EXPECT_FALSE(partial.energyUj.has_value());
    EXPECT_TRUE(partial.otherPackages.empty());
}

TEST(LinuxProcessProbeTest, UnreadableRaplCounterDisablesPowerUsage)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root can read a mode-000 file";
    }
    ScopedTempDir proc("ts_test_proc_rapl_denied");
    ScopedTempDir powercap("ts_test_powercap_denied");
    const auto energyFile = powercap.path / "intel-rapl:0" / "energy_uj";
    writeFile(energyFile, "123456\n");
    std::filesystem::permissions(energyFile, std::filesystem::perms::none);

    const LinuxProcessProbe probe(proc.path, powercap.path);
    EXPECT_FALSE(probe.capabilities().hasPowerUsage);
}

// #1395: what the system-wide reads give when their files are not what they should be.

TEST(LinuxProcessProbeTest, AMalformedProcStatGivesNoCpuTime)
{
    ScopedTempDir notCpu("ts_test_proc_stat_not_cpu");
    writeFile(notCpu.path / "stat", "intr 12345\nbtime 1700000000\n"); // no "cpu " line first
    EXPECT_EQ(LinuxProcessProbe(notCpu.path).totalCpuTime(), 0U);

    ScopedTempDir tooShort("ts_test_proc_stat_short");
    writeFile(tooShort.path / "stat", "cpu  100 20 30\n"); // fewer than the eight fields read
    EXPECT_EQ(LinuxProcessProbe(tooShort.path).totalCpuTime(), 0U);
}

TEST(LinuxProcessProbeTest, AGarbledBootTimeLeavesStartTimesUnknown)
{
    ScopedTempDir proc("ts_test_proc_btime_garbled");
    writeFile(proc.path / "stat", "cpu  100 20 30 400 50 6 7 8 0 0\nbtime soon\n");
    writeVmHwmProcess(proc.path, "Name:\tapp\nUid:\t0\t0\t0\t0\n");
    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes[0].startTimeEpoch, 0U);
}

TEST(LinuxProcessProbeTest, MeminfoWithoutMemTotalIsNoTotal)
{
    ScopedTempDir proc("ts_test_proc_meminfo_no_total");
    writeFile(proc.path / "meminfo", "MemFree:        1024 kB\nMemAvailable:   2048 kB\nMemTotal:       lots\n");
    EXPECT_EQ(LinuxProcessProbe(proc.path).systemTotalMemory(), 0U);
}

TEST(LinuxProcessProbeTest, AnUnreadableRaplRangeIsZeroAndAnUnreadableCounterIsNoReading)
{
    ScopedTempDir proc("ts_test_proc_rapl_garbled");
    ScopedTempDir powercap("ts_test_powercap_garbled");
    const auto energyFile = powercap.path / "intel-rapl:0" / "energy_uj";
    writeFile(energyFile, "123456\n");
    writeFile(powercap.path / "intel-rapl:0" / "max_energy_range_uj", "unknown\n");
    writeFile(proc.path / "stat", "cpu  100 20 30 400 50 6 7 8 0 0\n");

    const LinuxProcessProbe probe(proc.path, powercap.path);
    ASSERT_TRUE(probe.capabilities().hasPowerUsage);
    const PackageEnergyReading reading = probe.readPackageEnergy().value_or(PackageEnergyReading{});
    EXPECT_EQ(reading.energyUj, 123456U);
    EXPECT_EQ(reading.maxRangeUj, 0U); // no wrap range: none is assumed

    writeFile(energyFile, "");
    EXPECT_FALSE(probe.readPackageEnergy().value_or(PackageEnergyReading{}).energyUj.has_value());
    writeFile(energyFile, "n/a\n");
    EXPECT_FALSE(probe.readPackageEnergy().value_or(PackageEnergyReading{}).energyUj.has_value());
}

TEST(LinuxProcessProbeTest, ACgroupFileLongerThanTheStackReadIsReadWhole)
{
    // Many v1 hierarchies with long paths put the v2 line naming the container past the 1 KiB read.
    ScopedTempDir proc("ts_test_proc_long_cgroup");
    writeVmHwmProcess(proc.path, "Name:\tapp\nUid:\t0\t0\t0\t0\n");
    const std::string id = "1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e0f1a2b";
    std::string cgroup;
    for (int hierarchy = 12; hierarchy > 0; --hierarchy)
    {
        cgroup += std::to_string(hierarchy) + ":controller" + std::to_string(hierarchy) + ":/" + std::string(100, 'x') + "\n";
    }
    ASSERT_GT(cgroup.size(), 1024U);
    cgroup += "0::/system.slice/docker-" + id + ".scope\n";
    writeFile(proc.path / "4242" / "cgroup", cgroup);

    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes[0].container, "docker:1a2b3c4d5e6f");
}

TEST(LinuxProcessProbeTest, ServicesAreTheSystemdServiceFromTheCgroup)
{
    // #1886: from the same /proc/[pid]/cgroup read as the container.
    ScopedTempDir proc("ts_test_proc_service");
    writeVmHwmProcess(proc.path, "Name:\tsshd\nUid:\t0\t0\t0\t0\n");
    writeFile(proc.path / "4242" / "cgroup", "0::/system.slice/ssh.service\n");
    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes[0].services, "ssh.service");
    EXPECT_TRUE(probe.capabilities().hasServices);
}

/// The first bytes of an ELF header (#2394): the magic, @p elfClass (1: 32-bit, 2: 64-bit), little-endian,
/// and e_machine @p machine (an EM_* value). The probe reads no more than these.
[[nodiscard]] std::string elfHeader(std::uint16_t machine, char elfClass = 2)
{
    std::string header(Platform::ElfArchitecture::ELF_HEADER_PREFIX_BYTES, '\0');
    header[0] = '\x7F';
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = elfClass;
    header[5] = 1; // ELFDATA2LSB
    header[18] = static_cast<char>(machine & 0xFFU);
    header[19] = static_cast<char>(machine >> 8U);
    return header;
}

TEST(LinuxProcessProbeTest, TheArchitectureIsTheExecutablesElfMachineReadOncePerProcess)
{
    ScopedTempDir proc("ts_test_proc_architecture");
    writeVmHwmProcess(proc.path, "Name:\tapp\nUid:\t0\t0\t0\t0\n");
    // A plain file, not the kernel's link: read for its header, with no path to name an emulator.
    writeFile(proc.path / "4242" / "exe", elfHeader(62)); // EM_X86_64
    LinuxProcessProbe probe(proc.path);
    probe.setHostMachineForTesting(ProcessArchitecture::Machine::Arm64);
    EXPECT_TRUE(probe.capabilities().hasArchitecture);
    const auto architecture = [&probe]
    {
        const auto processes = probe.enumerate();
        EXPECT_EQ(processes.size(), 1U);
        return processes.empty() ? std::string("<missing>") : processes[0].architecture;
    };
    EXPECT_EQ(architecture(), "x64 (emulated)");

    // The same process (start time and comm): not read again.
    writeFile(proc.path / "4242" / "exe", elfHeader(183)); // EM_AARCH64
    EXPECT_EQ(architecture(), "x64 (emulated)");

    // A new process with the PID (another start time) is read.
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 99999 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    EXPECT_EQ(architecture(), "ARM64");
}

TEST(LinuxProcessProbeTest, AThirtyTwoBitOrUnreadableExecutable)
{
    const auto architectureOf = [](std::string_view name, const std::optional<std::string>& exe)
    {
        ScopedTempDir proc(std::string("ts_test_proc_arch_") + std::string(name));
        writeVmHwmProcess(proc.path, "Name:\tapp\nUid:\t0\t0\t0\t0\n");
        if (exe.has_value())
        {
            writeFile(proc.path / "4242" / "exe", *exe);
        }
        LinuxProcessProbe probe(proc.path);
        probe.setHostMachineForTesting(ProcessArchitecture::Machine::X64);
        const auto processes = probe.enumerate();
        return processes.empty() ? std::string("<missing>") : processes[0].architecture;
    };
    EXPECT_EQ(architectureOf("x86", elfHeader(3, 1)), "x86 (32-bit)"); // EM_386, ELFCLASS32
    EXPECT_EQ(architectureOf("x64", elfHeader(62)), "x64");
    EXPECT_TRUE(architectureOf("none", std::nullopt).empty());                 // A kernel thread has no exe
    EXPECT_TRUE(architectureOf("script", std::string("#!/bin/sh\n")).empty()); // Not ELF
    EXPECT_TRUE(architectureOf("short",
                               std::string("\x7F"
                                           "ELF"))
                    .empty()); // Cut short
}

TEST(LinuxProcessProbeTest, OurProcessRunsAsTheArchitectureItWasBuiltFor)
{
    LinuxProcessProbe probe;
    const auto processes = probe.enumerate();
    const auto ours = std::ranges::find(processes, static_cast<std::int32_t>(::getpid()), &ProcessCounters::pid);
    ASSERT_NE(ours, processes.end());
#if defined(__x86_64__)
    EXPECT_EQ(ours->architecture, "x64");
#elif defined(__aarch64__)
    EXPECT_EQ(ours->architecture, "ARM64");
#endif
    // What this machine's processes show, for the log and the XML report.
    std::map<std::string, int> counts;
    for (const ProcessCounters& process : processes)
    {
        ++counts[process.architecture.empty() ? std::string("(unread)") : process.architecture];
    }
    std::string summary;
    for (const auto& [architecture, count] : counts)
    {
        summary += (summary.empty() ? "" : ", ") + architecture + ": " + std::to_string(count);
    }
    std::cout << "This process: " << ours->architecture << "\nEvery process: " << summary << '\n';
    ::testing::Test::RecordProperty("own_architecture", ours->architecture);
    ::testing::Test::RecordProperty("architectures", summary);
}
TEST(LinuxProcessProbeTest, ReducedPrivilegesReadsCapEffUnderTheProcRoot)
{
    // A readable CapEff decides for root too, so these hold whatever the test runs as.
    // CAP_DAC_READ_SEARCH (bit 2), CAP_KILL (5), CAP_SYS_PTRACE (19) and CAP_SYS_NICE (23): nothing missing.
    ScopedTempDir withCaps("ts_test_proc_capeff_full");
    writeFile(withCaps.path / "self" / "status", "Name:\tTaskSmack\nCapEff:\t0000000000880024\n");
    EXPECT_FALSE(LinuxProcessProbe(withCaps.path).capabilities().hasReducedPrivileges);
    EXPECT_EQ(LinuxProcessProbe(withCaps.path).capabilities().missingPrivileges, 0);

    // Read access alone (the FAQ's old setcap line) still can't act on other users' processes (#2303).
    ScopedTempDir readOnly("ts_test_proc_capeff_read");
    writeFile(readOnly.path / "self" / "status", "Name:\tTaskSmack\nCapEff:\t0000000000080004\n");
    EXPECT_TRUE(LinuxProcessProbe(readOnly.path).capabilities().hasReducedPrivileges);
    EXPECT_EQ(LinuxProcessProbe(readOnly.path).capabilities().missingPrivileges,
              MissingPrivilege::SIGNAL_OTHERS | MissingPrivilege::RAISE_PRIORITY);

    // hidepid=invisible under the proc root, outside its group: hidden too.
    ScopedTempDir hidden("ts_test_proc_capeff_hidepid");
    writeFile(hidden.path / "self" / "status", "Name:\tTaskSmack\nCapEff:\t0000000000800024\n"); // no CAP_SYS_PTRACE
    writeFile(hidden.path / "self" / "mountinfo", "22 28 0:21 / /proc rw - proc proc rw,hidepid=invisible,gid=4294967290\n");
    EXPECT_NE(LinuxProcessProbe(hidden.path).capabilities().missingPrivileges & MissingPrivilege::PROCESSES_HIDDEN, 0);

    ScopedTempDir dacOnly("ts_test_proc_capeff_dac");
    writeFile(dacOnly.path / "self" / "status", "Name:\tTaskSmack\nCapEff:\t0000000000000004\n");
    EXPECT_TRUE(LinuxProcessProbe(dacOnly.path).capabilities().hasReducedPrivileges);

    ScopedTempDir dropped("ts_test_proc_capeff_dropped");
    writeFile(dropped.path / "self" / "status", "Name:\tTaskSmack\nCapEff:\t0000000000000000\n");
    EXPECT_TRUE(LinuxProcessProbe(dropped.path).capabilities().hasReducedPrivileges);

    // Only an unreadable status falls back to the EUID.
    ScopedTempDir noStatus("ts_test_proc_capeff_none");
    EXPECT_EQ(LinuxProcessProbe(noStatus.path).capabilities().hasReducedPrivileges, ::geteuid() != 0);
}

TEST(LinuxProcessProbeTest, NoRaplCounterDisablesPowerUsage)
{
    ScopedTempDir proc("ts_test_proc_rapl_none");
    ScopedTempDir powercap("ts_test_powercap_none");

    const LinuxProcessProbe probe(proc.path, powercap.path);
    EXPECT_FALSE(probe.capabilities().hasPowerUsage);
}

TEST(LinuxProcessProbeTest, TotalCpuTimeIsTheOneTakenAfterTheStatPass)
{
    // #1119: the total that per-process CPU deltas are divided by is read right after enumerate()'s
    // per-process stat reads -- not later, after network attribution's variable-latency work -- and
    // handed to the totalCpuTime() call that follows. Any other call reads it fresh.
    ScopedTempDir proc("ts_test_proc_total_cpu");
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 4242 4242 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    writeFile(proc.path / "stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");

    LinuxProcessProbe probe(proc.path);
    // Time passes between enumerate()'s stat pass and the totalCpuTime() call: the total must
    // already have been taken, so moving the capture after the stat pass fails this test.
    probe.setEnumerateTailHookForTesting([&proc] { writeFile(proc.path / "stat", "cpu  200 0 200 1600 0 0 0 0 0 0\n"); });
    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 1U);

    EXPECT_EQ(probe.totalCpuTime(), 1000U); // the pre-tail total
    EXPECT_EQ(probe.totalCpuTime(), 2000U); // taken once; then a fresh read
}

TEST(LinuxProcessProbeTest, AFailedPreTailTotalReadIsReturnedNotRetriedAfterTheTail)
{
    // #1119: if /proc/stat can't be read after the stat pass, totalCpuTime() returns that 0 (so
    // ProcessModel skips the interval) rather than re-reading after the tail, which would bring the
    // late denominator back.
    ScopedTempDir proc("ts_test_proc_total_cpu_fail");
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 4242 4242 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    // No /proc/stat yet: the pre-tail read fails. It appears during the tail.
    LinuxProcessProbe probe(proc.path);
    probe.setEnumerateTailHookForTesting([&proc] { writeFile(proc.path / "stat", "cpu  200 0 200 1600 0 0 0 0 0 0\n"); });
    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 1U);

    EXPECT_EQ(probe.totalCpuTime(), 0U);    // the failed pre-tail read, not a post-tail retry
    EXPECT_EQ(probe.totalCpuTime(), 2000U); // taken once; then a fresh read
}

TEST(LinuxProcessProbeTest, UnreadableFdAndIoAreReportedUnavailableNotZero)
{
    // #1110: without the needed capabilities, another user's /proc/[pid]/fd and /proc/[pid]/io can't be read. Their
    // values used to be left at 0, which the table showed as "0 FDs" / "no I/O" and the totals
    // counted. They are now marked unavailable -- and so are the process's network counters, whose
    // attribution needs that same fd directory. Here the fd "directory" is a plain file and there is
    // no io file, which fails the reads the same way for any user (a mode-000 one wouldn't for root).
    ScopedTempDir proc("ts_test_proc_unreadable_fd_io");
    const auto writeStat = [&proc](std::int32_t pid)
    {
        writeFile(proc.path / std::to_string(pid) / "stat",
                  std::format("{} (app) S 1 {} {} 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
                              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                              pid,
                              pid,
                              pid));
    };
    writeFile(proc.path / "self" / "io", "read_bytes: 0\nwrite_bytes: 0\n"); // I/O counters are readable in general

    writeStat(4242); // readable: two fds and an io file
    writeFile(proc.path / "4242" / "fd" / "0", "");
    writeFile(proc.path / "4242" / "fd" / "1", "");
    writeFile(proc.path / "4242" / "io", "rchar: 1\nwchar: 2\nread_bytes: 4096\nwrite_bytes: 8192\n");

    writeStat(4343); // unreadable: no fd directory, no io file
    writeFile(proc.path / "4343" / "fd", "not a directory");

    LinuxProcessProbe probe(proc.path);
    ASSERT_TRUE(probe.capabilities().hasIoCounters);
    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 2U);
    const auto find = [&processes](std::int32_t pid)
    {
        return std::ranges::find(processes, pid, &ProcessCounters::pid);
    };

    const auto readable = find(4242);
    ASSERT_NE(readable, processes.end());
    EXPECT_TRUE(readable->handleCountAvailable);
    EXPECT_EQ(readable->handleCount, 2);
    EXPECT_TRUE(readable->ioCountersAvailable);
    EXPECT_EQ(readable->readBytes, 4096U);
    EXPECT_EQ(readable->writeBytes, 8192U);
    EXPECT_TRUE(readable->networkCountersAvailable);

    const auto unreadable = find(4343);
    ASSERT_NE(unreadable, processes.end());
    EXPECT_FALSE(unreadable->handleCountAvailable);
    EXPECT_FALSE(unreadable->ioCountersAvailable);
    EXPECT_FALSE(unreadable->networkCountersAvailable);
}

TEST(LinuxProcessProbeTest, CpuAffinityIsReadFromCpusAllowedListWithoutA64CpuCap)
{
    // #1247: the affinity was a 64-bit mask, so a process allowed only CPUs from
    // 64 up (taskset -c 70) showed none. It now comes from /proc/[pid]/status
    // Cpus_allowed_list, under the probe's procRoot.
    ScopedTempDir proc("ts_test_proc_cpus_allowed_list");
    const auto writeProcess = [&proc](std::int32_t pid, const std::optional<std::string>& status)
    {
        writeFile(proc.path / std::to_string(pid) / "stat",
                  std::format("{} (app) S 1 {} {} 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 "
                              "0 "
                              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                              pid,
                              pid,
                              pid));
        if (status.has_value())
        {
            writeFile(proc.path / std::to_string(pid) / "status", *status);
        }
    };
    const auto statusWith = [](std::string_view cpusAllowedList)
    {
        return std::format("Name:\tapp\nUid:\t0\t0\t0\t0\nCpus_allowed:\tffffffff,"
                           "ffffffff,0000000f\n"
                           "Cpus_allowed_list:\t{}\nMems_allowed_list:\t0\n",
                           cpusAllowedList);
    };
    writeProcess(4242, statusWith("0-3,64-127"));
    writeProcess(4343, statusWith("70"));
    writeProcess(4444,
                 statusWith("0-3,64-x")); // Malformed: rejected whole, not read as 0-3
    writeProcess(4545, std::nullopt);     // No status file at all
    // A list the probe's 8 KiB stack read cuts off ("0-12" of "0-127") is not read as the part that
    // fit: the probe reads the whole file and gets all of it. Pad an earlier line so the buffer ends
    // right after "0-12".
    const std::string head = "Name:\tapp\nUid:\t0\t0\t0\t0\nGroups:\t";
    const std::string tail = "\nCpus_allowed_list:\t0-12";
    writeProcess(4646, head + std::string(STATUS_STACK_READ_SIZE - head.size() - tail.size(), '1') + tail + "7\nMems_allowed_list:\t0\n");
    // A sparse list, every even CPU up to 8190, is about 20 KiB on its own -- longer than the stack
    // read -- and is still read whole.
    std::string sparseList;
    for (std::size_t cpu = 0; cpu <= 8190; cpu += 2)
    {
        sparseList += (cpu == 0 ? "" : ",") + std::to_string(cpu);
    }
    ASSERT_GT(sparseList.size(), 2 * STATUS_STACK_READ_SIZE);
    writeProcess(4747, statusWith(sparseList));
    // A long Groups: line (many supplementary groups) pushes the list past the stack read.
    std::string groups;
    for (int gid = 100000; gid < 102000; ++gid)
    {
        groups += std::to_string(gid) + ' ';
    }
    ASSERT_GT(groups.size(), STATUS_STACK_READ_SIZE);
    writeProcess(4848, "Name:\tapp\nUid:\t0\t0\t0\t0\nGroups:\t" + groups + "\nCpus_allowed_list:\t0-3,64-127\nMems_allowed_list:\t0\n");
    // A file that really ends partway through the list line (no newline) is still not read in part.
    writeProcess(4949, "Name:\tapp\nUid:\t0\t0\t0\t0\nGroups:\t" + groups + "\nCpus_allowed_list:\t0-12");

    // No CPU sysfs online list, so the host's own online CPUs don't trim these lists (#1384).
    const auto absent = proc.path / "absent";
    LinuxProcessProbe probe(proc.path, absent, absent, absent);
    EXPECT_TRUE(probe.capabilities().hasCpuAffinity);
    const auto processes = probe.enumerate();
    const auto affinityOf = [&processes](std::int32_t pid) -> CpuAffinity
    {
        const auto it = std::ranges::find(processes, pid, &ProcessCounters::pid);
        EXPECT_NE(it, processes.end()) << pid;
        return (it == processes.end()) ? CpuAffinity{} : it->cpuAffinity;
    };

    EXPECT_EQ(affinityOf(4242), CpuAffinity::fromCpuList("0-3,64-127").value_or(CpuAffinity{}));
    EXPECT_EQ(affinityOf(4242).count(), 68U);
    EXPECT_EQ(affinityOf(4343), CpuAffinity::fromCpuList("70").value_or(CpuAffinity{}));
    EXPECT_TRUE(affinityOf(4343).test(70));
    EXPECT_TRUE(affinityOf(4444).empty());
    EXPECT_TRUE(affinityOf(4545).empty());
    EXPECT_EQ(affinityOf(4646), CpuAffinity::fromCpuList("0-127").value_or(CpuAffinity{}));
    EXPECT_EQ(affinityOf(4747).count(), 4096U);
    EXPECT_TRUE(affinityOf(4747).test(0));
    EXPECT_TRUE(affinityOf(4747).test(8190));
    EXPECT_FALSE(affinityOf(4747).test(8189));
    EXPECT_EQ(affinityOf(4848), CpuAffinity::fromCpuList("0-3,64-127").value_or(CpuAffinity{}));
    EXPECT_TRUE(affinityOf(4949).empty());
}

/// Each process's affinity from a synthetic /proc with `Cpus_allowed_list` per pid, and a fake CPU
/// sysfs root whose `online` file holds `online` (none at all when nullopt).
[[nodiscard]] std::vector<std::pair<std::int32_t, CpuAffinity>>
affinitiesWithOnline(const std::vector<std::pair<std::int32_t, std::string>>& lists, const std::optional<std::string>& online)
{
    ScopedTempDir proc("ts_test_proc_affinity_online");
    ScopedTempDir cpuSysfs("ts_test_sys_cpu_online");
    for (const auto& [pid, list] : lists)
    {
        writeFile(proc.path / std::to_string(pid) / "stat",
                  std::format("{} (app) S 1 {} {} 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
                              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                              pid,
                              pid,
                              pid));
        writeFile(proc.path / std::to_string(pid) / "status",
                  std::format("Name:\tapp\nUid:\t0\t0\t0\t0\nCpus_allowed_list:\t{}\nMems_allowed_list:\t0\n", list));
    }
    if (online.has_value())
    {
        writeFile(cpuSysfs.path / "online", *online);
    }
    const auto absent = proc.path / "absent";
    LinuxProcessProbe probe(proc.path, absent, absent, cpuSysfs.path);
    std::vector<std::pair<std::int32_t, CpuAffinity>> result;
    for (const auto& process : probe.enumerate())
    {
        result.emplace_back(process.pid, process.cpuAffinity);
    }
    std::ranges::sort(result, {}, &std::pair<std::int32_t, CpuAffinity>::first);
    return result;
}

[[nodiscard]] CpuAffinity cpuList(std::string_view list)
{
    const auto affinity = CpuAffinity::fromCpuList(list);
    EXPECT_TRUE(affinity.has_value()) << list;
    return affinity.value_or(CpuAffinity{});
}

TEST(LinuxProcessProbeTest, CpuAffinityIsLimitedToOnlineCpus)
{
    // #1384: Cpus_allowed_list is the task's raw mask, which keeps possible-but-offline CPUs (a VM
    // with hot-add slots: possible 0-63, online 0-7). sched_getaffinity() -- what the column showed
    // before #1360 -- ANDs it with the active CPUs; so does the probe, with the sysfs online list.
    const auto affinities = affinitiesWithOnline({{4242, "0-63"}, {4343, "4-127"}, {4444, "2"}}, "0-7\n");
    ASSERT_EQ(affinities.size(), 3U);
    EXPECT_EQ(affinities[0].second, cpuList("0-7"));
    EXPECT_EQ(affinities[0].second.count(), 8U);
    EXPECT_EQ(affinities[1].second, cpuList("4-7")); // A spilled list comes back inline
    EXPECT_EQ(affinities[1].second.words().size(), 1U);
    EXPECT_EQ(affinities[2].second, cpuList("2"));
}

TEST(LinuxProcessProbeTest, CpuAffinityFollowsASparseOnlineList)
{
    // CPUs 4 and 5 taken offline by hand (echo 0 > cpu4/online): online reads 0-3,6-7.
    const auto affinities = affinitiesWithOnline({{4242, "0-63"}, {4343, "0-3,64-127"}}, "0-3,6-7\n");
    ASSERT_EQ(affinities.size(), 2U);
    EXPECT_EQ(affinities[0].second, cpuList("0-3,6-7"));
    EXPECT_FALSE(affinities[0].second.test(4));
    EXPECT_FALSE(affinities[0].second.test(5));
    EXPECT_EQ(affinities[1].second, cpuList("0-3"));
}

TEST(LinuxProcessProbeTest, CpuAffinityIsTheRawListWhenOnlineCantBeUsed)
{
    // No online file, or one that doesn't parse: each process's list is used as it is.
    for (const auto& online : {std::optional<std::string>{}, std::optional<std::string>{"garbage\n"}, std::optional<std::string>{""}})
    {
        const auto affinities = affinitiesWithOnline({{4242, "0-63"}, {4343, "0-3,64-127"}}, online);
        ASSERT_EQ(affinities.size(), 2U) << online.value_or("<missing>");
        EXPECT_EQ(affinities[0].second, cpuList("0-63")) << online.value_or("<missing>");
        EXPECT_EQ(affinities[1].second, cpuList("0-3,64-127")) << online.value_or("<missing>");
    }
}

TEST(LinuxProcessProbeTest, CpuAffinityWithNoOnlineCpuInCommonKeepsTheRawList)
{
    // A process pinned to a CPU that went offline between the two reads: the raw list, not "unknown".
    const auto affinities = affinitiesWithOnline({{4242, "8-9"}}, "0-7\n");
    ASSERT_EQ(affinities.size(), 1U);
    EXPECT_EQ(affinities[0].second, cpuList("8-9"));
}

TEST(LinuxProcessProbeTest, CpuAffinityReadsAnOnlineListLongerThanItsStackBuffer)
{
    // Every even CPU up to 2046 online (over 4 KiB of list): read whole, not cut off partway.
    std::string online;
    for (std::size_t cpu = 0; cpu <= 2046; cpu += 2)
    {
        online += (cpu == 0 ? "" : ",") + std::to_string(cpu);
    }
    ASSERT_GT(online.size(), 4096U);
    const auto affinities = affinitiesWithOnline({{4242, "0-2047"}}, online + "\n");
    ASSERT_EQ(affinities.size(), 1U);
    EXPECT_EQ(affinities[0].second.count(), 1024U);
    EXPECT_TRUE(affinities[0].second.test(2046));
    EXPECT_FALSE(affinities[0].second.test(2045));
}

TEST(LinuxProcessProbeTest, OwnProcessAffinityMatchesSchedGetaffinity)
{
    // The real /proc: our own Cpus_allowed_list, limited to the online CPUs (#1384), names the same
    // CPUs the kernel's affinity call does -- on hosts with possible-but-offline CPUs too. The set is sized at run time (CPU_ALLOC),
    // growing until the kernel's mask fits, so a machine with more than CPU_SETSIZE (1024) CPUs is checked in full.
    struct CpuSetDeleter
    {
        void operator()(cpu_set_t* set) const noexcept // NOLINT(misc-include-cleaner) - <sched.h>
        {
            CPU_FREE(set);
        }
    };
    std::size_t cpuCapacity = CPU_SETSIZE;
    std::unique_ptr<cpu_set_t, CpuSetDeleter> set;
    std::size_t setSize = 0;
    for (;;)
    {
        set.reset(CPU_ALLOC(cpuCapacity));
        ASSERT_NE(set, nullptr);
        setSize = CPU_ALLOC_SIZE(cpuCapacity);
        CPU_ZERO_S(setSize, set.get());
        if (sched_getaffinity(0, setSize, set.get()) == 0)
        {
            break;
        }
        ASSERT_EQ(errno, EINVAL);
        ASSERT_LT(cpuCapacity, CpuAffinity::MAX_CPUS) << "kernel affinity mask larger than CpuAffinity supports";
        cpuCapacity *= 2;
    }

    LinuxProcessProbe probe;
    const auto processes = probe.enumerate();
    const auto self = std::ranges::find(processes, static_cast<std::int32_t>(::getpid()), &ProcessCounters::pid);
    ASSERT_NE(self, processes.end());
    ASSERT_FALSE(self->cpuAffinity.empty());
    EXPECT_EQ(self->cpuAffinity.count(), static_cast<std::size_t>(CPU_COUNT_S(setSize, set.get())));
    for (std::size_t cpu = 0; cpu < cpuCapacity; ++cpu)
    {
        EXPECT_EQ(self->cpuAffinity.test(cpu), CPU_ISSET_S(cpu, setSize, set.get()) != 0) << cpu;
    }
}
/// Restores a directory's permissions on scope exit, so ScopedTempDir can remove it.
class RestoreDirPermissions
{
  public:
    explicit RestoreDirPermissions(std::filesystem::path dir) : m_Dir(std::move(dir))
    {}
    ~RestoreDirPermissions()
    {
        std::error_code ignored;
        std::filesystem::permissions(m_Dir, std::filesystem::perms::owner_all, ignored);
    }
    RestoreDirPermissions(const RestoreDirPermissions&) = delete;
    RestoreDirPermissions& operator=(const RestoreDirPermissions&) = delete;
    RestoreDirPermissions(RestoreDirPermissions&&) = delete;
    RestoreDirPermissions& operator=(RestoreDirPermissions&&) = delete;

  private:
    std::filesystem::path m_Dir;
};

TEST(LinuxProcessProbeTest, ListableFdsWhoseLinksCantBeReadReportNetworkUnavailable)
{
    // #1328: with CAP_DAC_READ_SEARCH but not CAP_SYS_PTRACE, another user's /proc/[pid]/fd can be
    // listed but its links can't be read, and the socket inode-to-PID map reads those links. The FD
    // count is known, but none of the process's connections can be attributed to it: network must be
    // unavailable, not a confident 0. A directory with read but no search permission behaves the same
    // way for an ordinary user: readdir() lists the entries, readlink() on them fails with EACCES.
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root can read links in a directory without search permission";
    }
    ScopedTempDir proc("ts_test_proc_fd_links_denied");
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 4242 4242 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    const auto fdDir = proc.path / "4242" / "fd";
    std::filesystem::create_directories(fdDir);
    std::filesystem::create_symlink("socket:[11]", fdDir / "3");
    std::filesystem::create_symlink("/dev/null", fdDir / "4");
    std::filesystem::permissions(fdDir, std::filesystem::perms::owner_read);
    const RestoreDirPermissions restore(fdDir);

    LinuxProcessProbe probe(proc.path);
    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_TRUE(processes[0].handleCountAvailable);
    EXPECT_EQ(processes[0].handleCount, 2);
    EXPECT_FALSE(processes[0].networkCountersAvailable);

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    // The same from a pass that only counts (no network attribution, so no inode-to-PID map built
    // from the walk, #1426): the first pass above rebuilt the map wherever netlink is available.
    LinuxProcessProbe countOnly(proc.path);
    countOnly.setSocketStatsForTesting(nullptr);
    const auto counted = countOnly.enumerate();
    ASSERT_EQ(counted.size(), 1U);
    EXPECT_TRUE(counted[0].handleCountAvailable);
    EXPECT_EQ(counted[0].handleCount, 2);
    EXPECT_FALSE(counted[0].networkCountersAvailable);
#endif
}

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
// #2398: the socket dump sees only this process's network namespace, so a process in another one (a
// container's) has unknown network traffic, not a confident 0.
TEST(LinuxProcessProbeTest, AProcessInAnotherNetworkNamespaceHasNoNetworkCounters)
{
    ScopedTempDir proc("ts_test_proc_netns");
    for (const char* pid : {"4242", "4243"})
    {
        writeFile(proc.path / pid / "stat",
                  std::string(pid) + " (app) S 1 1 1 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
                                     "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
        std::filesystem::create_directories(proc.path / pid / "fd");
        std::filesystem::create_directories(proc.path / pid / "ns");
    }
    writeFile(proc.path / "stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");
    std::filesystem::create_directories(proc.path / "self" / "ns");
    std::filesystem::create_symlink("net:[4026531840]", proc.path / "self" / "ns" / "net");
    std::filesystem::create_symlink("net:[4026532999]", proc.path / "4242" / "ns" / "net"); // a container's
    std::filesystem::create_symlink("net:[4026531840]", proc.path / "4243" / "ns" / "net"); // ours
    std::filesystem::create_symlink("socket:[11]", proc.path / "4242" / "fd" / "3");
    std::filesystem::create_symlink("socket:[12]", proc.path / "4243" / "fd" / "3");

    LinuxProcessProbe probe(proc.path);
    if (!probe.capabilities().hasNetworkCounters)
    {
        GTEST_SKIP() << "no netlink socket stats here";
    }
    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 2U);
    for (const auto& process : processes)
    {
        EXPECT_EQ(process.networkCountersAvailable, process.pid == 4243) << process.pid;
    }
}

TEST(LinuxProcessProbeTest, ReadSocketTrafficReportsRawAttributedSocketCounters)
{
    // #1099: the probe reports each socket's own cumulative counters, attributed through the
    // inode-to-PID map, and leaves the per-process accounting to Domain (SocketTrafficAccumulator):
    // no per-socket deltas, no per-process sums, and enumerate() carries no network bytes.
    ScopedTempDir proc("ts_test_proc_net_raw");
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 4242 4242 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    writeFile(proc.path / "stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");
    std::filesystem::create_directories(proc.path / "4242" / "fd");
    std::filesystem::create_symlink("socket:[11]", proc.path / "4242" / "fd" / "3");
    std::filesystem::create_symlink("socket:[12]", proc.path / "4242" / "fd" / "4");

    using Platform::TestSupport::FakeSocket;
    using Platform::TestSupport::ScriptedNetlinkTransport;
    const std::vector<std::vector<FakeSocket>> readings{
        {{.inode = 11, .bytesReceived = 1'000, .bytesSent = 10}, {.inode = 12, .bytesReceived = 5'000}, {.inode = 99, .bytesReceived = 7}},
        {},                                                       // failed dump (scripted below)
        {{.inode = 11, .bytesReceived = 3'000, .bytesSent = 20}}, // 12 closed
    };
    std::size_t reading = 0;
    auto transport = std::make_unique<ScriptedNetlinkTransport>();
    auto* script = transport.get();
    auto stats = std::make_shared<Platform::NetlinkSocketStats>(std::move(transport), std::chrono::milliseconds{0});
    script->onRequest = [&](const ScriptedNetlinkTransport::Request& request) -> ScriptedNetlinkTransport::Reply
    {
        if (request.family != AF_INET)
        {
            return Platform::TestSupport::completeDump(request, {});
        }
        const std::size_t index = reading++;
        if (index == 1)
        {
            return {Platform::TestSupport::errorDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID, -ENOBUFS)};
        }
        return Platform::TestSupport::completeDump(request, readings.at(index));
    };

    LinuxProcessProbe probe(proc.path);
    probe.setSocketStatsForTesting(stats);
    ASSERT_TRUE(probe.capabilities().hasNetworkCounters);

    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes[0].netReceivedBytes, 0U);
    EXPECT_EQ(processes[0].netSampleTimeNs, 0U);

    const auto first = probe.readSocketTraffic();
    EXPECT_NE(first.sampleTimeNs, 0U);
    ASSERT_EQ(first.sockets.size(), 3U);
    const auto find = [](const Platform::SocketTrafficReading& traffic, std::uint64_t inode)
    {
        return std::ranges::find(traffic.sockets, inode, &Platform::SocketTrafficSample::key);
    };
    const auto socket11 = find(first, 11);
    const auto socket12 = find(first, 12);
    const auto socket99 = find(first, 99);
    ASSERT_NE(socket11, first.sockets.end());
    ASSERT_NE(socket12, first.sockets.end());
    ASSERT_NE(socket99, first.sockets.end());
    EXPECT_EQ(socket11->pid, 4242);
    EXPECT_EQ(socket11->ownerStartTimeTicks, processes[0].startTimeTicks) << "the owner's start time, as enumerate() reads it (#1336)";
    EXPECT_EQ(socket11->ownerStartTimeTicks, 12345U);
    EXPECT_EQ(socket11->bytesReceived, 1'000U);
    EXPECT_EQ(socket11->bytesSent, 10U);
    EXPECT_EQ(socket12->pid, 4242);
    EXPECT_EQ(socket12->bytesReceived, 5'000U);
    EXPECT_EQ(socket99->pid, 0) << "a socket no process in /proc holds is reported unattributed";

    const auto failed = probe.readSocketTraffic();
    EXPECT_EQ(failed.sampleTimeNs, 0U) << "a failed dump is no reading, not an empty one";
    EXPECT_TRUE(failed.sockets.empty());

    const auto third = probe.readSocketTraffic();
    EXPECT_GT(third.sampleTimeNs, first.sampleTimeNs);
    ASSERT_EQ(third.sockets.size(), 1U);
    EXPECT_EQ(third.sockets[0].bytesReceived, 3'000U) << "the raw cumulative counter, not a delta";
    EXPECT_EQ(third.sockets[0].pid, 4242);
}

TEST(LinuxProcessProbeTest, ANewSocketIsAttributedInTheReadingItFirstAppearsIn)
{
    // #1259: the inode-to-PID map is rebuilt every INODE_PID_CACHE_TTL_MS, so a connection opened
    // just after a rebuild used to stay unowned for up to that long, and a short one was never
    // credited at all. A socket that appears unowned after the map was built now triggers an early
    // (rate-limited) rebuild; one that was already unowned before the build doesn't.
    ScopedTempDir proc("ts_test_proc_net_early_rebuild");
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 4242 4242 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    writeFile(proc.path / "stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");
    const auto fdDir = proc.path / "4242" / "fd";
    std::filesystem::create_directories(fdDir);
    std::filesystem::create_symlink("socket:[11]", fdDir / "3");

    using Platform::TestSupport::FakeSocket;
    using Platform::TestSupport::ScriptedNetlinkTransport;
    std::vector<FakeSocket> current{{.inode = 11, .bytesReceived = 100}, {.inode = 99, .bytesReceived = 7}};
    auto transport = std::make_unique<ScriptedNetlinkTransport>();
    auto* script = transport.get();
    auto stats = std::make_shared<Platform::NetlinkSocketStats>(std::move(transport), std::chrono::milliseconds{0});
    script->onRequest = [&](const ScriptedNetlinkTransport::Request& request) -> ScriptedNetlinkTransport::Reply
    {
        return Platform::TestSupport::completeDump(request, request.family == AF_INET ? current : std::vector<FakeSocket>{});
    };

    LinuxProcessProbe probe(proc.path);
    probe.setSocketStatsForTesting(stats);
    probe.setInodeMapEarlyRebuildIntervalForTesting(std::chrono::milliseconds{0}); // no rate limit, for the test
    ASSERT_TRUE(probe.capabilities().hasNetworkCounters);
    const auto ownerOf = [](const Platform::SocketTrafficReading& traffic, std::uint64_t inode)
    {
        const auto it = std::ranges::find(traffic.sockets, inode, &Platform::SocketTrafficSample::key);
        return it != traffic.sockets.end() ? it->pid : -1;
    };

    const auto first = probe.readSocketTraffic(); // builds the map
    EXPECT_EQ(ownerOf(first, 11), 4242);
    EXPECT_EQ(ownerOf(first, 99), 0);

    // 99 was unowned before the map was built (another user's process, say): it must not force a
    // rebuild, so an fd for it appearing now isn't seen until the TTL rebuild.
    std::filesystem::create_symlink("socket:[99]", fdDir / "5");
    const auto second = probe.readSocketTraffic();
    EXPECT_EQ(ownerOf(second, 99), 0) << "an already-unowned socket doesn't trigger an early rebuild";

    // 12 opens after the build: the map is rebuilt in this very reading and it has its owner.
    std::filesystem::create_symlink("socket:[12]", fdDir / "4");
    current.push_back({.inode = 12, .bytesReceived = 4'096});
    const auto third = probe.readSocketTraffic();
    EXPECT_EQ(ownerOf(third, 12), 4242) << "a new socket is attributed in the reading it first appears in";
    EXPECT_EQ(ownerOf(third, 99), 4242);
}

TEST(LinuxProcessProbeTest, AnEmptyInodeMapIsRescannedAtMostOncePerEarlyInterval)
{
    // #1327 review: when every visible socket belongs to a process we can't read, each scan comes
    // back empty. The empty-scan path backdated the cache time for a quick retry, which also let the
    // early rebuild for those (still unowned) sockets through: two /proc/*/fd scans per reading.
    // The last attempt is now tracked separately and gates every rebuild.
    ScopedTempDir proc("ts_test_proc_net_empty_map");
    writeFile(proc.path / "4343" / "stat",
              "4343 (app) S 1 4343 4343 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    writeFile(proc.path / "stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");
    writeFile(proc.path / "4343" / "fd", "not a directory"); // its fds can't be read

    using Platform::TestSupport::FakeSocket;
    using Platform::TestSupport::ScriptedNetlinkTransport;
    const std::vector<FakeSocket> current{{.inode = 99, .bytesReceived = 7}};
    auto transport = std::make_unique<ScriptedNetlinkTransport>();
    auto* script = transport.get();
    auto stats = std::make_shared<Platform::NetlinkSocketStats>(std::move(transport), std::chrono::milliseconds{0});
    script->onRequest = [&](const ScriptedNetlinkTransport::Request& request) -> ScriptedNetlinkTransport::Reply
    {
        return Platform::TestSupport::completeDump(request, request.family == AF_INET ? current : std::vector<FakeSocket>{});
    };

    constexpr auto EARLY_INTERVAL = std::chrono::milliseconds{500};
    LinuxProcessProbe probe(proc.path);
    probe.setSocketStatsForTesting(stats);
    probe.setInodeMapEarlyRebuildIntervalForTesting(EARLY_INTERVAL);
    int scans = 0;
    probe.setInodeMapScanHookForTesting([&scans] { ++scans; });
    ASSERT_TRUE(probe.capabilities().hasNetworkCounters);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i)
    {
        const auto traffic = probe.readSocketTraffic();
        ASSERT_EQ(traffic.sockets.size(), 1U);
        EXPECT_EQ(traffic.sockets[0].pid, 0);
    }
    if (std::chrono::steady_clock::now() - start >= EARLY_INTERVAL)
    {
        GTEST_SKIP() << "the readings took longer than the early interval";
    }
    EXPECT_EQ(scans, 1) << "one scan for three readings within the early interval";

    std::this_thread::sleep_for(EARLY_INTERVAL + std::chrono::milliseconds{100});
    (void) probe.readSocketTraffic();
    EXPECT_EQ(scans, 2) << "once the interval has passed, one more scan -- not a retry plus an early rebuild";
}

TEST(LinuxProcessProbeTest, AConnectionAttributedOnACachedSocketQueryKeepsItsHeldBytes)
{
    // #1327 review: a rate-limited early rebuild can run on a cached socket query, so a connection
    // gets its owner in a reading with the same sampleTimeNs as the last. Domain used to skip that
    // reading as a repeat, and the bytes the connection moved while unowned were lost if it closed
    // before the next fresh query. End to end, through SocketTrafficAccumulator, they are credited.
    ScopedTempDir proc("ts_test_proc_net_cached_rebuild");
    writeFile(proc.path / "4242" / "stat",
              "4242 (app) S 1 4242 4242 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    writeFile(proc.path / "stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");
    const auto fdDir = proc.path / "4242" / "fd";
    std::filesystem::create_directories(fdDir);
    std::filesystem::create_symlink("socket:[11]", fdDir / "3");

    using Platform::TestSupport::FakeSocket;
    using Platform::TestSupport::ScriptedNetlinkTransport;
    std::vector<FakeSocket> current{{.inode = 11, .bytesReceived = 10}};
    auto transport = std::make_unique<ScriptedNetlinkTransport>();
    auto* script = transport.get();
    // Cached until invalidated, so a reading is fresh only when the test says so.
    auto stats = std::make_shared<Platform::NetlinkSocketStats>(std::move(transport), std::chrono::hours{1});
    script->onRequest = [&](const ScriptedNetlinkTransport::Request& request) -> ScriptedNetlinkTransport::Reply
    {
        return Platform::TestSupport::completeDump(request, request.family == AF_INET ? current : std::vector<FakeSocket>{});
    };

    constexpr auto EARLY_INTERVAL = std::chrono::milliseconds{500};
    LinuxProcessProbe probe(proc.path);
    probe.setSocketStatsForTesting(stats);
    probe.setInodeMapEarlyRebuildIntervalForTesting(EARLY_INTERVAL);
    ASSERT_TRUE(probe.capabilities().hasNetworkCounters);

    Domain::SocketTrafficAccumulator accumulator;
    Platform::ProcessCounters owner;
    owner.pid = 4242;
    owner.startTimeTicks = 12345;
    std::vector processes{owner};
    const auto ownerOf = [](const Platform::SocketTrafficReading& traffic, std::uint64_t inode)
    {
        const auto it = std::ranges::find(traffic.sockets, inode, &Platform::SocketTrafficSample::key);
        return it != traffic.sockets.end() ? it->pid : -1;
    };

    const auto start = std::chrono::steady_clock::now();
    accumulator.apply(probe.readSocketTraffic(), processes); // builds the map

    // 12 opens with no fd visible yet; the early rebuild it asks for is rate-limited.
    current.push_back({.inode = 12, .bytesReceived = 100});
    stats->invalidateCache();
    const auto unowned = probe.readSocketTraffic();
    ASSERT_EQ(ownerOf(unowned, 12), 0);
    accumulator.apply(unowned, processes);

    current.back().bytesReceived = 600; // 500 held for its future owner
    stats->invalidateCache();
    const auto held = probe.readSocketTraffic();
    ASSERT_EQ(ownerOf(held, 12), 0);
    accumulator.apply(held, processes);
    if (std::chrono::steady_clock::now() - start >= EARLY_INTERVAL)
    {
        GTEST_SKIP() << "the readings took longer than the early interval";
    }

    // Its fd appears; once the interval passes, the next read rebuilds on the cached query.
    std::filesystem::create_symlink("socket:[12]", fdDir / "4");
    std::this_thread::sleep_for(EARLY_INTERVAL + std::chrono::milliseconds{100});
    const auto attributed = probe.readSocketTraffic();
    ASSERT_EQ(attributed.sampleTimeNs, held.sampleTimeNs) << "the cached query, not a fresh one";
    ASSERT_EQ(ownerOf(attributed, 12), 4242);
    accumulator.apply(attributed, processes);

    // 12 closes before the next fresh query.
    current.pop_back();
    stats->invalidateCache();
    accumulator.apply(probe.readSocketTraffic(), processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 500U) << "the bytes 12 moved while unowned";
}

TEST(LinuxProcessProbeTest, ACompleteEmptyReadingForgetsUnownedSocketsAndAFailedOneDoesNot)
{
    // #1327 review: the unowned sockets' first-seen times were replaced only by a reading with
    // sockets in it, so an empty one left them behind. A later connection reusing one of those
    // inodes then looked older than the map and never triggered the early rebuild.
    using Platform::TestSupport::FakeSocket;
    using Platform::TestSupport::ScriptedNetlinkTransport;
    const auto ownerOfReused = [](bool failMiddleReading)
    {
        ScopedTempDir proc(failMiddleReading ? "ts_test_proc_net_unowned_failed" : "ts_test_proc_net_unowned_empty");
        writeFile(proc.path / "4242" / "stat",
                  "4242 (app) S 1 4242 4242 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 12345 0 0 "
                  "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
        writeFile(proc.path / "stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");
        const auto fdDir = proc.path / "4242" / "fd";
        std::filesystem::create_directories(fdDir);
        std::filesystem::create_symlink("socket:[11]", fdDir / "3");

        std::vector<FakeSocket> current{{.inode = 11, .bytesReceived = 10}, {.inode = 99, .bytesReceived = 7}};
        bool failNext = false;
        auto transport = std::make_unique<ScriptedNetlinkTransport>();
        auto* script = transport.get();
        auto stats = std::make_shared<Platform::NetlinkSocketStats>(std::move(transport), std::chrono::milliseconds{0});
        script->onRequest = [&](const ScriptedNetlinkTransport::Request& request) -> ScriptedNetlinkTransport::Reply
        {
            if (request.family == AF_INET && failNext)
            {
                failNext = false;
                return {Platform::TestSupport::errorDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID, -ENOBUFS)};
            }
            return Platform::TestSupport::completeDump(request, request.family == AF_INET ? current : std::vector<FakeSocket>{});
        };

        LinuxProcessProbe probe(proc.path);
        probe.setSocketStatsForTesting(stats);
        probe.setInodeMapEarlyRebuildIntervalForTesting(std::chrono::milliseconds{0}); // no rate limit, for the test
        EXPECT_TRUE(probe.capabilities().hasNetworkCounters);

        const auto first = probe.readSocketTraffic(); // builds the map; 99 is unowned before the build
        EXPECT_EQ(first.sockets.size(), 2U);

        current.clear();
        failNext = failMiddleReading;
        const auto middle = probe.readSocketTraffic();
        EXPECT_TRUE(middle.sockets.empty());
        EXPECT_EQ(middle.sampleTimeNs != 0, !failMiddleReading);

        // A connection now holds inode 99, and its fd is visible.
        std::filesystem::create_symlink("socket:[99]", fdDir / "4");
        current.push_back({.inode = 99, .bytesReceived = 1});
        const auto reused = probe.readSocketTraffic();
        const auto it = std::ranges::find(reused.sockets, std::uint64_t{99}, &Platform::SocketTrafficSample::key);
        return it != reused.sockets.end() ? it->pid : -1;
    };

    EXPECT_EQ(ownerOfReused(false), 4242) << "after a complete empty reading, a reused inode is new and rebuilds the map";
    EXPECT_EQ(ownerOfReused(true), 0) << "a failed reading says nothing about the sockets: 99 is still unowned from before the build";
}
#endif

// =============================================================================
// #1425: command lines cached by process identity
// =============================================================================

/// NUL-separated, NUL-terminated arguments, as /proc/[pid]/cmdline holds them.
[[nodiscard]] std::string cmdlineOf(std::initializer_list<std::string_view> args)
{
    std::string text;
    for (const std::string_view arg : args)
    {
        text += arg;
        text.push_back('\0');
    }
    return text;
}

/// A synthetic process: its stat (comm, state, start time) and, unless nullopt,
/// its cmdline.
void writeCmdlineProcess(const std::filesystem::path& procRoot,
                         std::int32_t pid,
                         std::string_view comm,
                         char state,
                         std::uint64_t startTime,
                         const std::optional<std::string>& cmdline)
{
    const auto dir = procRoot / std::to_string(pid);
    writeFile(dir / "stat",
              std::format("{} ({}) {} 1 {} {} 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 {} 0 0 "
                          "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                          pid,
                          comm,
                          state,
                          pid,
                          pid,
                          startTime));
    std::error_code ignored;
    std::filesystem::remove(dir / "cmdline", ignored);
    if (cmdline.has_value())
    {
        writeFile(dir / "cmdline", *cmdline);
    }
}

/// The process `pid` as one enumerate() sees it (pid 0 if it isn't listed).
[[nodiscard]] ProcessCounters enumerateOne(LinuxProcessProbe& probe, std::int32_t pid)
{
    const auto processes = probe.enumerate();
    const auto it = std::ranges::find(processes, pid, &ProcessCounters::pid);
    return it != processes.end() ? *it : ProcessCounters{};
}

constexpr auto NEVER_EXPIRES = std::chrono::hours{1};

TEST(LinuxProcessProbeTest, ACommandLineIsReadOnceAndReusedWhileFresh)
{
    // #1425: /proc/[pid]/cmdline used to be opened and read for every process on
    // every sample, though a command line is set at exec. It is now kept per
    // process and reused: a change to the file within the TTL isn't read -- which
    // is how this test can see the cache at all.
    ScopedTempDir proc("ts_test_proc_cmdline_cached");
    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, cmdlineOf({"/usr/bin/app", "--first"}));
    // A comm the kernel cut at 15 characters: the full name comes from the
    // command line, and must come back with the cached command too (#951).
    writeCmdlineProcess(proc.path, 4343, "systemd-journal", 'S', 100, cmdlineOf({"/usr/lib/systemd/systemd-journald"}));

    LinuxProcessProbe probe(proc.path);
    probe.setCmdlineCacheTtlForTesting(NEVER_EXPIRES);
    EXPECT_EQ(enumerateOne(probe, 4242).command, "/usr/bin/app --first");
    EXPECT_EQ(enumerateOne(probe, 4343).name, "systemd-journald");

    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, cmdlineOf({"/usr/bin/app", "--second"}));
    EXPECT_EQ(enumerateOne(probe, 4242).command, "/usr/bin/app --first") << "same process, within the TTL: the cached command";
    const auto journald = enumerateOne(probe, 4343);
    EXPECT_EQ(journald.name, "systemd-journald") << "the full name is cached with the command";
    EXPECT_EQ(journald.command, "/usr/lib/systemd/systemd-journald");
}

TEST(LinuxProcessProbeTest, ACachedCommandLineIsReadAgainOnceItExpires)
{
    // #1425: a process can rewrite its argv (a process title: postgres, sshd), so
    // a cached command line is read again after at most the TTL.
    ScopedTempDir proc("ts_test_proc_cmdline_ttl");
    writeCmdlineProcess(proc.path, 4242, "postgres", 'S', 100, cmdlineOf({"postgres: idle"}));

    constexpr auto TTL = std::chrono::milliseconds{200};
    LinuxProcessProbe probe(proc.path);
    probe.setCmdlineCacheTtlForTesting(TTL);
    EXPECT_EQ(enumerateOne(probe, 4242).command, "postgres: idle");

    writeCmdlineProcess(proc.path, 4242, "postgres", 'S', 100, cmdlineOf({"postgres: SELECT"}));
    std::this_thread::sleep_for(TTL + std::chrono::milliseconds{50});
    EXPECT_EQ(enumerateOne(probe, 4242).command, "postgres: SELECT") << "past the TTL: read again";

    // With no TTL nothing is reused.
    LinuxProcessProbe uncached(proc.path);
    uncached.setCmdlineCacheTtlForTesting(std::chrono::milliseconds{0});
    EXPECT_EQ(enumerateOne(uncached, 4242).command, "postgres: SELECT");
    writeCmdlineProcess(proc.path, 4242, "postgres", 'S', 100, cmdlineOf({"postgres: COMMIT"}));
    EXPECT_EQ(enumerateOne(uncached, 4242).command, "postgres: COMMIT");
}

TEST(LinuxProcessProbeTest, AReusedPidGetsItsOwnCommandLine)
{
    // #1425: the cache is keyed on the process, not the PID: a process that
    // reuses the PID has a different start time, and is read, not shown the
    // exited process's command.
    ScopedTempDir proc("ts_test_proc_cmdline_pid_reuse");
    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, cmdlineOf({"app", "--old"}));

    LinuxProcessProbe probe(proc.path);
    probe.setCmdlineCacheTtlForTesting(NEVER_EXPIRES);
    EXPECT_EQ(enumerateOne(probe, 4242).command, "app --old");

    writeCmdlineProcess(proc.path, 4242, "app", 'S', 200, cmdlineOf({"app", "--new"}));
    EXPECT_EQ(enumerateOne(probe, 4242).command, "app --new") << "same PID and comm, another start time: another process";
}

TEST(LinuxProcessProbeTest, AnExecGetsItsNewCommandLine)
{
    // #1425: exec keeps the PID and the start time but replaces the command line
    // -- a shell's child is "bash" until it execs "ls". Exec also sets a new
    // comm, and a changed comm is read again.
    ScopedTempDir proc("ts_test_proc_cmdline_exec");
    writeCmdlineProcess(proc.path, 4242, "bash", 'S', 100, cmdlineOf({"bash"}));

    LinuxProcessProbe probe(proc.path);
    probe.setCmdlineCacheTtlForTesting(NEVER_EXPIRES);
    EXPECT_EQ(enumerateOne(probe, 4242).command, "bash");

    writeCmdlineProcess(proc.path, 4242, "ls", 'R', 100, cmdlineOf({"ls", "-la"}));
    const auto exec = enumerateOne(probe, 4242);
    EXPECT_EQ(exec.name, "ls");
    EXPECT_EQ(exec.command, "ls -la");
}

TEST(LinuxProcessProbeTest, AnExitedProcessLeavesTheCommandLineCache)
{
    // #1425: an entry lasts only while its process is listed: the pass that no
    // longer sees it drops it, so the cache holds no more than the live
    // processes. Seen here by bringing back a process with the very same
    // identity, which a kept entry would have answered.
    ScopedTempDir proc("ts_test_proc_cmdline_evict");
    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, cmdlineOf({"app", "--first"}));
    writeCmdlineProcess(proc.path, 4343, "other", 'S', 100, cmdlineOf({"other"}));

    LinuxProcessProbe probe(proc.path);
    probe.setCmdlineCacheTtlForTesting(NEVER_EXPIRES);
    EXPECT_EQ(enumerateOne(probe, 4242).command, "app --first");

    std::filesystem::remove_all(proc.path / "4242");
    EXPECT_EQ(enumerateOne(probe, 4242).pid, 0) << "exited";

    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, cmdlineOf({"app", "--second"}));
    EXPECT_EQ(enumerateOne(probe, 4242).command, "app --second") << "its entry went when it exited";
    EXPECT_EQ(enumerateOne(probe, 4343).command, "other");
}

TEST(LinuxProcessProbeTest, ACachedProcessThatBecomesAZombieIsDefunct)
{
    // #1155 with #1425: the <defunct> label is decided from stat's state each
    // sample, before the cache, so a process whose command was cached while it
    // ran is still shown as defunct.
    ScopedTempDir proc("ts_test_proc_cmdline_zombie");
    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, cmdlineOf({"app", "--serve"}));

    LinuxProcessProbe probe(proc.path);
    probe.setCmdlineCacheTtlForTesting(NEVER_EXPIRES);
    EXPECT_EQ(enumerateOne(probe, 4242).command, "app --serve");

    writeCmdlineProcess(proc.path, 4242, "app", 'Z', 100, std::string{}); // a zombie's cmdline reads empty
    EXPECT_EQ(enumerateOne(probe, 4242).command, "app <defunct>");
}

TEST(LinuxProcessProbeTest, AnUnreadableCommandLineIsTriedAgainNextSample)
{
    // #1425: a command line that can't be read (another user's process under
    // hidepid, or one exiting) leaves the command empty and is not cached: the
    // next sample tries again.
    ScopedTempDir proc("ts_test_proc_cmdline_unreadable");
    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, std::nullopt);

    LinuxProcessProbe probe(proc.path);
    probe.setCmdlineCacheTtlForTesting(NEVER_EXPIRES);
    EXPECT_EQ(enumerateOne(probe, 4242).command, "");

    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, cmdlineOf({"app"}));
    EXPECT_EQ(enumerateOne(probe, 4242).command, "app");
}

TEST(LinuxProcessProbeTest, ACommandLineTooLongToCacheIsReadEverySample)
{
    // #1891: a command line over the cache's 64 KiB cap is not kept, so a change to it is seen at once
    // even within the TTL.
    ScopedTempDir proc("ts_test_proc_cmdline_long");
    const std::string longArg(std::size_t{70} * 1024, 'x');
    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, cmdlineOf({"/usr/bin/app", longArg, "--first"}));

    LinuxProcessProbe probe(proc.path);
    probe.setCmdlineCacheTtlForTesting(NEVER_EXPIRES);
    EXPECT_TRUE(enumerateOne(probe, 4242).command.ends_with(" --first"));

    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, cmdlineOf({"/usr/bin/app", longArg, "--second"}));
    EXPECT_TRUE(enumerateOne(probe, 4242).command.ends_with(" --second")) << "too long to cache: read again";
}

TEST(LinuxProcessProbeTest, ACommandLineThatFailsToReadIsNotAKernelThread)
{
    // #1891: a cmdline that opens but fails to read (here a directory: EISDIR) is treated like one that
    // can't be opened -- the command stays empty and the process isn't shown as a [kernel thread].
    ScopedTempDir proc("ts_test_proc_cmdline_read_error");
    writeCmdlineProcess(proc.path, 4242, "app", 'S', 100, std::nullopt);
    std::filesystem::create_directory(proc.path / "4242" / "cmdline");

    LinuxProcessProbe probe(proc.path);
    const auto process = enumerateOne(probe, 4242);
    EXPECT_EQ(process.pid, 4242);
    EXPECT_EQ(process.name, "app");
    EXPECT_EQ(process.command, "");
}

TEST(ProcFdScanTest, OnlyASocketLinkNamesASocketInode)
{
    // #1426: the shared fd walk takes a socket's inode from its link target, as buildInodeToPidMap() did.
    EXPECT_EQ(ProcFdScan::socketInode("socket:[12345]"), std::optional<std::uint64_t>{12345});
    EXPECT_EQ(ProcFdScan::socketInode("socket:[18446744073709551615]"), std::optional<std::uint64_t>{18446744073709551615ULL});
    for (const std::string_view target : {"pipe:[12345]",
                                          "/dev/null",
                                          "anon_inode:[eventpoll]",
                                          "socket:[]",
                                          "socket:[0]",
                                          "socket:[12a]",
                                          "socket:[123",
                                          "socket:[18446744073709551616]"})
    {
        EXPECT_EQ(ProcFdScan::socketInode(target), std::nullopt) << target;
    }
}

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
TEST(LinuxProcessProbeTest, ARebuildPassCountsFdsAndBuildsTheInodeMapInOneWalk)
{
    // #1426: every process's /proc/[pid]/fd was walked twice on the samples that
    // rebuilt the socket inode-to-PID map -- once by enumerate() to count FDs,
    // once by buildInodeToPidMap(). enumerate() now builds the map from its own
    // walk when it is due, and readSocketTraffic() uses it. Both results must be
    // what the two separate walks gave.
    ScopedTempDir proc("ts_test_proc_shared_fd_walk");
    writeFile(proc.path / "stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");
    const auto writeProcess = [&proc](std::int32_t pid, std::uint64_t startTime)
    {
        writeFile(proc.path / std::to_string(pid) / "stat",
                  std::format("{} (app) S 1 {} {} 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 {} 0 0 "
                              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                              pid,
                              pid,
                              pid,
                              startTime));
        std::filesystem::create_directories(proc.path / std::to_string(pid) / "fd");
        return proc.path / std::to_string(pid) / "fd";
    };
    const auto fd4242 = writeProcess(4242, 1000); // two sockets, a file and a pipe
    std::filesystem::create_symlink("socket:[11]", fd4242 / "3");
    std::filesystem::create_symlink("socket:[12]", fd4242 / "4");
    std::filesystem::create_symlink("/dev/null", fd4242 / "5");
    std::filesystem::create_symlink("pipe:[77]", fd4242 / "6");
    const auto fd4343 = writeProcess(4343, 2000); // shares 11 (the lowest PID keeps it) and has 13
    std::filesystem::create_symlink("socket:[11]", fd4343 / "3");
    std::filesystem::create_symlink("socket:[13]", fd4343 / "4");
    const auto fd4444 = writeProcess(4444, 3000); // fds can't be listed
    std::filesystem::remove(fd4444);
    writeFile(fd4444, "not a directory");
    const auto fd4545 = writeProcess(4545, 4000); // listable, no links (a synthetic /proc's plain files)
    writeFile(fd4545 / "0", "");
    writeFile(fd4545 / "1", "");

    using Platform::TestSupport::FakeSocket;
    using Platform::TestSupport::ScriptedNetlinkTransport;
    const std::vector<FakeSocket> sockets{{.inode = 11}, {.inode = 12}, {.inode = 13}, {.inode = 99}};
    auto transport = std::make_unique<ScriptedNetlinkTransport>();
    auto* script = transport.get();
    auto stats = std::make_shared<Platform::NetlinkSocketStats>(std::move(transport), std::chrono::milliseconds{0});
    script->onRequest = [&](const ScriptedNetlinkTransport::Request& request) -> ScriptedNetlinkTransport::Reply
    {
        return Platform::TestSupport::completeDump(request, request.family == AF_INET ? sockets : std::vector<FakeSocket>{});
    };

    LinuxProcessProbe probe(proc.path);
    probe.setSocketStatsForTesting(stats);
    int scans = 0;
    probe.setInodeMapScanHookForTesting([&scans] { ++scans; });
    ASSERT_TRUE(probe.capabilities().hasNetworkCounters);

    // The first pass rebuilds the map (it has never been built), walking each fd
    // directory once.
    const auto rebuildPass = probe.enumerate();
    EXPECT_EQ(scans, 1) << "enumerate() built the map";
    const auto traffic = probe.readSocketTraffic();
    EXPECT_EQ(scans, 1) << "readSocketTraffic() used it rather than walking /proc/*/fd again";

    // The map is the one buildInodeToPidMap()'s own walk builds.
    const auto expected = buildInodeToPidMap(proc.path);
    ASSERT_EQ(traffic.sockets.size(), sockets.size());
    for (const auto& socket : traffic.sockets)
    {
        const auto it = expected.find(socket.key);
        const SocketOwner owner = it != expected.end() ? it->second : SocketOwner{};
        EXPECT_EQ(socket.pid, owner.pid) << "socket " << socket.key;
        EXPECT_EQ(socket.ownerStartTimeTicks, owner.startTimeTicks) << "socket " << socket.key;
    }
    const auto ownerOf = [&traffic](std::uint64_t inode)
    {
        const auto it = std::ranges::find(traffic.sockets, inode, &SocketTrafficSample::key);
        return it != traffic.sockets.end() ? std::pair{it->pid, it->ownerStartTimeTicks} : std::pair{-1, std::uint64_t{0}};
    };
    EXPECT_EQ(ownerOf(11), (std::pair{4242, std::uint64_t{1000}})) << "shared: the lowest PID keeps it (#1099)";
    EXPECT_EQ(ownerOf(12), (std::pair{4242, std::uint64_t{1000}}));
    EXPECT_EQ(ownerOf(13), (std::pair{4343, std::uint64_t{2000}})) << "with its owner's start time (#1336)";
    EXPECT_EQ(ownerOf(99), (std::pair{0, std::uint64_t{0}})) << "held by no process listed";

    // The FD counts are the ones a pass that doesn't rebuild the map gives: this
    // probe's next pass (the map is fresh) and a probe with no network
    // attribution at all.
    const auto countPass = probe.enumerate();
    EXPECT_EQ(scans, 1) << "the map isn't due again yet";
    LinuxProcessProbe noNetwork(proc.path);
    noNetwork.setSocketStatsForTesting(nullptr);
    const auto plainPass = noNetwork.enumerate();
    struct FdView
    {
        std::int32_t count;
        bool countAvailable;
        bool networkAvailable;
        bool operator==(const FdView&) const = default;
    };
    const auto fdsOf = [](const std::vector<ProcessCounters>& processes, std::int32_t pid)
    {
        const auto it = std::ranges::find(processes, pid, &ProcessCounters::pid);
        EXPECT_NE(it, processes.end()) << pid;
        return it != processes.end() ? FdView{it->handleCount, it->handleCountAvailable, it->networkCountersAvailable}
                                     : FdView{-1, false, false};
    };
    const std::vector<std::pair<std::int32_t, FdView>> expectedFds{
        {4242, {4, true, true}}, {4343, {2, true, true}}, {4444, {0, false, false}}, {4545, {2, true, true}}};
    for (const auto& [pid, fds] : expectedFds)
    {
        EXPECT_EQ(fdsOf(rebuildPass, pid), fds) << "rebuild pass, pid " << pid;
        EXPECT_EQ(fdsOf(countPass, pid), fds) << "count-only pass, pid " << pid;
        EXPECT_EQ(fdsOf(plainPass, pid), fds) << "no network, pid " << pid;
    }
}
TEST(LinuxProcessProbeTest, APidReusedBetweenTheStatPassAndTheFdWalkGivesTheRowNoFds)
{
    // #1426 review: a rebuild pass reopens /proc/[pid] for its fd walk after the stat pass. If the
    // process exited and its PID was reused in between, the row (the old process) must not get the new
    // process's FD count; the map still credits the new process's sockets to it, by its start time.
    ScopedTempDir proc("ts_test_proc_fd_walk_pid_reuse");
    writeFile(proc.path / "stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");
    const auto writeStat = [&proc](std::uint64_t startTime)
    {
        writeFile(proc.path / "4242" / "stat",
                  std::format("4242 (app) S 1 4242 4242 0 -1 4194304 0 0 0 0 10 5 0 0 20 0 1 0 {} 0 0 "
                              "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                              startTime));
    };
    writeStat(1000);
    const auto fdDir = proc.path / "4242" / "fd";
    std::filesystem::create_directories(fdDir);
    std::filesystem::create_symlink("/dev/null", fdDir / "0");

    using Platform::TestSupport::FakeSocket;
    using Platform::TestSupport::ScriptedNetlinkTransport;
    const std::vector<FakeSocket> sockets{{.inode = 11}};
    auto transport = std::make_unique<ScriptedNetlinkTransport>();
    auto* script = transport.get();
    auto stats = std::make_shared<Platform::NetlinkSocketStats>(std::move(transport), std::chrono::milliseconds{0});
    script->onRequest = [&](const ScriptedNetlinkTransport::Request& request) -> ScriptedNetlinkTransport::Reply
    {
        return Platform::TestSupport::completeDump(request, request.family == AF_INET ? sockets : std::vector<FakeSocket>{});
    };

    LinuxProcessProbe probe(proc.path);
    probe.setSocketStatsForTesting(stats);
    // Runs between the stat pass and the fd walk: the process exits and PID 4242 is reused.
    probe.setInodeMapScanHookForTesting(
        [&]
        {
            writeStat(2000);
            std::filesystem::create_symlink("socket:[11]", fdDir / "1");
            std::filesystem::create_symlink("/dev/null", fdDir / "2");
        });
    ASSERT_TRUE(probe.capabilities().hasNetworkCounters);

    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes[0].startTimeTicks, 1000U) << "the row is the process the stat pass read";
    EXPECT_FALSE(processes[0].handleCountAvailable) << "the reopened process's FDs aren't the row's";
    EXPECT_EQ(processes[0].handleCount, 0);
    EXPECT_FALSE(processes[0].networkCountersAvailable);

    const auto traffic = probe.readSocketTraffic();
    ASSERT_EQ(traffic.sockets.size(), 1U);
    EXPECT_EQ(traffic.sockets[0].pid, 4242);
    EXPECT_EQ(traffic.sockets[0].ownerStartTimeTicks, 2000U) << "credited to the process that holds it (#1336)";

    // With no reuse, the same pass gives the row its count.
    probe.setInodeMapScanHookForTesting({});
    probe.setInodeMapTtlForTesting(std::chrono::milliseconds{0});
    probe.setInodeMapEarlyRebuildIntervalForTesting(std::chrono::milliseconds{0});
    const auto next = probe.enumerate();
    ASSERT_EQ(next.size(), 1U);
    EXPECT_EQ(next[0].startTimeTicks, 2000U);
    EXPECT_TRUE(next[0].handleCountAvailable);
    EXPECT_EQ(next[0].handleCount, 3);
    EXPECT_TRUE(next[0].networkCountersAvailable);
}
#endif
TEST(LinuxProcessProbeTest, EmptyProcDirReturnsNoProcesses)
{
    ScopedTempDir scoped("ts_test_proc_empty");
    LinuxProcessProbe probe(scoped.path);
    auto processes = probe.enumerate();
    EXPECT_TRUE(processes.empty());
}

TEST(LinuxProcessProbeTest, NonexistentProcDirDoesNotCrash)
{
    const auto tmpDir = std::filesystem::temp_directory_path() / std::format("ts_test_proc_nonexist_{}", getpid());
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec); // ensure it does not exist; ignore error
    LinuxProcessProbe probe(tmpDir);
    auto result = probe.enumerate();
    EXPECT_TRUE(result.empty());
}

TEST(LinuxProcessProbeTest, MissingStatReturnsZeroTotalCpuTime)
{
    ScopedTempDir scoped("ts_test_proc_nocputime");
    LinuxProcessProbe probe(scoped.path);
    EXPECT_EQ(probe.totalCpuTime(), 0ULL);
}

TEST(LinuxProcessProbeTest, MissingMeminfoReturnsZeroSystemMemory)
{
    ScopedTempDir scoped("ts_test_proc_nomeminfo");
    LinuxProcessProbe probe(scoped.path);
    EXPECT_EQ(probe.systemTotalMemory(), 0ULL);
}

TEST(LinuxProcessProbeTest, ParseProcessStatMissingFileDoesNotCrash)
{
    ScopedTempDir scoped("ts_test_proc_nostatfile");
    std::filesystem::create_directories(scoped.path / "1234");
    LinuxProcessProbe probe(scoped.path);
    auto result = probe.enumerate();
    EXPECT_TRUE(result.empty());
}

TEST(LinuxProcessProbeTest, ParseProcessStatMalformedContentDoesNotCrash)
{
    ScopedTempDir scoped("ts_test_proc_badstat");
    std::filesystem::create_directories(scoped.path / "1234");
    {
        // Write garbage to stat — no valid fields; parse fails, enumerate returns empty
        std::ofstream f(scoped.path / "1234" / "stat");
        f << "not valid stat content at all\n";
    }
    LinuxProcessProbe probe(scoped.path);
    auto result = probe.enumerate();
    EXPECT_TRUE(result.empty());
}

} // namespace
} // namespace Platform

#endif
