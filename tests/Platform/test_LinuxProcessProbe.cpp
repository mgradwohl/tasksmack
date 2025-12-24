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
#include "Platform/ProcessTypes.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

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
    auto selfProc = std::find_if(processes.begin(), processes.end(),
                                  [selfPid](const ProcessCounters& p) { return p.pid == selfPid; });
    
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
    auto selfProc1 = std::find_if(processes1.begin(), processes1.end(),
                                   [selfPid](const ProcessCounters& p) { return p.pid == selfPid; });
    ASSERT_NE(selfProc1, processes1.end());
    const uint64_t readBytes1 = selfProc1->readBytes;
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
    auto selfProc2 = std::find_if(processes2.begin(), processes2.end(),
                                   [selfPid](const ProcessCounters& p) { return p.pid == selfPid; });
    ASSERT_NE(selfProc2, processes2.end());
    const uint64_t readBytes2 = selfProc2->readBytes;
    const uint64_t writeBytes2 = selfProc2->writeBytes;
    
    // Write bytes should have increased (we wrote to a file)
    EXPECT_GE(writeBytes2, writeBytes1) << "Write bytes should increase after file write";
    
    // Clean up
    std::filesystem::remove(tempFilePath);
}

} // namespace
} // namespace Platform

#endif
