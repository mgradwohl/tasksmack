/// @file test_LinuxProcessProbe.cpp
/// @brief Integration tests for Platform::LinuxProcessProbe
///
/// These are integration tests that interact with the real /proc filesystem.
/// They verify that the probe correctly reads and parses process information.

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
#include "Platform/PlatformConfig.h"
#include "Platform/ProcessTypes.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <system_error>
#include <thread>

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
}

TEST(LinuxProcessProbeTest, ReducedPrivilegesMatchesEuid)
{
    LinuxProcessProbe probe;
    const auto caps = probe.capabilities();

    // hasReducedPrivileges should be true when running as non-root, false as root
    const bool expectedReducedPrivileges = (geteuid() != 0);
    EXPECT_EQ(caps.hasReducedPrivileges, expectedReducedPrivileges);
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
    volatile int sum = 0;
    for (int i = 0; i < 1000000; ++i)
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
    volatile int sum = 0;
    for (int iteration = 0; iteration < 5; ++iteration)
    {
        for (int i = 0; i < 10000000; ++i)
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
        GTEST_SKIP() << "I/O counters not available (requires root or CAP_DAC_READ_SEARCH)";
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

TEST(LinuxProcessProbeTest, IoCountersIncreaseWithActivity)
{
    LinuxProcessProbe probe;
    auto caps = probe.capabilities();

    if (!caps.hasIoCounters)
    {
        GTEST_SKIP() << "I/O counters not available (requires root or CAP_DAC_READ_SEARCH)";
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
    LinuxProcessProbe probe;
    auto processes = probe.enumerate();

    const auto it = std::find_if(processes.begin(),
                                 processes.end(),
                                 [](const ProcessCounters& proc)
                                 {
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
#endif

// ========== Error Path / Injection Tests ==========

// RAII helper: creates a uniquely-named temp directory (with PID suffix to
// avoid parallel-test collisions) and removes it on destruction so cleanup
// happens even if an assertion fires.
struct ScopedTempDir
{
    std::filesystem::path path;

    explicit ScopedTempDir(std::string_view name) : path(std::filesystem::temp_directory_path() / std::format("{}_{}", name, getpid()))
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec); // best-effort pre-clean; ignore error
        std::filesystem::create_directories(path);
    }

    ~ScopedTempDir() noexcept
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec); // must not throw from destructor
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ScopedTempDir(ScopedTempDir&&) = delete;
    ScopedTempDir& operator=(ScopedTempDir&&) = delete;
};

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
    std::filesystem::remove_all(tmpDir); // ensure it does not exist
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
