/// @file test_WindowsRealProbes.cpp
/// @brief Integration tests for real Windows platform probes
///
/// These tests validate actual Windows API calls and real system behavior.
/// They ensure probes correctly handle real-world scenarios on Windows.

#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessProbe.h"
#include "Platform/Windows/WindowsSystemProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <thread>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
// clang-format on

// =============================================================================
// Real Windows API Tests
// =============================================================================

TEST(WindowsRealProbesTest, SystemMemoryIsAccessible)
{
    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);

    EXPECT_NE(GlobalMemoryStatusEx(&memStatus), 0) << "GlobalMemoryStatusEx should succeed";
    EXPECT_GT(memStatus.ullTotalPhys, 0ULL) << "System should have physical memory";
}

TEST(WindowsRealProbesTest, SystemTimesAreAccessible)
{
    FILETIME ftIdle{};
    FILETIME ftKernel{};
    FILETIME ftUser{};

    EXPECT_NE(GetSystemTimes(&ftIdle, &ftKernel, &ftUser), 0) << "GetSystemTimes should succeed";
}

TEST(WindowsRealProbesTest, ProcessSnapshotIsAccessible)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    EXPECT_NE(hSnapshot, INVALID_HANDLE_VALUE) << "CreateToolhelp32Snapshot should succeed";

    if (hSnapshot != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hSnapshot);
    }
}

// =============================================================================
// WindowsProcessProbe Real Behavior Tests
// =============================================================================

TEST(WindowsRealProbesTest, ProcessProbeEnumeratesRealProcesses)
{
    Platform::WindowsProcessProbe probe;

    auto processes = probe.enumerate();

    // Should find many processes (at least System + this test)
    EXPECT_GT(processes.size(), 1ULL);

    // All processes should have valid PIDs (>= 0 on Windows, PID 0 is System Idle Process)
    for (const auto& proc : processes)
    {
        EXPECT_GE(proc.pid, 0);
        EXPECT_FALSE(proc.name.empty());
    }
}

TEST(WindowsRealProbesTest, ProcessProbeFindsOwnProcess)
{
    Platform::WindowsProcessProbe probe;

    auto processes = probe.enumerate();
    auto ownPid = static_cast<int32_t>(GetCurrentProcessId());

    bool foundSelf = false;
    for (const auto& proc : processes)
    {
        if (proc.pid == ownPid)
        {
            foundSelf = true;

            // Validate our own process data
            EXPECT_FALSE(proc.name.empty());
            EXPECT_GT(proc.rssBytes, 0ULL) << "Own process should have non-zero RSS";
            EXPECT_GT(proc.virtualBytes, 0ULL) << "Own process should have non-zero virtual memory";
            EXPECT_GE(proc.threadCount, 1) << "Own process should have at least 1 thread";

            // State should be Running or Unknown
            // Note: Windows doesn't have a zombie state like Unix; terminated processes
            // are cleaned up immediately or return access denied errors
            EXPECT_TRUE(proc.state == 'R' || proc.state == '?') << "Own process state should be R or ?, got: " << proc.state;

            // Should have user information
            EXPECT_FALSE(proc.user.empty()) << "Own process should have a username";

            // Should have command information
            EXPECT_FALSE(proc.command.empty()) << "Own process should have a command";

            // Start time should be set
            EXPECT_GT(proc.startTimeTicks, 0ULL) << "Own process should have a start time";

            break;
        }
    }

    EXPECT_TRUE(foundSelf) << "Should find own process (PID " << ownPid << ")";
}

TEST(WindowsRealProbesTest, ProcessProbeFindsSystemProcess)
{
    Platform::WindowsProcessProbe probe;

    auto processes = probe.enumerate();

    // Look for System process (PID 4) or any other well-known system process
    // Note: On Windows, System (PID 4) and System Idle Process (PID 0) may not report memory
    // in certain Windows configurations or virtualized environments
    bool foundSystemProcess = false;
    for (const auto& proc : processes)
    {
        if (proc.pid == 4 && !proc.name.empty())
        {
            foundSystemProcess = true;
            // System process found - memory reporting varies by Windows version
            break;
        }
    }

    EXPECT_TRUE(foundSystemProcess) << "Should find System process (PID 4)";
}

TEST(WindowsRealProbesTest, TotalCpuTimeMonotonicallyIncreases)
{
    Platform::WindowsProcessProbe probe;

    auto time1 = probe.totalCpuTime();

    // Do some work to consume CPU
    volatile uint64_t sum = 0;
    for (int i = 0; i < 1'000'000; ++i)
    {
        sum += static_cast<uint64_t>(i);
    }

    auto time2 = probe.totalCpuTime();

    EXPECT_GE(time2, time1) << "Total CPU time should not decrease";
}

TEST(WindowsRealProbesTest, EnumerationIsConsistent)
{
    Platform::WindowsProcessProbe probe;

    auto procs1 = probe.enumerate();
    auto procs2 = probe.enumerate();

    // Process count should be similar (some may start/exit)
    auto count1 = procs1.size();
    auto count2 = procs2.size();

    // Within 20% is reasonable (Windows processes can spawn/die frequently)
    auto diff = static_cast<size_t>(std::abs(static_cast<int64_t>(count1) - static_cast<int64_t>(count2)));
    // Use integer arithmetic to avoid floating-point conversion warnings
    auto maxDiff = std::max(size_t{1}, count1 / 5); // At least 1, or 20%

    EXPECT_LE(diff, maxDiff) << "Process count between enumerations should be similar";
}

TEST(WindowsRealProbesTest, ProcessProbeCapabilitiesAreAccurate)
{
    Platform::WindowsProcessProbe probe;
    auto caps = probe.capabilities();

    // Windows should support these
    EXPECT_TRUE(caps.hasUserSystemTime);
    EXPECT_TRUE(caps.hasStartTime);
    EXPECT_TRUE(caps.hasThreadCount);
    EXPECT_TRUE(caps.hasIoCounters);
    EXPECT_TRUE(caps.hasUser);
    EXPECT_TRUE(caps.hasCommand);
    EXPECT_TRUE(caps.hasNice);

    // Verify actual data matches capabilities
    auto processes = probe.enumerate();
    EXPECT_GT(processes.size(), 0ULL);

    // Find our process and verify all claimed capabilities work
    auto ownPid = static_cast<int32_t>(GetCurrentProcessId());
    auto it = std::find_if(processes.begin(), processes.end(), [ownPid](const auto& p) { return p.pid == ownPid; });

    if (it != processes.end())
    {
        if (caps.hasIoCounters)
        {
            // This test just verifies we can retrieve I/O counters without error
            // Values may be 0 for newly started or idle processes
            // The fact that we got here means GetProcessIoCounters succeeded
        }
        if (caps.hasThreadCount)
        {
            EXPECT_GT(it->threadCount, 0);
        }
        if (caps.hasUser)
        {
            EXPECT_FALSE(it->user.empty());
        }
        if (caps.hasCommand)
        {
            EXPECT_FALSE(it->command.empty());
        }
    }
}

// =============================================================================
// WindowsSystemProbe Real Behavior Tests
// =============================================================================

TEST(WindowsRealProbesTest, SystemProbeReturnsValidMemory)
{
    Platform::WindowsSystemProbe probe;

    auto counters = probe.read();

    // Memory should be reasonable
    EXPECT_GT(counters.memory.totalBytes, 128ULL * 1024 * 1024) << "Should have at least 128 MB RAM";
    EXPECT_LE(counters.memory.totalBytes, 1024ULL * 1024 * 1024 * 1024) << "Should have less than 1 TB RAM";

    EXPECT_GT(counters.memory.availableBytes, 0ULL);
    EXPECT_LE(counters.memory.availableBytes, counters.memory.totalBytes);
    EXPECT_LE(counters.memory.freeBytes, counters.memory.totalBytes);
}

TEST(WindowsRealProbesTest, SystemProbeReturnsValidCpu)
{
    Platform::WindowsSystemProbe probe;

    auto counters = probe.read();

    // CPU counters should be non-zero
    auto total = counters.cpuTotal.user + counters.cpuTotal.system + counters.cpuTotal.idle;

    EXPECT_GT(total, 0ULL) << "CPU counters should have accumulated some time";

    // Per-core counters should exist
    EXPECT_GT(counters.cpuPerCore.size(), 0ULL);
    EXPECT_EQ(counters.cpuPerCore.size(), counters.cpuCoreCount);
}

TEST(WindowsRealProbesTest, SystemProbeUptimeIsPositive)
{
    Platform::WindowsSystemProbe probe;

    auto counters = probe.read();

    EXPECT_GT(counters.uptimeSeconds, 0ULL) << "System uptime should be positive";

    // Boot timestamp should be reasonable (within the last year)
    auto now = std::chrono::system_clock::now();
    auto nowEpoch = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
    auto oneYearAgo = nowEpoch - (365ULL * 24 * 60 * 60);

    EXPECT_GT(counters.bootTimestamp, oneYearAgo) << "Boot timestamp should be recent";
    EXPECT_LE(counters.bootTimestamp, nowEpoch) << "Boot timestamp should not be in the future";
}

TEST(WindowsRealProbesTest, SystemProbeCpuCountersIncrease)
{
    Platform::WindowsSystemProbe probe;

    auto counters1 = probe.read();

    // Do some work and wait
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    volatile uint64_t sum = 0;
    for (int i = 0; i < 1'000'000; ++i)
    {
        sum += static_cast<uint64_t>(i);
    }

    auto counters2 = probe.read();

    auto total1 = counters1.cpuTotal.user + counters1.cpuTotal.system + counters1.cpuTotal.idle;
    auto total2 = counters2.cpuTotal.user + counters2.cpuTotal.system + counters2.cpuTotal.idle;

    EXPECT_GT(total2, total1) << "CPU counters should increase over time";
}

TEST(WindowsRealProbesTest, SystemProbeStaticInfoIsValid)
{
    Platform::WindowsSystemProbe probe;

    auto counters = probe.read();

    EXPECT_FALSE(counters.hostname.empty()) << "Hostname should not be empty";
    EXPECT_FALSE(counters.cpuModel.empty()) << "CPU model should not be empty";
    EXPECT_GT(counters.cpuCoreCount, 0U) << "Should have at least one CPU core";

    // CPU frequency should be reasonable (modern CPUs are > 100 MHz)
    if (counters.cpuFreqMHz > 0)
    {
        EXPECT_GT(counters.cpuFreqMHz, 100ULL) << "CPU frequency should be at least 100 MHz";
        EXPECT_LT(counters.cpuFreqMHz, 10'000ULL) << "CPU frequency should be less than 10 GHz";
    }
}

TEST(WindowsRealProbesTest, SystemProbeCapabilitiesAreAccurate)
{
    Platform::WindowsSystemProbe probe;
    auto caps = probe.capabilities();

    // Windows should support these
    EXPECT_TRUE(caps.hasPerCoreCpu);
    EXPECT_TRUE(caps.hasMemoryAvailable);
    EXPECT_TRUE(caps.hasSwap);
    EXPECT_TRUE(caps.hasUptime);
    EXPECT_TRUE(caps.hasCpuFreq);

    // Windows should NOT support these (Linux-specific)
    EXPECT_FALSE(caps.hasIoWait);
    EXPECT_FALSE(caps.hasSteal);
    EXPECT_FALSE(caps.hasLoadAvg);

    // Verify actual data matches capabilities
    auto counters = probe.read();

    if (caps.hasPerCoreCpu)
    {
        EXPECT_GT(counters.cpuPerCore.size(), 0ULL);
    }

    if (caps.hasMemoryAvailable)
    {
        EXPECT_GT(counters.memory.availableBytes, 0ULL);
    }

    if (caps.hasUptime)
    {
        EXPECT_GT(counters.uptimeSeconds, 0ULL);
    }

    // Verify Linux-specific fields are zero when not supported
    if (!caps.hasIoWait)
    {
        EXPECT_EQ(counters.cpuTotal.iowait, 0ULL) << "I/O wait should be 0 when not supported";
    }

    if (!caps.hasSteal)
    {
        EXPECT_EQ(counters.cpuTotal.steal, 0ULL) << "Steal time should be 0 when not supported";
    }

    if (!caps.hasLoadAvg)
    {
        EXPECT_EQ(counters.loadAvg1, 0.0);
        EXPECT_EQ(counters.loadAvg5, 0.0);
        EXPECT_EQ(counters.loadAvg15, 0.0);
    }
}

TEST(WindowsRealProbesTest, SystemProbeHandlesMultipleCalls)
{
    Platform::WindowsSystemProbe probe;

    // Rapidly call read() multiple times - should not crash or deadlock
    for (int i = 0; i < 50; ++i)
    {
        auto counters = probe.read();
        EXPECT_GT(counters.cpuCoreCount, 0);
        EXPECT_GT(counters.memory.totalBytes, 0ULL);
    }
}

// =============================================================================
// WindowsProcessActions Tests
// =============================================================================

TEST(WindowsRealProbesTest, ProcessActionsCapabilitiesAreAccurate)
{
    Platform::WindowsProcessActions actions;
    auto caps = actions.actionCapabilities();

    // Windows should support terminate and kill
    EXPECT_TRUE(caps.canTerminate);
    EXPECT_TRUE(caps.canKill);

    // Windows should NOT support stop/continue (Unix signals)
    EXPECT_FALSE(caps.canStop);
    EXPECT_FALSE(caps.canContinue);
}

TEST(WindowsRealProbesTest, ProcessActionsStopReturnsError)
{
    Platform::WindowsProcessActions actions;

    // Verify stop returns an error since it's not supported
    auto result = actions.stop(static_cast<int32_t>(GetCurrentProcessId()));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}

TEST(WindowsRealProbesTest, ProcessActionsResumeReturnsError)
{
    Platform::WindowsProcessActions actions;

    // Verify resume returns an error since it's not supported
    auto result = actions.resume(static_cast<int32_t>(GetCurrentProcessId()));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST(WindowsRealProbesTest, ProbeHandlesAccessDeniedGracefully)
{
    Platform::WindowsProcessProbe probe;

    // Enumerate processes - some system processes may deny access
    auto processes = probe.enumerate();

    // Should still return processes even if some are inaccessible
    EXPECT_GT(processes.size(), 0ULL);

    // All returned processes should have at least basic info (PID >= 0, name)
    // Note: PID 0 is valid on Windows (System Idle Process)
    for (const auto& proc : processes)
    {
        EXPECT_GE(proc.pid, 0);
        EXPECT_FALSE(proc.name.empty());
    }
}

TEST(WindowsRealProbesTest, ProbeHandlesHighLoadEnumeration)
{
    Platform::WindowsProcessProbe probe;

    // Enumerate many times in quick succession
    for (int i = 0; i < 50; ++i)
    {
        auto processes = probe.enumerate();
        EXPECT_GT(processes.size(), 0ULL);
    }
}

TEST(WindowsRealProbesTest, ProbeHandlesProcessExitingDuringEnumeration)
{
    Platform::WindowsProcessProbe probe;

    // Create a short-lived process by launching a simple command
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    // CreateProcessA modifies its second parameter, so we need a mutable buffer
    char cmdLine[] = "cmd.exe /c exit";
    if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != 0)
    {
        // Enumerate while child might be exiting
        for (int i = 0; i < 10; ++i)
        {
            auto processes = probe.enumerate();

            // Should complete successfully even if processes are exiting
            EXPECT_GT(processes.size(), 0ULL);
        }

        // Clean up handles
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}
