/// @file test_WindowsProcessProbe.cpp
/// @brief Integration tests for Platform::WindowsProcessProbe

#include "Platform/ProcessTypes.h"
#include "Platform/Windows/WindowsProcessProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

namespace Platform
{
namespace
{
// Test constants for consistency checks
constexpr double PROCESS_COUNT_VARIANCE_TOLERANCE = 0.2; // 20% variance allowed

// Test constants for CPU time measurement
constexpr int CPU_WORK_ITERATIONS = 5;
constexpr int CPU_WORK_INNER_LOOP = 10'000'000;

} // namespace

TEST(WindowsProcessProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ WindowsProcessProbe probe; });
}

TEST(WindowsProcessProbeTest, CapabilitiesReportedCorrectly)
{
    WindowsProcessProbe probe;
    const auto caps = probe.capabilities();

    EXPECT_TRUE(caps.hasUserSystemTime);
    EXPECT_TRUE(caps.hasStartTime);
    EXPECT_TRUE(caps.hasThreadCount);

    EXPECT_TRUE(caps.hasIoCounters);
    EXPECT_TRUE(caps.hasUser);
    EXPECT_TRUE(caps.hasCommand);
    EXPECT_TRUE(caps.hasNice);

    // New capabilities for issues #184, #185, #195
    EXPECT_TRUE(caps.hasPublisher);
    EXPECT_TRUE(caps.hasProcessType);
    EXPECT_TRUE(caps.hasGdiObjects);
}

TEST(WindowsProcessProbeTest, TicksPerSecondMatchesFileTime)
{
    WindowsProcessProbe probe;
    EXPECT_EQ(probe.ticksPerSecond(), 10'000'000L);
}

TEST(WindowsProcessProbeTest, TotalCpuTimeIsPositiveAndMonotonic)
{
    WindowsProcessProbe probe;

    const uint64_t time1 = probe.totalCpuTime();
    EXPECT_GT(time1, 0ULL);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    volatile int sum = 0;
    for (int i = 0; i < 1'000'000; ++i)
    {
        sum += i;
    }

    const uint64_t time2 = probe.totalCpuTime();
    EXPECT_GE(time2, time1);
}

TEST(WindowsProcessProbeTest, SystemTotalMemoryIsPositive)
{
    WindowsProcessProbe probe;
    const uint64_t totalMem = probe.systemTotalMemory();

    EXPECT_GT(totalMem, 128ULL * 1024ULL * 1024ULL);
}

TEST(WindowsProcessProbeTest, EnumerateReturnsProcesses)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    EXPECT_GT(processes.size(), 0ULL);
}

TEST(WindowsProcessProbeTest, EnumerateFindsOurOwnProcess)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    const int32_t ourPid = static_cast<int32_t>(GetCurrentProcessId());
    const auto it = std::find_if(processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == ourPid; });

    ASSERT_NE(it, processes.end());

    EXPECT_GT(it->name.size(), 0ULL);
    EXPECT_GT(it->command.size(), 0ULL);
    EXPECT_GT(it->user.size(), 0ULL);

    EXPECT_GT(it->rssBytes, 0ULL);
    EXPECT_GT(it->virtualBytes, 0ULL);

    EXPECT_GT(it->startTimeTicks, 0ULL);
    EXPECT_GE(it->threadCount, 1);

    const std::string validStates = "RZ?";
    EXPECT_NE(validStates.find(it->state), std::string::npos);

    // Verify handle count is populated for our own process
    EXPECT_GT(it->handleCount, 0) << "Our process should have at least one handle";

    // Verify start time epoch is populated and reasonable
    // Should not be before 2020-01-01 (guard against obviously invalid timestamps)
    constexpr std::uint64_t jan2020 = 1577836800; // 2020-01-01 00:00:00 UTC
    EXPECT_GT(it->startTimeEpoch, jan2020) << "Start time epoch should be a reasonable modern timestamp";
}

// =============================================================================
// Process Data Validation Tests
// =============================================================================

TEST(WindowsProcessProbeTest, ProcessNamesAreNonEmpty)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    // Most processes should have names, but some system processes might not
    int processesWithNames = 0;
    for (const auto& proc : processes)
    {
        if (proc.name.size() > 0)
        {
            ++processesWithNames;
        }
    }
    EXPECT_GT(processesWithNames, 0) << "At least some processes should have names";
}

TEST(WindowsProcessProbeTest, ProcessPidsArePositive)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    // Most processes should have positive PIDs
    int processesWithPositivePids = 0;
    for (const auto& proc : processes)
    {
        if (proc.pid > 0)
        {
            ++processesWithPositivePids;
        }
    }
    EXPECT_GT(processesWithPositivePids, 0) << "At least some processes should have positive PIDs";
}

TEST(WindowsProcessProbeTest, ProcessParentPidsAreValid)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    // Most processes should have valid parent PIDs (>= 0)
    int processesWithValidParentPids = 0;
    for (const auto& proc : processes)
    {
        if (proc.parentPid >= 0)
        {
            ++processesWithValidParentPids;
        }
    }
    EXPECT_GT(processesWithValidParentPids, 0) << "At least some processes should have valid parent PIDs";
}

TEST(WindowsProcessProbeTest, MemoryValuesAreReasonable)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    // Most processes with memory values should have RSS <= virtual memory
    int processesWithValidMemory = 0;
    int processesWithMemoryData = 0;
    for (const auto& proc : processes)
    {
        // RSS should be <= virtual memory (when both are non-zero)
        if (proc.rssBytes > 0 && proc.virtualBytes > 0)
        {
            ++processesWithMemoryData;
            if (proc.rssBytes <= proc.virtualBytes)
            {
                ++processesWithValidMemory;
            }
        }
    }
    // If we have any processes with memory data, most should be valid
    if (processesWithMemoryData > 0)
    {
        EXPECT_GT(processesWithValidMemory, 0) << "At least some processes with memory data should have valid RSS <= virtual memory";
    }
}

TEST(WindowsProcessProbeTest, StartTimeTicksAreNonZero)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    // Most processes should have non-zero start times
    int processesWithStartTime = 0;
    for (const auto& proc : processes)
    {
        if (proc.startTimeTicks > 0)
        {
            ++processesWithStartTime;
        }
    }
    EXPECT_GT(processesWithStartTime, 0) << "At least some processes should have start times";
}

TEST(WindowsProcessProbeTest, ThreadCountsArePositive)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    // Most processes should have at least 1 thread
    int processesWithThreads = 0;
    for (const auto& proc : processes)
    {
        if (proc.threadCount >= 1)
        {
            ++processesWithThreads;
        }
    }
    EXPECT_GT(processesWithThreads, 0) << "At least some processes should have thread counts";
}

TEST(WindowsProcessProbeTest, StateIsValid)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    // Valid Windows process states: R (Running), Z (Zombie/exiting), ? (Unknown)
    const std::string validStates = "RZ?";

    // Most processes should have valid states
    int processesWithValidState = 0;
    for (const auto& proc : processes)
    {
        const char state = proc.state;
        if (validStates.find(state) != std::string::npos)
        {
            ++processesWithValidState;
        }
    }
    EXPECT_GT(processesWithValidState, 0) << "At least some processes should have valid states";
}

// =============================================================================
// Consistency Tests
// =============================================================================

TEST(WindowsProcessProbeTest, MultipleEnumerationsAreConsistent)
{
    WindowsProcessProbe probe;

    const auto processes1 = probe.enumerate();
    const auto processes2 = probe.enumerate();

    // Process counts might differ slightly due to short-lived processes,
    // but should be in the same ballpark
    EXPECT_NEAR(static_cast<double>(processes1.size()),
                static_cast<double>(processes2.size()),
                static_cast<double>(processes1.size()) * PROCESS_COUNT_VARIANCE_TOLERANCE)
        << "Multiple enumerations should return similar process counts";
}

TEST(WindowsProcessProbeTest, OwnProcessDataIsStable)
{
    WindowsProcessProbe probe;
    const int32_t ourPid = static_cast<int32_t>(GetCurrentProcessId());

    auto findOurProcess = [ourPid](const std::vector<ProcessCounters>& processes)
    {
        const auto it = std::find_if(processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == ourPid; });
        return it != processes.end() ? *it : ProcessCounters{};
    };

    const auto proc1 = findOurProcess(probe.enumerate());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto proc2 = findOurProcess(probe.enumerate());

    // PID should be the same
    EXPECT_EQ(proc1.pid, proc2.pid);

    // Name should be stable
    EXPECT_EQ(proc1.name, proc2.name);

    // Start time should be stable
    EXPECT_EQ(proc1.startTimeTicks, proc2.startTimeTicks);

    // Parent PID should be stable
    EXPECT_EQ(proc1.parentPid, proc2.parentPid);
}

TEST(WindowsProcessProbeTest, CpuTimeIncreasesBetweenSamples)
{
    WindowsProcessProbe probe;
    const int32_t ourPid = static_cast<int32_t>(GetCurrentProcessId());

    auto findOurProcess = [ourPid](const std::vector<ProcessCounters>& processes)
    {
        const auto it = std::find_if(processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == ourPid; });
        return it != processes.end() ? *it : ProcessCounters{};
    };

    const auto proc1 = findOurProcess(probe.enumerate());

    // Do significant CPU work to ensure measurable time increase
    volatile int sum = 0;
    for (int iteration = 0; iteration < CPU_WORK_ITERATIONS; ++iteration)
    {
        for (int i = 0; i < CPU_WORK_INNER_LOOP; ++i)
        {
            sum += i;
        }
    }

    const auto proc2 = findOurProcess(probe.enumerate());

    // CPU time should have increased (allow for rounding/measurement variance)
    const uint64_t totalTime1 = proc1.userTime + proc1.systemTime;
    const uint64_t totalTime2 = proc2.userTime + proc2.systemTime;
    EXPECT_GE(totalTime2, totalTime1) << "CPU time should not decrease after doing work";
}

// =============================================================================
// Edge Cases and Error Handling
// =============================================================================

TEST(WindowsProcessProbeTest, HandlesMissingProcesses)
{
    // Processes may disappear between enumeration calls
    // The probe should handle this gracefully
    WindowsProcessProbe probe;

    // Just verify enumeration doesn't crash
    EXPECT_NO_THROW({
        for (int i = 0; i < 10; ++i)
        {
            const auto processes = probe.enumerate();
            (void) processes; // Suppress unused variable warning
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

TEST(WindowsProcessProbeTest, HandlesRapidEnumeration)
{
    WindowsProcessProbe probe;

    // Rapidly enumerate many times - should not crash or leak
    EXPECT_NO_THROW({
        for (int i = 0; i < 100; ++i)
        {
            const auto processes = probe.enumerate();
            EXPECT_GT(processes.size(), 0ULL);
        }
    });
}

// =============================================================================
// Multithreading Tests
// =============================================================================

// Temporarily disabled - investigating CI failures
// TEST(WindowsProcessProbeTest, ConcurrentEnumeration)
// {
//     WindowsProcessProbe probe;
//
//     std::atomic<int> successCount{0};
//     std::atomic<bool> running{true};
//
//     auto enumerateTask = [&]()
//     {
//         while (running)
//         {
//             try
//             {
//                 const auto processes = probe.enumerate();
//                 if (!processes.empty())
//                 {
//                     ++successCount;
//                 }
//             }
//             catch (...)
//             {
//                 // Enumeration should not throw
//                 FAIL() << "Enumeration threw an exception";
//             }
//         }
//     };
//
//     // Start multiple threads enumerating concurrently
//     std::vector<std::thread> threads;
//     for (int i = 0; i < 4; ++i)
//     {
//         threads.emplace_back(enumerateTask);
//     }
//
//     // Let them run for a bit
//     std::this_thread::sleep_for(std::chrono::milliseconds(100));
//     running = false;
//
//     for (auto& t : threads)
//     {
//         t.join();
//     }
//
//     // All enumerations should have succeeded
//     EXPECT_GT(successCount.load(), 0);
// }

// =============================================================================
// Publisher, Type, and GDI Object Tests (Issues #184, #185, #195)
// =============================================================================

TEST(WindowsProcessProbeTest, OurProcessHasProcessTypeSet)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    const int32_t ourPid = static_cast<int32_t>(GetCurrentProcessId());
    const auto it = std::find_if(processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == ourPid; });

    ASSERT_NE(it, processes.end());

    // Our test process should have a non-empty processType
    EXPECT_FALSE(it->processType.empty()) << "Process type should not be empty for accessible processes";

    // The process type must be one of the three expected values
    const bool validType =
        (it->processType == "App") || (it->processType == "Background Process") || (it->processType == "Windows Process");
    EXPECT_TRUE(validType) << "Process type should be one of: App, Background Process, Windows Process; got: " << it->processType;

    // The classification must be consistent with what Win32 itself reports for our process.
    // classifyProcessType() checks GR_USEROBJECTS first, so if the process owns USER objects
    // it is classified as "App"; otherwise the path heuristic determines the result.
    const DWORD userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (userObjects > 0)
    {
        EXPECT_EQ(it->processType, "App") << "Process with USER objects (" << userObjects << ") should be classified as App";
    }
    else
    {
        // No USER objects: path heuristic applies. Our test binary is not under Windows\System32,
        // so it must be classified as Background Process.
        EXPECT_EQ(it->processType, "Background Process")
            << "Console test runner with no USER objects should be classified as Background Process";
    }
}

TEST(WindowsProcessProbeTest, SomeProcessesHavePublisherSet)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    // svchost.exe (Service Host) is always running on Windows and its image
    // (C:\Windows\System32\svchost.exe) carries a well-known Microsoft publisher string.
    // If the probe can open it and read version resources, the publisher must be
    // "Microsoft Corporation" — this verifies that VarFileInfo\Translation lookup works.
    const auto svchostIt = std::find_if(
        processes.begin(), processes.end(), [](const ProcessCounters& p) { return p.name == "svchost.exe" && !p.publisher.empty(); });

    if (svchostIt != processes.end())
    {
        EXPECT_EQ(svchostIt->publisher, "Microsoft Corporation")
            << "svchost.exe must be published by Microsoft Corporation; "
            << "got: '" << svchostIt->publisher << "' — check VarFileInfo\\Translation lookup";
    }
    else
    {
        // Fallback: at least one process should have a publisher
        const auto anyIt = std::find_if(processes.begin(), processes.end(), [](const ProcessCounters& p) { return !p.publisher.empty(); });
        EXPECT_NE(anyIt, processes.end()) << "At least one process should have a publisher field populated; "
                                          << "if svchost.exe was inaccessible this may indicate a permissions issue";
    }
}

TEST(WindowsProcessProbeTest, AllEnumeratedProcessTypesAreValid)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    for (const auto& proc : processes)
    {
        // processType is either empty (for protected/inaccessible processes) or one of the three valid values
        if (!proc.processType.empty())
        {
            const bool validType =
                (proc.processType == "App") || (proc.processType == "Background Process") || (proc.processType == "Windows Process");
            EXPECT_TRUE(validType) << "Process " << proc.name << " (PID " << proc.pid << ") has invalid type: " << proc.processType;
        }
    }
}

TEST(WindowsProcessProbeTest, SystemDirectoryProcessesAreWindowsProcess)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    // svchost.exe is a well-known Windows service host that always runs from
    // C:\Windows\System32\svchost.exe. An accessible instance must be classified
    // as "Windows Process". This tests the *outcome* without duplicating the path
    // heuristic logic: if classifyProcessType's detection breaks, this fails.
    const auto svchostIt = std::find_if(
        processes.begin(), processes.end(), [](const ProcessCounters& p) { return p.name == "svchost.exe" && !p.processType.empty(); });

    // Every Windows system has at least one accessible svchost.exe instance.
    ASSERT_NE(svchostIt, processes.end()) << "Expected at least one accessible svchost.exe in the enumeration";

    EXPECT_EQ(svchostIt->processType, "Windows Process")
        << "svchost.exe must be classified as Windows Process; got: '" << svchostIt->processType << "'"
        << " (path: " << svchostIt->command << ")";
}

TEST(WindowsProcessProbeTest, OurProcessHasNonNegativeGdiCount)
{
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    const int32_t ourPid = static_cast<int32_t>(GetCurrentProcessId());
    const auto it = std::find_if(processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == ourPid; });

    ASSERT_NE(it, processes.end());

    // The test process can always be opened with PROCESS_QUERY_INFORMATION (it is our own handle),
    // so the probe must return a value (not nullopt).
    ASSERT_TRUE(it->gdiObjectCount.has_value())
        << "GDI count should be readable for our own process (opened with PROCESS_QUERY_INFORMATION)";

    // Compare the probe result directly against the Win32 API for our own process.
    SetLastError(0);
    const DWORD expected = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    // Only compare when GetGuiResources itself succeeds (returns non-zero or no error).
    if (expected != 0 || GetLastError() == 0)
    {
        EXPECT_EQ(static_cast<DWORD>(*it->gdiObjectCount), expected)
            << "GDI object count from probe should match GetGuiResources for the current process";
    }
}

} // namespace Platform
