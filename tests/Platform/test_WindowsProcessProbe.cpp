/// @file test_WindowsProcessProbe.cpp
/// @brief Integration tests for Platform::WindowsProcessProbe
///
/// The pure helpers (WindowsProcessProbeMath.h) are tested on every platform in
/// WindowsMath/test_WindowsProcessProbeMath.cpp; the tests here need the real probe or
/// Windows types (the MIB_TCPROW conversions in WindowsTcpRows.h).

#include "Platform/ProcessTypes.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"
#include "Platform/Windows/WindowsProcessProbe.h"
#include "Platform/Windows/WindowsProcessProbeMath.h"
#include "Platform/Windows/WindowsTcpRows.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
    // SYSTEM_PROCESS_INFORMATION's PageFaultCount is a 32-bit ULONG that wraps (#1184).
    EXPECT_EQ(caps.pageFaultCountBits, 32U);

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
        EXPECT_EQ(probe.readSocketTraffic().sampleTimeNs, 0U); // no reading, not "every connection closed"
    }
    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasNetworkCounters);
    EXPECT_TRUE(caps.hasReducedPrivileges);

    // ...and those 0 B are not readings (#1285): every process's network bytes are unavailable.
    for (const auto& proc : probe.enumerate())
    {
        EXPECT_FALSE(proc.networkCountersAvailable) << proc.name;
    }
}

TEST(WindowsProcessProbeTest, NetworkCountersAreUnavailableForEveryProcessExactlyWhenTheyAreOff)
{
    // #1285: per-process network bytes are read for every process (EStats on, elevated) or for none;
    // the per-process flag follows the capability, whichever way this machine and token go.
    WindowsProcessProbe probe;
    for (int sample = 0; sample < 3; ++sample)
    {
        (void) probe.enumerate();
        (void) probe.readSocketTraffic(); // may revoke EStats (#1161); the next enumerate() follows
    }
    const auto processes = probe.enumerate();
    const bool hasNetworkCounters = probe.capabilities().hasNetworkCounters;
    ASSERT_FALSE(processes.empty());
    for (const auto& proc : processes)
    {
        EXPECT_EQ(proc.networkCountersAvailable, hasNetworkCounters) << proc.name << " (PID " << proc.pid << ")";
    }
}

TEST(WindowsProcessProbeTest, HandlesAndIoAreReadEvenForProcessesItCannotOpen)
{
    // #1285: handle counts and I/O bytes come from the SystemProcessInformation snapshot, which needs
    // no access to the process, so a process the probe can't open (protected, or another user's
    // without elevation) still has real readings -- the System process always owns handles.
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();

    bool sawSystem = false;
    int unopenableWithHandles = 0;
    for (const auto& proc : processes)
    {
        EXPECT_TRUE(proc.handleCountAvailable) << proc.name;
        EXPECT_TRUE(proc.ioCountersAvailable) << proc.name;
        if (proc.pid == 4)
        {
            sawSystem = true;
            EXPECT_GT(proc.handleCount, 0) << "System process";
        }
        if (proc.pid <= 4)
        {
            continue; // Idle has no handle table; System is checked above
        }
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(proc.pid));
        if (hProcess != nullptr)
        {
            CloseHandle(hProcess);
            continue;
        }
        // Minimal processes (Secure System, Registry, Memory Compression) really own no handles.
        if (proc.handleCount > 0)
        {
            ++unopenableWithHandles;
        }
    }
    EXPECT_TRUE(sawSystem);
    // Protected processes (csrss.exe, smss.exe, ...) refuse PROCESS_QUERY_INFORMATION even elevated,
    // and still report their handles.
    EXPECT_GT(unopenableWithHandles, 0) << "expected a protected process with a real handle count";
}

TEST(WindowsProcessProbeTest, NetworkFlagsStayConsistentAfterSampling)
{
    // The first real samples may revoke EStats availability (#1161); whichever way they go, the
    // capability invariant must still hold afterwards, and elevated never shows the lock icon.
    WindowsProcessProbe probe;
    for (int sample = 0; sample < 3; ++sample)
    {
        (void) probe.enumerate();
        (void) probe.readSocketTraffic(); // the EStats walk, which classifies availability (#1256)
    }
    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasNetworkCounters && caps.hasReducedPrivileges);
    // Blocked (#1358) is the elevated counterpart of reduced privileges: never with either flag.
    EXPECT_FALSE(caps.networkCountersBlocked && caps.hasNetworkCounters);
    EXPECT_FALSE(caps.networkCountersBlocked && caps.hasReducedPrivileges);
    if (isTestProcessElevated())
    {
        EXPECT_FALSE(caps.hasReducedPrivileges);
    }
    else
    {
        EXPECT_FALSE(caps.networkCountersBlocked) << "non-elevated, a denial is reduced privileges";
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

TEST(WindowsProcessProbeTest, StateComesFromThreadStates)
{
    // #1156: every live process used to read "R". The state now comes from the snapshot's thread
    // states, for every process: the one enumerating runs (R) and the System Idle Process is Idle
    // (I). Windows never reports Z. How many of the rest wait depends on the host's load, so the
    // R/S/T mapping itself is tested on fixed tallies (DeriveProcessStateTest).
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();
    ASSERT_FALSE(processes.empty());

    const std::string validStates = "RSTI?";
    const auto ourPid = static_cast<std::int32_t>(GetCurrentProcessId());
    for (const auto& proc : processes)
    {
        EXPECT_NE(validStates.find(proc.state), std::string::npos) << proc.name << " has state '" << proc.state << "'";
        if (proc.pid == 0)
        {
            EXPECT_EQ(proc.state, 'I') << "System Idle Process";
        }
        if (proc.pid == ourPid)
        {
            EXPECT_EQ(proc.state, 'R') << "this thread was running while it enumerated";
        }
    }
}

TEST(WindowsProcessProbeTest, OurCommandIsTheCommandLineNotTheImagePath)
{
    // #1156: Command was the image path, so filtering by arguments worked only on Linux.
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();
    const auto ourPid = static_cast<std::int32_t>(GetCurrentProcessId());
    const auto it = std::ranges::find_if(processes, [ourPid](const ProcessCounters& p) { return p.pid == ourPid; });
    ASSERT_NE(it, processes.end());
    EXPECT_EQ(it->command, WinString::wideToUtf8(GetCommandLineW()));
}

TEST(WindowsProcessProbeTest, PriorityChangeShowsOnTheNextSample)
{
    // #1156: the priority class was read on the heavy TTL (4-15 s), so the badge kept the old class
    // for seconds after Set Priority. A base-priority change in the snapshot now re-reads it at once.
    WindowsProcessProbe probe;
    const auto ourPid = static_cast<std::int32_t>(GetCurrentProcessId());
    const auto ourNice = [&probe, ourPid] -> std::optional<std::int32_t>
    {
        const auto processes = probe.enumerate();
        const auto it = std::ranges::find_if(processes, [ourPid](const ProcessCounters& p) { return p.pid == ourPid; });
        return it != processes.end() ? std::optional<std::int32_t>(it->nice) : std::nullopt;
    };

    const DWORD originalClass = GetPriorityClass(GetCurrentProcess());
    ASSERT_NE(originalClass, 0U);
    const DWORD otherClass = (originalClass == BELOW_NORMAL_PRIORITY_CLASS) ? NORMAL_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS;

    EXPECT_EQ(ourNice(), priorityClassToNice(originalClass)); // First sample: cached for a heavy TTL
    ASSERT_NE(SetPriorityClass(GetCurrentProcess(), otherClass), FALSE);
    const auto changed = ourNice();
    const BOOL restored = SetPriorityClass(GetCurrentProcess(), originalClass);
    EXPECT_EQ(changed, priorityClassToNice(otherClass));
    ASSERT_NE(restored, FALSE);
    EXPECT_EQ(ourNice(), priorityClassToNice(originalClass));
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

    // The test process can always be opened (it is our own process), so the probe must return a
    // value (not nullopt).
    ASSERT_TRUE(it->gdiObjectCount.has_value()) << "GDI count should be readable for our own process";

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

// estatsConnectionKey (#1256) from real owner-PID rows; its pure tests are in
// WindowsMath/test_WindowsProcessProbeMath.cpp.
TEST(EStatsConnectionKeyTest, OwnerRowsGiveTheSameKeyEveryRead)
{
    MIB_TCPROW_OWNER_PID owner{};
    owner.dwState = MIB_TCP_STATE_ESTAB;
    owner.dwLocalAddr = 0x0100007FU;
    owner.dwLocalPort = 0x0000BB01U;
    owner.dwRemoteAddr = 0x0A01A8C0U;
    owner.dwRemotePort = 0x0000D2C3U;
    MIB_TCPROW_OWNER_PID nextRead = owner;
    nextRead.dwLocalPort |= 0xABCD0000U; // undefined upper bits differ between reads
    nextRead.dwOwningPid = 99;           // the key is the connection's, not its owner's
    EXPECT_EQ(estatsConnectionKey(toConnectionEndpoints(owner)), estatsConnectionKey(toConnectionEndpoints(nextRead)));

    const TcpConnectionEndpoints endpoints = toConnectionEndpoints(owner);
    EXPECT_EQ(endpoints.family, TcpAddressFamily::IPv4);
    EXPECT_EQ(endpoints.localAddr[0], 127U);
    EXPECT_EQ(endpoints.localAddr[3], 1U);
    EXPECT_EQ(endpoints.remoteAddr[0], 192U);
    EXPECT_EQ(endpoints.remoteAddr[3], 10U);

    MIB_TCP6ROW_OWNER_PID owner6{};
    constexpr std::array<UCHAR, 16> LOCAL{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    std::memcpy(owner6.ucLocalAddr, LOCAL.data(), LOCAL.size());
    owner6.dwRemoteScopeId = 12;
    const TcpConnectionEndpoints endpoints6 = toConnectionEndpoints(owner6);
    EXPECT_EQ(endpoints6.family, TcpAddressFamily::IPv6);
    EXPECT_EQ(endpoints6.localAddr[0], 0x20U);
    EXPECT_EQ(endpoints6.localAddr[15], 0x01U);
    EXPECT_EQ(endpoints6.remoteScopeId, 12U);
}

TEST(WindowsProcessProbeTest, EnumerateLeavesNetworkCountersToTheSocketReading)
{
    // Per-process network bytes come from readSocketTraffic() through Domain's accumulator (#1256);
    // enumerate() no longer writes a sum over live connections.
    WindowsProcessProbe probe;
    for (const auto& proc : probe.enumerate())
    {
        EXPECT_EQ(proc.netSentBytes, 0ULL) << proc.name;
        EXPECT_EQ(proc.netReceivedBytes, 0ULL) << proc.name;
    }
    const auto reading = probe.readSocketTraffic();
    if (!probe.capabilities().hasNetworkCounters)
    {
        EXPECT_EQ(reading.sampleTimeNs, 0U);
        EXPECT_TRUE(reading.sockets.empty());
    }
    for (const auto& socket : reading.sockets)
    {
        EXPECT_NE(socket.key, 0U);
    }
}

} // namespace
} // namespace Platform
