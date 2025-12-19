/// @file test_LinuxRealProbes.cpp
/// @brief Integration tests for real Linux platform probes
///
/// These tests validate actual /proc filesystem parsing and real system behavior.
/// They ensure probes correctly handle real-world scenarios on Linux.

#include "Platform/Linux/LinuxProcessProbe.h"
#include "Platform/Linux/LinuxSystemProbe.h"

#include <gtest/gtest.h>

#include <fstream>
#include <thread>

#include <sys/types.h>
#include <unistd.h>

// =============================================================================
// Real /proc Filesystem Tests
// =============================================================================

TEST(LinuxRealProbesTest, ProcStatExistsAndIsReadable)
{
    std::ifstream statFile("/proc/stat");
    EXPECT_TRUE(statFile.is_open()) << "/proc/stat should be readable";

    std::string line;
    bool foundCpuLine = false;
    while (std::getline(statFile, line))
    {
        if (line.starts_with("cpu "))
        {
            foundCpuLine = true;
            break;
        }
    }

    EXPECT_TRUE(foundCpuLine) << "/proc/stat should contain CPU aggregate line";
}

TEST(LinuxRealProbesTest, ProcMeminfoExistsAndIsReadable)
{
    std::ifstream meminfo("/proc/meminfo");
    EXPECT_TRUE(meminfo.is_open()) << "/proc/meminfo should be readable";

    std::string line;
    bool foundMemTotal = false;
    while (std::getline(meminfo, line))
    {
        if (line.starts_with("MemTotal:"))
        {
            foundMemTotal = true;
            break;
        }
    }

    EXPECT_TRUE(foundMemTotal) << "/proc/meminfo should contain MemTotal";
}

TEST(LinuxRealProbesTest, OwnProcessProcDirExists)
{
    auto pid = getpid();
    std::string procPath = "/proc/" + std::to_string(pid);

    std::ifstream statFile(procPath + "/stat");
    EXPECT_TRUE(statFile.is_open()) << "Own process /proc/[pid]/stat should exist";
}

// =============================================================================
// LinuxProcessProbe Real Behavior Tests
// =============================================================================

TEST(LinuxRealProbesTest, ProcessProbeEnumeratesRealProcesses)
{
    Platform::LinuxProcessProbe probe;

    auto processes = probe.enumerate();

    // Should find many processes (at least init + this test)
    EXPECT_GT(processes.size(), 1ULL);

    // All processes should have valid PIDs
    for (const auto& proc : processes)
    {
        EXPECT_GT(proc.pid, 0);
        EXPECT_FALSE(proc.name.empty());
    }
}

TEST(LinuxRealProbesTest, ProcessProbeFindsOwnProcess)
{
    Platform::LinuxProcessProbe probe;

    auto processes = probe.enumerate();
    auto ownPid = static_cast<int32_t>(getpid());

    bool foundSelf = false;
    for (const auto& proc : processes)
    {
        if (proc.pid == ownPid)
        {
            foundSelf = true;

            // Validate our own process data
            EXPECT_FALSE(proc.name.empty());
            EXPECT_GT(proc.rssBytes, 0) << "Own process should have non-zero RSS";
            EXPECT_GT(proc.virtualBytes, 0) << "Own process should have non-zero virtual memory";
            EXPECT_GT(proc.threadCount, 0) << "Own process should have at least 1 thread";

            // State should be Running or Sleeping
            EXPECT_TRUE(proc.state == 'R' || proc.state == 'S')
                << "Own process state should be R or S, got: " << proc.state;

            break;
        }
    }

    EXPECT_TRUE(foundSelf) << "Should find own process (PID " << ownPid << ")";
}

TEST(LinuxRealProbesTest, ProcessProbeFindsInitProcess)
{
    Platform::LinuxProcessProbe probe;

    auto processes = probe.enumerate();

    bool foundInit = false;
    for (const auto& proc : processes)
    {
        if (proc.pid == 1)
        {
            foundInit = true;

            // Init should have specific characteristics
            EXPECT_GT(proc.rssBytes, 0);
            EXPECT_EQ(proc.parentPid, 0); // Init has no parent

            break;
        }
    }

    EXPECT_TRUE(foundInit) << "Should find init process (PID 1)";
}

TEST(LinuxRealProbesTest, TotalCpuTimeMonotonicallyIncreases)
{
    Platform::LinuxProcessProbe probe;

    auto time1 = probe.totalCpuTime();

    // Do some work to consume CPU
    volatile uint64_t sum = 0;
    for (int i = 0; i < 1000000; ++i)
    {
        sum += static_cast<uint64_t>(i);
    }

    auto time2 = probe.totalCpuTime();

    EXPECT_GE(time2, time1) << "Total CPU time should not decrease";
}

TEST(LinuxRealProbesTest, EnumerationIsConsistent)
{
    Platform::LinuxProcessProbe probe;

    auto procs1 = probe.enumerate();
    auto procs2 = probe.enumerate();

    // Process count should be similar (some may start/exit)
    auto count1 = procs1.size();
    auto count2 = procs2.size();

    // Within 10% is reasonable (processes can spawn/die)
    auto diff = std::abs(static_cast<int64_t>(count1) - static_cast<int64_t>(count2));
    auto maxDiff = count1 / 10; // 10%

    EXPECT_LE(diff, maxDiff) << "Process count between enumerations should be similar";
}

// =============================================================================
// LinuxSystemProbe Real Behavior Tests
// =============================================================================

TEST(LinuxRealProbesTest, SystemProbeReturnsValidMemory)
{
    Platform::LinuxSystemProbe probe;

    auto counters = probe.read();

    // Memory should be reasonable
    EXPECT_GT(counters.memory.totalBytes, 128ULL * 1024 * 1024) << "Should have at least 128 MB RAM";
    EXPECT_LE(counters.memory.totalBytes, 1024ULL * 1024 * 1024 * 1024) << "Should have less than 1 TB RAM";

    EXPECT_GT(counters.memory.availableBytes, 0);
    EXPECT_LE(counters.memory.availableBytes, counters.memory.totalBytes);
}

TEST(LinuxRealProbesTest, SystemProbeReturnsValidCpu)
{
    Platform::LinuxSystemProbe probe;

    auto counters = probe.read();

    // CPU counters should be non-zero
    auto total = counters.cpuTotal.user + counters.cpuTotal.nice +
                 counters.cpuTotal.system + counters.cpuTotal.idle +
                 counters.cpuTotal.iowait + counters.cpuTotal.steal;

    EXPECT_GT(total, 0ULL) << "CPU counters should have accumulated some time";
}

TEST(LinuxRealProbesTest, SystemProbeUptimeIsPositive)
{
    Platform::LinuxSystemProbe probe;

    auto counters = probe.read();

    EXPECT_GT(counters.uptimeSeconds, 0) << "System uptime should be positive";
}

TEST(LinuxRealProbesTest, SystemProbeCpuCountersIncrease)
{
    Platform::LinuxSystemProbe probe;

    auto counters1 = probe.read();

    // Do some work
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto counters2 = probe.read();

    auto total1 = counters1.cpuTotal.user + counters1.cpuTotal.nice +
                  counters1.cpuTotal.system + counters1.cpuTotal.idle;

    auto total2 = counters2.cpuTotal.user + counters2.cpuTotal.nice +
                  counters2.cpuTotal.system + counters2.cpuTotal.idle;

    EXPECT_GT(total2, total1) << "CPU counters should increase over time";
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST(LinuxRealProbesTest, ProbeHandlesProcessExitingDuringEnumeration)
{
    Platform::LinuxProcessProbe probe;

    // Create a short-lived process
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process - exit immediately
        _exit(0);
    }
    else if (pid > 0)
    {
        // Parent process - enumerate while child might be exiting
        for (int i = 0; i < 10; ++i)
        {
            auto processes = probe.enumerate();

            // Should complete successfully even if processes are exiting
            EXPECT_GE(processes.size(), 0ULL);
        }

        // Wait for child to prevent zombie
        waitpid(pid, nullptr, 0);
    }
}

TEST(LinuxRealProbesTest, ProbeHandlesHighLoadEnumeration)
{
    Platform::LinuxProcessProbe probe;

    // Enumerate many times in quick succession
    for (int i = 0; i < 50; ++i)
    {
        auto processes = probe.enumerate();
        EXPECT_GT(processes.size(), 0ULL);
    }
}
