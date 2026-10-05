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

#include "Platform/Linux/LinuxProcessProbe.h"
#include "Platform/Linux/ProcPrivileges.h"
#include "Platform/PlatformConfig.h"
#include "Platform/ProcessTypes.h"
#include "Platform/ScopedTempDir.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "Platform/Linux/NetlinkSocketStats.h"
#include "Platform/NetlinkTestUtils.h"

#include <memory>
#include <vector>

#include <sys/socket.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

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
}

TEST(LinuxProcessProbeTest, ReducedPrivilegesMatchesEuidAndEffectiveCapabilities)
{
    const LinuxProcessProbe probe;
    const auto caps = probe.capabilities();

    // Not reduced as root, or with CAP_DAC_READ_SEARCH + CAP_SYS_PTRACE effective (setcap).
    std::ifstream statusFile("/proc/self/status");
    const std::string status{std::istreambuf_iterator<char>(statusFile), std::istreambuf_iterator<char>()};
    const bool expectedReducedPrivileges = ProcPrivileges::hasReducedPrivileges(geteuid() == 0, ProcPrivileges::parseCapEff(status));
    EXPECT_EQ(caps.hasReducedPrivileges, expectedReducedPrivileges);
}

// #1327 review: the privilege notice fired for every non-root process, including one granted the
// capabilities docs/guide/faq.md recommends. CapEff (hex) in /proc/self/status decides instead.
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

TEST(LinuxProcessProbeTest, ConcurrentEnumeration)
{
    LinuxProcessProbe probe;

    std::atomic<int> successCount{0};
    std::atomic<bool> running{true};

    auto enumerateTask = [&]()
    {
        while (running)
        {
            try
            {
                auto processes = probe.enumerate();
                if (!processes.empty())
                {
                    ++successCount;
                }
            }
            catch (...)
            {
                // Enumeration should not throw
                FAIL() << "Enumeration threw an exception";
            }
        }
    };

    // Start multiple threads enumerating concurrently
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back(enumerateTask);
    }

    // Let them run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& t : threads)
    {
        t.join();
    }

    // All enumerations should have succeeded
    EXPECT_GT(successCount.load(), 0);
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

TEST(LinuxProcessProbeTest, ReducedPrivilegesReadsCapEffUnderTheProcRoot)
{
    // A readable CapEff decides for root too, so these hold whatever the test runs as.
    ScopedTempDir withCaps("ts_test_proc_capeff_full");
    writeFile(withCaps.path / "self" / "status", "Name:\tTaskSmack\nCapEff:\t0000000000080004\n");
    EXPECT_FALSE(LinuxProcessProbe(withCaps.path).capabilities().hasReducedPrivileges);

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
    // #1110: without root, another user's /proc/[pid]/fd and /proc/[pid]/io can't be read. Their
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

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
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
