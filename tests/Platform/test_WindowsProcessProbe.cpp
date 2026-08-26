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

TEST(WindowsProcessProbeTest, ReducedPrivilegesIsConsistent)
{
    // hasReducedPrivileges should be false when EStats is available (admin) or when
    // EStats failed for a non-privilege reason (unsupported API). It should only be
    // true when non-admin AND EStats was specifically denied due to admin requirement.
    WindowsProcessProbe probe;
    const auto caps = probe.capabilities();

    // Network counters and reduced-privileges are mutually exclusive:
    // if EStats is working we're admin, so there can be no privilege data gap.
    EXPECT_FALSE(caps.hasNetworkCounters && caps.hasReducedPrivileges);

    // Stronger check: if network counters are available, privilege notice must not fire.
    if (caps.hasNetworkCounters)
    {
        EXPECT_FALSE(caps.hasReducedPrivileges);
    }
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

TEST(WindowsProcessProbeTest, OwnProcessHasPerSampleCountersEverySample)
{
    // The bulk snapshot must deliver handle count, virtual memory, and I/O counters
    // on every enumerate() call — these are no longer TTL-cached. Force deterministic
    // changes between samples and verify the very next snapshot reflects them.
    WindowsProcessProbe probe;
    const int32_t ourPid = static_cast<int32_t>(GetCurrentProcessId());

    auto findOurProcess = [ourPid](const std::vector<ProcessCounters>& processes)
    {
        const auto it = std::find_if(processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == ourPid; });
        return it != processes.end() ? *it : ProcessCounters{};
    };

    const auto before = findOurProcess(probe.enumerate());
    EXPECT_EQ(before.pid, ourPid);
    EXPECT_GT(before.handleCount, 0);
    EXPECT_GT(before.virtualBytes, 0ULL);
    EXPECT_GT(before.rssBytes, 0ULL);
    EXPECT_GT(before.pageFaultCount, 0ULL);
    EXPECT_GT(before.threadCount, 0);

    // Open a batch of event handles and reserve a large virtual region. A TTL-cached
    // implementation would keep serving the stale pre-change values here. RAII guard
    // ensures cleanup even if an ASSERT aborts the test mid-setup.
    constexpr int EXTRA_HANDLES = 64;
    constexpr SIZE_T EXTRA_VIRTUAL_BYTES = 256ULL * 1024ULL * 1024ULL; // 256 MB reserve
    struct ScopedResources
    {
        std::vector<HANDLE> events;
        void* reservation = nullptr;

        ScopedResources() = default;
        ScopedResources(const ScopedResources&) = delete;
        ScopedResources& operator=(const ScopedResources&) = delete;
        ScopedResources(ScopedResources&&) = delete;
        ScopedResources& operator=(ScopedResources&&) = delete;
        ~ScopedResources()
        {
            if (reservation != nullptr)
            {
                VirtualFree(reservation, 0, MEM_RELEASE);
            }
            for (HANDLE event : events)
            {
                CloseHandle(event);
            }
        }
    };
    ScopedResources resources;
    resources.events.reserve(EXTRA_HANDLES);
    for (int i = 0; i < EXTRA_HANDLES; ++i)
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ASSERT_NE(event, nullptr);
        resources.events.push_back(event);
    }
    resources.reservation = VirtualAlloc(nullptr, EXTRA_VIRTUAL_BYTES, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_NE(resources.reservation, nullptr);

    const auto after = findOurProcess(probe.enumerate());

    // Allow slack for unrelated handle churn in the test process, but the next sample
    // must observe most of the new handles and the full reservation immediately.
    EXPECT_GE(after.handleCount, before.handleCount + (EXTRA_HANDLES / 2)) << "handle count must be refreshed every sample, not TTL-cached";
    EXPECT_GE(after.virtualBytes, before.virtualBytes + (EXTRA_VIRTUAL_BYTES / 2))
        << "virtual size must be refreshed every sample, not TTL-cached";
}

TEST(WindowsProcessProbeTest, EnumerateIncludesKernelPseudoProcesses)
{
    // The system snapshot always contains the Idle pseudo-process (PID 0) and the
    // System process (PID 4); both are inaccessible via OpenProcess but must still
    // be reported with stable names across samples (name cache / fallback path).
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    const auto idle = std::find_if(processes.begin(), processes.end(), [](const ProcessCounters& p) { return p.pid == 0; });
    ASSERT_NE(idle, processes.end());
    EXPECT_EQ(idle->name, "[System Process]");

    const auto system = std::find_if(processes.begin(), processes.end(), [](const ProcessCounters& p) { return p.pid == 4; });
    ASSERT_NE(system, processes.end());
    EXPECT_FALSE(system->name.empty());

    // A second sample must report identical names — the cached-name/fallback path
    // may not degrade or change once details refresh TTLs kick in.
    const auto second = probe.enumerate();
    const auto idle2 = std::find_if(second.begin(), second.end(), [](const ProcessCounters& p) { return p.pid == 0; });
    ASSERT_NE(idle2, second.end());
    EXPECT_EQ(idle2->name, idle->name);

    const auto system2 = std::find_if(second.begin(), second.end(), [](const ProcessCounters& p) { return p.pid == 4; });
    ASSERT_NE(system2, second.end());
    EXPECT_EQ(system2->name, system->name);
}

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

TEST(WindowsProcessProbeTest, ConcurrentEnumeration)
{
    std::atomic<int> successCount{0};
    std::atomic<bool> running{true};
    constexpr int TARGET_SUCCESSES = 4; // Ensure each thread completes at least one iteration

    auto enumerateTask = [&]()
    {
        WindowsProcessProbe probe;
        while (running)
        {
            try
            {
                const auto processes = probe.enumerate();
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

    // Wait until we reach the target success count or timeout (500ms to handle heavily-loaded CI agents)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline && successCount.load() < TARGET_SUCCESSES)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    running = false;

    for (auto& t : threads)
    {
        t.join();
    }

    // Each thread should have completed at least one successful enumeration
    EXPECT_GE(successCount.load(), TARGET_SUCCESSES)
        << "Expected at least " << TARGET_SUCCESSES << " successful enumerations, got " << successCount.load();
}

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

    // svchost.exe always runs from C:\Windows\System32\svchost.exe.
    // classifyProcessType checks USER objects before the path heuristic (so that
    // inbox apps like Notepad that live in System32 are correctly classified as
    // "App"). Some svchost.exe instances own USER objects (message-only windows)
    // and will therefore be classified as "App". The invariant we can reliably
    // assert is that no accessible svchost.exe is a "Background Process" —
    // the path heuristic must recognise it as a system binary.
    const auto svchostIt = std::find_if(
        processes.begin(), processes.end(), [](const ProcessCounters& p) { return p.name == "svchost.exe" && !p.processType.empty(); });

    // Every Windows system has at least one accessible svchost.exe instance.
    ASSERT_NE(svchostIt, processes.end()) << "Expected at least one accessible svchost.exe in the enumeration";

    const bool isSystemBinary = (svchostIt->processType == "Windows Process") || (svchostIt->processType == "App");
    EXPECT_TRUE(isSystemBinary) << "svchost.exe must be classified as 'Windows Process' or 'App', not 'Background Process'; "
                                << "got: '" << svchostIt->processType << "' (path: " << svchostIt->command << ")";
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
