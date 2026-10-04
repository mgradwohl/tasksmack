/// @file test_WindowsProcessProbe.cpp
/// @brief Integration tests for Platform::WindowsProcessProbe

#include "Platform/ProcessTypes.h"
#include "Platform/Windows/WindowsProcessProbe.h"
#include "Platform/Windows/WindowsProcessProbeMath.h"
#include "Platform/Windows/WindowsTcpRows.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
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

TEST(WindowsProcessProbeTest, PowerAndSharedMemoryAreNotClaimedAndNoEnergyIsInvented)
{
    // Power used to be "available" whenever AC status was known, and was a fabricated 1 J per
    // sample shared out by CPU time (#1028). Shared memory was never filled (#1035). Neither is
    // claimed now, and no process carries energy.
    WindowsProcessProbe probe;
    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasPowerUsage);
    EXPECT_FALSE(caps.hasSharedMemory);

    for (int sample = 0; sample < 2; ++sample)
    {
        for (const auto& proc : probe.enumerate())
        {
            EXPECT_EQ(proc.energyMicrojoules, 0ULL) << proc.name;
        }
    }
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

namespace
{
bool isTestProcessElevated()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
    {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}
} // namespace

TEST(WindowsProcessProbeTest, NonElevatedNeverClaimsNetworkCounters)
{
    // #1161: non-elevated, the old dummy-row probe could get ERROR_NOT_FOUND and claim per-process
    // network counters, then report 0 B for every process with no lock icon. EStats now requires
    // an elevated token: non-elevated reports the counters unavailable for privilege.
    if (isTestProcessElevated())
    {
        GTEST_SKIP() << "Test process is elevated; the non-elevated path cannot run here";
    }

    WindowsProcessProbe probe;
    for (int sample = 0; sample < 2; ++sample)
    {
        for (const auto& proc : probe.enumerate())
        {
            EXPECT_EQ(proc.netSentBytes, 0ULL) << proc.name;
            EXPECT_EQ(proc.netReceivedBytes, 0ULL) << proc.name;
        }
    }
    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasNetworkCounters);
    EXPECT_TRUE(caps.hasReducedPrivileges);
}

TEST(WindowsProcessProbeTest, NetworkFlagsStayConsistentAfterSampling)
{
    // The first real samples may revoke EStats availability (#1161); whichever way they go, the
    // capability invariant must still hold afterwards, and elevated never shows the lock icon.
    WindowsProcessProbe probe;
    for (int sample = 0; sample < 3; ++sample)
    {
        (void) probe.enumerate();
    }
    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasNetworkCounters && caps.hasReducedPrivileges);
    if (isTestProcessElevated())
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

namespace
{

// ==========================================================================
// calculateDetailTTLsFromTotalRAMBytes: pure RAM-tier logic, no OS calls
// required. This machine's actual RAM only ever exercises one tier via the
// real GlobalMemoryStatusEx()-backed member function, so these fabricated
// byte counts are the only way to reach the other four tiers.
// ==========================================================================

constexpr std::uint64_t GIB = 1024ULL * 1024 * 1024;

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, BelowTwoGibUsesMostAggressiveCaching)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(GIB); // 1 GiB
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(4000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(15000));
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, TwoToFourGibUsesConservativeTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(2 * GIB);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(3000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(10000));
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, FourToEightGibUsesBalancedTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(4 * GIB);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(2000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(8000));
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, EightToSixteenGibUsesModerateTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(8 * GIB);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(1500));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(6000));
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, SixteenGibAndAboveUsesMostResponsiveTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(16 * GIB);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(1000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(4000));

    // Well above the top tier threshold should stay on the same (top) tier.
    const auto ttlsHuge = calculateDetailTTLsFromTotalRAMBytes(256 * GIB);
    EXPECT_EQ(ttlsHuge.light, ttls.light);
    EXPECT_EQ(ttlsHuge.heavy, ttls.heavy);
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, ZeroBytesFallsIntoLowestTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(0);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(4000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(15000));
}

// ---------------------------------------------------------------------------
// accumulateEStatsRow (#1100): the per-row decision shared by the IPv4 and IPv6 EStats walks
// ---------------------------------------------------------------------------

TEST(AccumulateEStatsRowTest, Ipv4AndIpv6RowsForTheSamePidSum)
{
    // Before #1100 only the AF_INET table was walked, so a process whose traffic was all IPv6
    // (most browser/CDN traffic on a dual-stack network) read ~0. Both walks now feed the same
    // per-PID map through this helper, so a v4 row and a v6 row for one PID must add up.
    PerPidNetworkBytes perPid;
    constexpr std::uint32_t PID = 4242;

    // IPv4 row
    EXPECT_EQ(accumulateEStatsRow(perPid, PID, TCP_STATE_ESTABLISHED, 0, 1'000, 2'000), EStatsRowOutcome::Accumulated);
    // IPv6 row (same helper, same map)
    EXPECT_EQ(accumulateEStatsRow(perPid, PID, TCP_STATE_ESTABLISHED, 0, 30'000, 40'000), EStatsRowOutcome::Accumulated);
    // Another process
    EXPECT_EQ(accumulateEStatsRow(perPid, 7, TCP_STATE_ESTABLISHED, 0, 5, 6), EStatsRowOutcome::Accumulated);

    ASSERT_EQ(perPid.size(), 2U);
    EXPECT_EQ(perPid.at(PID).first, 31'000ULL);
    EXPECT_EQ(perPid.at(PID).second, 42'000ULL);
    EXPECT_EQ(perPid.at(7).first, 5ULL);
    EXPECT_EQ(perPid.at(7).second, 6ULL);
}

TEST(AccumulateEStatsRowTest, NonEstablishedRowsAreSkipped)
{
    PerPidNetworkBytes perPid;
    constexpr std::uint32_t LISTEN = 2;
    constexpr std::uint32_t TIME_WAIT = 11;

    EXPECT_EQ(accumulateEStatsRow(perPid, 1, LISTEN, 0, 100, 100), EStatsRowOutcome::SkippedState);
    EXPECT_EQ(accumulateEStatsRow(perPid, 1, TIME_WAIT, 0, 100, 100), EStatsRowOutcome::SkippedState);
    EXPECT_TRUE(perPid.empty());
}

TEST(AccumulateEStatsRowTest, FailedReadsAreSkipped)
{
    PerPidNetworkBytes perPid;
    constexpr std::uint32_t ERROR_NOT_FOUND_CODE = 1168;
    constexpr std::uint32_t ERROR_ACCESS_DENIED_CODE = 5;

    EXPECT_EQ(accumulateEStatsRow(perPid, 1, TCP_STATE_ESTABLISHED, ERROR_NOT_FOUND_CODE, 100, 100), EStatsRowOutcome::ReadFailed);
    EXPECT_EQ(accumulateEStatsRow(perPid, 1, TCP_STATE_ESTABLISHED, ERROR_ACCESS_DENIED_CODE, 100, 100), EStatsRowOutcome::ReadFailed);
    // A failed read must not even create a zero entry for the PID.
    EXPECT_TRUE(perPid.empty());
}

TEST(AccumulateEStatsRowTest, CountersAboveOneTerabyteAreRejected)
{
    PerPidNetworkBytes perPid;
    constexpr std::uint32_t PID = 9;

    EXPECT_EQ(accumulateEStatsRow(perPid, PID, TCP_STATE_ESTABLISHED, 0, MAX_SANE_ESTATS_CONNECTION_BYTES + 1, 0),
              EStatsRowOutcome::Garbage);
    EXPECT_EQ(accumulateEStatsRow(perPid, PID, TCP_STATE_ESTABLISHED, 0, 0, MAX_SANE_ESTATS_CONNECTION_BYTES + 1),
              EStatsRowOutcome::Garbage);
    EXPECT_TRUE(perPid.empty());

    // Exactly 1 TB is still accepted (the cap is exclusive).
    EXPECT_EQ(accumulateEStatsRow(perPid, PID, TCP_STATE_ESTABLISHED, 0, MAX_SANE_ESTATS_CONNECTION_BYTES, 0),
              EStatsRowOutcome::Accumulated);
    EXPECT_EQ(perPid.at(PID).first, MAX_SANE_ESTATS_CONNECTION_BYTES);
}

TEST(AccumulateEStatsRowTest, ZeroByteEstablishedRowStillRegistersThePid)
{
    // A just-opened connection reads OK with zero bytes; it is accumulated (the PID has a
    // network presence at 0 B), matching the pre-#1100 IPv4 loop.
    PerPidNetworkBytes perPid;
    EXPECT_EQ(accumulateEStatsRow(perPid, 3, TCP_STATE_ESTABLISHED, 0, 0, 0), EStatsRowOutcome::Accumulated);
    ASSERT_EQ(perPid.count(3), 1U);
    EXPECT_EQ(perPid.at(3).first, 0ULL);
}

TEST(EStatsSampleCountsTest, Ipv4AndIpv6TalliesAdd)
{
    EStatsSampleCounts v4{
        .total = 10,
        .established = 4,
        .enabled = 4,
        .readOk = 3,
        .saneReads = 2,
        .readNotFound = 1,
        .readFailedOther = 0,
        .accessDenied = 0,
        .hasData = 2,
        .garbage = 1,
    };
    const EStatsSampleCounts v6{
        .total = 5,
        .established = 2,
        .enabled = 1,
        .readOk = 2,
        .saneReads = 2,
        .readNotFound = 0,
        .readFailedOther = 1,
        .accessDenied = 1,
        .hasData = 1,
        .garbage = 0,
    };
    v4 += v6;
    EXPECT_EQ(v4.total, 15U);
    EXPECT_EQ(v4.established, 6U);
    EXPECT_EQ(v4.enabled, 5U);
    EXPECT_EQ(v4.readOk, 5U);
    EXPECT_EQ(v4.saneReads, 4U);
    EXPECT_EQ(v4.readNotFound, 1U);
    EXPECT_EQ(v4.readFailedOther, 1U);
    EXPECT_EQ(v4.accessDenied, 1U);
    EXPECT_EQ(v4.hasData, 3U);
    EXPECT_EQ(v4.garbage, 1U);
}

TEST(RecordEStatsRowTest, TalliesEachOutcome)
{
    // The real per-row tally both table walks use (#1161): NOT_FOUND is counted apart from other
    // read failures, and a garbage read is a successful read but not a sane one.
    PerPidNetworkBytes perPid;
    EStatsSampleCounts counts;
    constexpr std::uint32_t LISTEN = 2;
    constexpr std::uint64_t TOO_BIG = MAX_SANE_ESTATS_CONNECTION_BYTES + 1;

    (void) recordEStatsRow(counts, perPid, 1, LISTEN, std::nullopt, NO_ERROR, 9, 9);             // not counted
    (void) recordEStatsRow(counts, perPid, 1, TCP_STATE_ESTABLISHED, NO_ERROR, NO_ERROR, 10, 0); // sane, has data
    (void) recordEStatsRow(counts, perPid, 1, TCP_STATE_ESTABLISHED, NO_ERROR, NO_ERROR, 0, 0);  // sane, no data
    (void) recordEStatsRow(counts, perPid, 2, TCP_STATE_ESTABLISHED, NO_ERROR, NO_ERROR, TOO_BIG, 0);
    (void) recordEStatsRow(counts, perPid, 3, TCP_STATE_ESTABLISHED, ERROR_NOT_FOUND, ERROR_NOT_FOUND, 0, 0);
    (void) recordEStatsRow(counts, perPid, 4, TCP_STATE_ESTABLISHED, std::nullopt, ERROR_INVALID_PARAMETER, 0, 0);
    (void) recordEStatsRow(counts, perPid, 5, TCP_STATE_ESTABLISHED, ERROR_ACCESS_DENIED, ERROR_ACCESS_DENIED, 0, 0);

    EXPECT_EQ(counts.established, 6U);
    EXPECT_EQ(counts.enabled, 3U);
    EXPECT_EQ(counts.readOk, 3U);
    EXPECT_EQ(counts.saneReads, 2U);
    EXPECT_EQ(counts.hasData, 1U);
    EXPECT_EQ(counts.garbage, 1U);
    EXPECT_EQ(counts.readNotFound, 1U);
    EXPECT_EQ(counts.readFailedOther, 1U); // ACCESS_DENIED is tallied as accessDenied, not here
    EXPECT_EQ(counts.accessDenied, 1U);
    ASSERT_EQ(perPid.size(), 1U);
    EXPECT_EQ(perPid.at(1).first, 10ULL);
}

// ---------------------------------------------------------------------------
// classifyEStatsProbe (#1161): does a real sample prove EStats works?
// ---------------------------------------------------------------------------

/// Replays a per-row (enableStatus, readStatus) error sequence for ESTABLISHED rows into the
/// tallies the probe's table walks produce, so each test reads as "the OS returned X, Y, Z".
struct EStatsRowResult
{
    DWORD enableStatus = NO_ERROR;
    DWORD readStatus = NO_ERROR;
    std::uint64_t bytesOut = 0;
    std::uint64_t bytesIn = 0;
};

EStatsSampleCounts tallyEstablishedRows(const std::vector<EStatsRowResult>& rows)
{
    EStatsSampleCounts counts;
    counts.total = rows.size();
    PerPidNetworkBytes perPid;
    std::uint32_t pid = 100;
    for (const auto& row : rows)
    {
        (void) recordEStatsRow(counts, perPid, pid++, TCP_STATE_ESTABLISHED, row.enableStatus, row.readStatus, row.bytesOut, row.bytesIn);
    }
    return counts;
}

TEST(ClassifyEStatsProbeTest, AllAccessDeniedIsUnavailable)
{
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ERROR_ACCESS_DENIED, .readStatus = ERROR_ACCESS_DENIED},
        {.enableStatus = ERROR_ACCESS_DENIED, .readStatus = ERROR_ACCESS_DENIED},
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Unavailable);
}

TEST(ClassifyEStatsProbeTest, AnyAccessDeniedIsUnavailableEvenIfSomeReadsWork)
{
    const auto counts = tallyEstablishedRows({
        {.enableStatus = NO_ERROR, .readStatus = NO_ERROR},
        {.enableStatus = ERROR_ACCESS_DENIED, .readStatus = ERROR_NOT_FOUND},
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Unavailable);
}

TEST(ClassifyEStatsProbeTest, DummyRowNotFoundThenEveryRealReadFailingIsUnavailable)
{
    // The #1161 case: the constructor's dummy-row probe returned ERROR_NOT_FOUND (which the old
    // detection treated as "available"), then every real established connection's Set/Get
    // fails without ever saying ACCESS_DENIED. The old code kept hasNetworkCounters = true and
    // showed 0 B for every process with no lock icon; a real sample now proves it unavailable.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ERROR_NOT_FOUND, .readStatus = ERROR_NOT_FOUND},
        {.enableStatus = ERROR_INVALID_PARAMETER, .readStatus = ERROR_NOT_FOUND},
        {.enableStatus = ERROR_NOT_FOUND, .readStatus = ERROR_INVALID_PARAMETER},
    });
    ASSERT_EQ(counts.accessDenied, 0U);
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Unavailable);
}

TEST(ClassifyEStatsProbeTest, ZeroEstablishedConnectionsIsUndetermined)
{
    // Nothing to read proves nothing either way: keep trying on the next sample rather than
    // declaring the feature dead on an idle machine.
    EXPECT_EQ(classifyEStatsProbe(EStatsSampleCounts{}), EStatsProbeResult::Undetermined);

    EStatsSampleCounts onlyListeners;
    onlyListeners.total = 40; // e.g. all LISTEN / TIME_WAIT rows
    EXPECT_EQ(classifyEStatsProbe(onlyListeners), EStatsProbeResult::Undetermined);
}

TEST(ClassifyEStatsProbeTest, SuccessfulReadsAreAvailable)
{
    const auto counts = tallyEstablishedRows({
        {.enableStatus = NO_ERROR, .readStatus = NO_ERROR},
        {.enableStatus = NO_ERROR, .readStatus = NO_ERROR},
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Available);
}

TEST(ClassifyEStatsProbeTest, ReadsWorkingWithoutEnableAreAvailable)
{
    // Collection may already have been enabled by another (elevated) process, so a failed
    // enable with a successful read still proves the counters are real.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ERROR_NOT_FOUND, .readStatus = NO_ERROR},
        {.enableStatus = ERROR_NOT_FOUND, .readStatus = ERROR_NOT_FOUND}, // connection closed mid-walk
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Available);
}

TEST(ClassifyEStatsProbeTest, OnlyNotFoundReadsAreUndetermined)
{
    // Every snapshotted connection closed before its EStats read: ERROR_NOT_FOUND for all of
    // them is an ordinary race, not proof the API is unusable. Before this fix one such sample
    // permanently disabled the network column.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ERROR_NOT_FOUND, .readStatus = ERROR_NOT_FOUND},
        {.enableStatus = ERROR_NOT_FOUND, .readStatus = ERROR_NOT_FOUND},
    });
    ASSERT_EQ(counts.readNotFound, 2U);
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Undetermined);
}

TEST(ClassifyEStatsProbeTest, NotFoundSampleThenSuccessfulSampleIsAvailable)
{
    const auto raced = tallyEstablishedRows({{.enableStatus = ERROR_NOT_FOUND, .readStatus = ERROR_NOT_FOUND}});
    ASSERT_EQ(classifyEStatsProbe(raced, 0), EStatsProbeResult::Undetermined);

    // The probe counted one inconclusive sample; the next one reads a live connection.
    const auto next = tallyEstablishedRows({{.enableStatus = NO_ERROR, .readStatus = NO_ERROR, .bytesOut = 512, .bytesIn = 2048}});
    EXPECT_EQ(classifyEStatsProbe(next, 1), EStatsProbeResult::Available);
}

TEST(ClassifyEStatsProbeTest, ReadAccessDeniedIsUnavailable)
{
    // ACCESS_DENIED from the read alone (enable succeeded or was skipped) is just as conclusive.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = NO_ERROR, .readStatus = ERROR_ACCESS_DENIED},
        {.enableStatus = ERROR_NOT_FOUND, .readStatus = ERROR_NOT_FOUND},
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Unavailable);
}

TEST(ClassifyEStatsProbeTest, GarbageOnlyReadsAreNotAvailable)
{
    // A > 1 TB counter is rejected and never reaches perPid, so a sample of only garbage reads
    // proves nothing; it used to count as a successful read and verify EStats with no data.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = NO_ERROR, .readStatus = NO_ERROR, .bytesOut = MAX_SANE_ESTATS_CONNECTION_BYTES + 1},
        {.enableStatus = NO_ERROR, .readStatus = NO_ERROR, .bytesIn = MAX_SANE_ESTATS_CONNECTION_BYTES + 1},
    });
    ASSERT_EQ(counts.readOk, 2U);
    ASSERT_EQ(counts.saneReads, 0U);
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Undetermined);
}

TEST(ClassifyEStatsProbeTest, InconclusiveSamplesInARowBecomeUnavailable)
{
    // A race does not repeat on every sample: after MAX_INCONCLUSIVE_ESTATS_SAMPLES consecutive
    // NOT_FOUND/garbage-only samples the reads plainly never work, so stop claiming the column.
    const auto notFound = tallyEstablishedRows({{.enableStatus = ERROR_NOT_FOUND, .readStatus = ERROR_NOT_FOUND}});
    const auto garbage = tallyEstablishedRows({{.readStatus = NO_ERROR, .bytesOut = MAX_SANE_ESTATS_CONNECTION_BYTES + 1}});
    for (std::size_t prior = 0; prior + 1 < MAX_INCONCLUSIVE_ESTATS_SAMPLES; ++prior)
    {
        EXPECT_EQ(classifyEStatsProbe(notFound, prior), EStatsProbeResult::Undetermined) << prior;
        EXPECT_EQ(classifyEStatsProbe(garbage, prior), EStatsProbeResult::Undetermined) << prior;
    }
    EXPECT_EQ(classifyEStatsProbe(notFound, MAX_INCONCLUSIVE_ESTATS_SAMPLES - 1), EStatsProbeResult::Unavailable);
    EXPECT_EQ(classifyEStatsProbe(garbage, MAX_INCONCLUSIVE_ESTATS_SAMPLES - 1), EStatsProbeResult::Unavailable);

    // An idle sample is never inconclusive in that sense, whatever the streak.
    EXPECT_EQ(classifyEStatsProbe(EStatsSampleCounts{}, MAX_INCONCLUSIVE_ESTATS_SAMPLES), EStatsProbeResult::Undetermined);
}

// ---------------------------------------------------------------------------
// toTcpRow / toTcp6Row (#1100): owner-PID table row -> the row EStats identifies a connection by
// ---------------------------------------------------------------------------

/// Decode a port the TCP tables store in network byte order in the low 16 bits of a DWORD.
constexpr std::uint16_t portFromNetworkOrder(DWORD raw)
{
    return static_cast<std::uint16_t>(((raw & 0xFFU) << 8U) | ((raw >> 8U) & 0xFFU));
}

TEST(TcpRowConversionTest, Ipv4OwnerRowMapsEveryField)
{
    MIB_TCPROW_OWNER_PID owner{};
    owner.dwState = MIB_TCP_STATE_ESTAB;
    owner.dwLocalAddr = 0x0100007FU;  // 127.0.0.1 in network byte order
    owner.dwLocalPort = 0x0000BB01U;  // 443 in network byte order
    owner.dwRemoteAddr = 0x0A01A8C0U; // 192.168.1.10 in network byte order
    owner.dwRemotePort = 0x0000D2C3U; // 50130 in network byte order
    owner.dwOwningPid = 4242;

    const MIB_TCPROW row = toTcpRow(owner);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - Windows API requires union access
    EXPECT_EQ(row.dwState, static_cast<DWORD>(MIB_TCP_STATE_ESTAB));
    EXPECT_EQ(row.dwLocalAddr, owner.dwLocalAddr);
    EXPECT_EQ(row.dwRemoteAddr, owner.dwRemoteAddr);
    // Ports are copied verbatim, still in network byte order (what EStats expects).
    EXPECT_EQ(row.dwLocalPort, owner.dwLocalPort);
    EXPECT_EQ(row.dwRemotePort, owner.dwRemotePort);
    EXPECT_EQ(portFromNetworkOrder(row.dwLocalPort), 443U);
    EXPECT_EQ(portFromNetworkOrder(row.dwRemotePort), 50130U);
}

TEST(TcpRowConversionTest, Ipv6OwnerRowMapsEveryField)
{
    MIB_TCP6ROW_OWNER_PID owner{};
    // 2001:db8::1 and fe80::abcd: distinct, asymmetric bytes so a swapped or truncated copy shows.
    constexpr std::array<UCHAR, 16> LOCAL{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    constexpr std::array<UCHAR, 16> REMOTE{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xab, 0xcd};
    std::memcpy(owner.ucLocalAddr, LOCAL.data(), LOCAL.size());
    std::memcpy(owner.ucRemoteAddr, REMOTE.data(), REMOTE.size());
    owner.dwLocalScopeId = 0;
    owner.dwRemoteScopeId = 12;       // link-local remote: interface index matters
    owner.dwLocalPort = 0x0000BB01U;  // 443 in network byte order
    owner.dwRemotePort = 0x0000D2C3U; // 50130 in network byte order
    owner.dwState = MIB_TCP_STATE_ESTAB;
    owner.dwOwningPid = 4242;

    const MIB_TCP6ROW row = toTcp6Row(owner);
    EXPECT_EQ(row.State, MIB_TCP_STATE_ESTAB);
    EXPECT_EQ(std::memcmp(&row.LocalAddr, LOCAL.data(), LOCAL.size()), 0);
    EXPECT_EQ(std::memcmp(&row.RemoteAddr, REMOTE.data(), REMOTE.size()), 0);
    EXPECT_EQ(row.dwLocalScopeId, 0U);
    EXPECT_EQ(row.dwRemoteScopeId, 12U);
    EXPECT_EQ(row.dwLocalPort, owner.dwLocalPort);
    EXPECT_EQ(row.dwRemotePort, owner.dwRemotePort);
    EXPECT_EQ(portFromNetworkOrder(row.dwLocalPort), 443U);
    EXPECT_EQ(portFromNetworkOrder(row.dwRemotePort), 50130U);
}

TEST(TcpRowConversionTest, Ipv6StateComesFromTheOwnerRow)
{
    // The state is mapped, not hard-coded to ESTABLISHED.
    MIB_TCP6ROW_OWNER_PID owner{};
    owner.dwState = MIB_TCP_STATE_TIME_WAIT;
    EXPECT_EQ(toTcp6Row(owner).State, MIB_TCP_STATE_TIME_WAIT);
}

} // namespace
} // namespace Platform
