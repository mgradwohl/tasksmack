/// @file test_WindowsProcessProbe.cpp
/// @brief Integration tests for Platform::WindowsProcessProbe

#include "Domain/SocketTrafficAccumulator.h"
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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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

TEST(WindowsProcessProbeTest, StateComesFromThreadStates)
{
    // #1156: every live process used to read "R". The state now comes from the snapshot's thread
    // states, for every process: most processes wait (S), the one enumerating runs (R), and the
    // System Idle Process is Idle (I). Windows never reports Z.
    WindowsProcessProbe probe;
    const auto processes = probe.enumerate();
    ASSERT_FALSE(processes.empty());

    const std::string validStates = "RSTI?";
    const auto ourPid = static_cast<std::int32_t>(GetCurrentProcessId());
    std::size_t sleeping = 0;
    for (const auto& proc : processes)
    {
        EXPECT_NE(validStates.find(proc.state), std::string::npos) << proc.name << " has state '" << proc.state << "'";
        if (proc.state == 'S')
        {
            ++sleeping;
        }
        if (proc.pid == 0)
        {
            EXPECT_EQ(proc.state, 'I') << "System Idle Process";
        }
        if (proc.pid == ourPid)
        {
            EXPECT_EQ(proc.state, 'R') << "this thread was running while it enumerated";
        }
    }
    EXPECT_GT(sleeping, processes.size() / 2) << "most processes should be waiting, not running";
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
// classifyEStatsRow (#1100): the per-row decision shared by the IPv4 and IPv6 EStats walks
// ---------------------------------------------------------------------------

TEST(MarkWindowsReadAvailabilityTest, WithoutPerProcessNetworkCountersNetworkIsUnavailableNotZero)
{
    // #1285: non-elevated, no process's network bytes are read; they must not pass for a 0 B/s reading.
    ProcessCounters counters{};
    markWindowsReadAvailability(counters, false);
    EXPECT_FALSE(counters.networkCountersAvailable);
    EXPECT_TRUE(counters.handleCountAvailable); // From the bulk snapshot, for every process
    EXPECT_TRUE(counters.ioCountersAvailable);
}

TEST(MarkWindowsReadAvailabilityTest, WithPerProcessNetworkCountersEveryReadingIsAvailable)
{
    ProcessCounters counters{};
    counters.handleCountAvailable = false;
    counters.ioCountersAvailable = false;
    counters.networkCountersAvailable = false;
    markWindowsReadAvailability(counters, true);
    EXPECT_TRUE(counters.networkCountersAvailable);
    EXPECT_TRUE(counters.handleCountAvailable);
    EXPECT_TRUE(counters.ioCountersAvailable);
}

namespace
{
[[nodiscard]] ProcessThreadTally tallyOf(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> threads)
{
    ProcessThreadTally tally;
    for (const auto& [state, waitReason] : threads)
    {
        tally.add(state, waitReason);
    }
    return tally;
}
constexpr std::uint32_t WAIT_REASON_USER_REQUEST = 6; // An ordinary wait (WaitForSingleObject and the like)
constexpr std::uint32_t THREAD_STATE_INITIALIZED = 0;
constexpr std::uint32_t THREAD_STATE_TERMINATED = 4;
} // namespace

TEST(DeriveProcessStateTest, AnyRunningOrReadyThreadIsRunning)
{
    // #1156: Linux 'R' is running or runnable.
    for (const std::uint32_t runnable :
         {THREAD_STATE_READY, THREAD_STATE_RUNNING, THREAD_STATE_STANDBY, THREAD_STATE_TRANSITION, THREAD_STATE_DEFERRED_READY})
    {
        EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_USER_REQUEST}, {runnable, 0}}), false), 'R') << runnable;
    }
    // A running thread beside suspended ones: the process is not stopped.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_SUSPENDED}, {THREAD_STATE_RUNNING, 0}}), false), 'R');
}

TEST(DeriveProcessStateTest, EveryThreadWaitingIsSleeping)
{
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_USER_REQUEST},
                                          {THREAD_STATE_GATE_WAIT, 0},
                                          {THREAD_STATE_WAITING_FOR_PROCESS_IN_SWAP, 0}}),
                                 false),
              'S');
    // Some threads suspended, others in ordinary waits: still sleeping, not stopped.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_SUSPENDED}, {THREAD_STATE_WAITING, WAIT_REASON_USER_REQUEST}}),
                                 false),
              'S');
}

TEST(DeriveProcessStateTest, EveryThreadSuspendedIsStopped)
{
    // A suspended or frozen process, or one stopped in a debugger: Linux 'T'.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_SUSPENDED}, {THREAD_STATE_WAITING, WAIT_REASON_WR_SUSPENDED}}),
                                 false),
              'T');
    // Initialized/terminated threads neither run nor wait, so they don't stop it reading as stopped.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_SUSPENDED}, {THREAD_STATE_TERMINATED, 0}}), false), 'T');
}

TEST(DeriveProcessStateTest, NoThreadToJudgeByIsUnknownNeverZombie)
{
    // Minimal processes (Secure System) list no threads; Windows has no zombie state to report.
    EXPECT_EQ(deriveProcessState(ProcessThreadTally{}, false), '?');
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_INITIALIZED, 0}, {THREAD_STATE_TERMINATED, 0}}), false), '?');
}

TEST(DeriveProcessStateTest, SystemIdleProcessIsIdle)
{
    // Its threads run whenever a CPU is idle; that is not load.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_RUNNING, 0}, {THREAD_STATE_RUNNING, 0}}), true), 'I');
}

TEST(PlanDetailRefreshTest, FirstSampleRefreshesEverything)
{
    const auto plan = planDetailRefresh(true, false, false, false);
    EXPECT_TRUE(plan.light);
    EXPECT_TRUE(plan.heavy);
    EXPECT_TRUE(plan.priority);
}

TEST(PlanDetailRefreshTest, NothingDueAndNoPriorityChangeNeedsNoHandle)
{
    EXPECT_FALSE(planDetailRefresh(false, false, false, false).any());
}

TEST(PlanDetailRefreshTest, BasePriorityChangeRereadsOnlyThePriorityClass)
{
    // #1156: a Set Priority shows on the next sample, not up to a heavy TTL later.
    const auto plan = planDetailRefresh(false, false, false, true);
    EXPECT_TRUE(plan.priority);
    EXPECT_FALSE(plan.light);
    EXPECT_FALSE(plan.heavy);
}

TEST(PlanDetailRefreshTest, PriorityIsAlsoReadWithTheHeavyDetails)
{
    const auto heavy = planDetailRefresh(false, false, true, false);
    EXPECT_TRUE(heavy.heavy);
    EXPECT_TRUE(heavy.priority);
    const auto light = planDetailRefresh(false, true, false, false);
    EXPECT_TRUE(light.light);
    EXPECT_FALSE(light.heavy);
    EXPECT_FALSE(light.priority);
}

TEST(ClassifyEStatsRowTest, SaneEstablishedReadsAreReported)
{
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, 1'000, 2'000), EStatsRowOutcome::Accumulated);
    // A just-opened connection reads OK with zero bytes; it is still reported, so Domain tracks it
    // from now on.
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, 0, 0), EStatsRowOutcome::Accumulated);
}

TEST(ClassifyEStatsRowTest, NonEstablishedRowsAreSkipped)
{
    constexpr std::uint32_t LISTEN = 2;
    constexpr std::uint32_t TIME_WAIT = 11;

    EXPECT_EQ(classifyEStatsRow(LISTEN, 0, 100, 100), EStatsRowOutcome::SkippedState);
    EXPECT_EQ(classifyEStatsRow(TIME_WAIT, 0, 100, 100), EStatsRowOutcome::SkippedState);
}

TEST(ClassifyEStatsRowTest, FailedReadsAreNotReported)
{
    constexpr std::uint32_t ERROR_NOT_FOUND_CODE = 1168;
    constexpr std::uint32_t ERROR_ACCESS_DENIED_CODE = 5;

    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, ERROR_NOT_FOUND_CODE, 100, 100), EStatsRowOutcome::ReadFailed);
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, ERROR_ACCESS_DENIED_CODE, 100, 100), EStatsRowOutcome::ReadFailed);
}

TEST(ClassifyEStatsRowTest, CountersAboveOneTerabyteAreRejected)
{
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, MAX_SANE_ESTATS_CONNECTION_BYTES + 1, 0), EStatsRowOutcome::Garbage);
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, 0, MAX_SANE_ESTATS_CONNECTION_BYTES + 1), EStatsRowOutcome::Garbage);
    // Exactly 1 TB is still accepted (the cap is exclusive).
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, MAX_SANE_ESTATS_CONNECTION_BYTES, 0), EStatsRowOutcome::Accumulated);
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
    EStatsSampleCounts counts;
    constexpr std::uint32_t LISTEN = 2;
    constexpr std::uint64_t TOO_BIG = MAX_SANE_ESTATS_CONNECTION_BYTES + 1;

    (void) recordEStatsRow(counts, LISTEN, std::nullopt, NO_ERROR, 9, 9);             // not counted
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, NO_ERROR, NO_ERROR, 10, 0); // sane, has data
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, NO_ERROR, NO_ERROR, 0, 0);  // sane, no data
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, NO_ERROR, NO_ERROR, TOO_BIG, 0);
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, ERROR_NOT_FOUND, ERROR_NOT_FOUND, 0, 0);
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, std::nullopt, ERROR_INVALID_PARAMETER, 0, 0);
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, ERROR_ACCESS_DENIED, ERROR_ACCESS_DENIED, 0, 0);

    EXPECT_EQ(counts.established, 6U);
    EXPECT_EQ(counts.enabled, 3U);
    EXPECT_EQ(counts.readOk, 3U);
    EXPECT_EQ(counts.saneReads, 2U);
    EXPECT_EQ(counts.hasData, 1U);
    EXPECT_EQ(counts.garbage, 1U);
    EXPECT_EQ(counts.readNotFound, 1U);
    EXPECT_EQ(counts.readFailedOther, 1U); // ACCESS_DENIED is tallied as accessDenied, not here
    EXPECT_EQ(counts.accessDenied, 1U);
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
    for (const auto& row : rows)
    {
        (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, row.enableStatus, row.readStatus, row.bytesOut, row.bytesIn);
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
    // A > 1 TB counter is rejected and never reported, so a sample of only garbage reads
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

// ---------------------------------------------------------------------------
// estatsConnectionKey (#1256): a stable per-connection key for SocketTrafficAccumulator
// ---------------------------------------------------------------------------

TcpConnectionEndpoints ipv4Endpoints(std::uint8_t localLast, std::uint32_t localPort, std::uint8_t remoteLast, std::uint32_t remotePort)
{
    TcpConnectionEndpoints endpoints;
    endpoints.family = TcpAddressFamily::IPv4;
    endpoints.localAddr = {192, 168, 1, localLast};
    endpoints.localPort = localPort;
    endpoints.remoteAddr = {10, 0, 0, remoteLast};
    endpoints.remotePort = remotePort;
    return endpoints;
}

TEST(EStatsConnectionKeyTest, SameConnectionHasTheSameKey)
{
    const auto endpoints = ipv4Endpoints(10, 0xBB01, 20, 0xD2C3);
    EXPECT_EQ(estatsConnectionKey(endpoints), estatsConnectionKey(endpoints));
    EXPECT_NE(estatsConnectionKey(endpoints), 0U);
    EXPECT_NE(estatsConnectionKey(TcpConnectionEndpoints{}), 0U); // 0 is reserved by the accumulator
}

TEST(EStatsConnectionKeyTest, UndefinedUpperPortBitsDoNotChangeTheKey)
{
    // The owner-PID tables leave the upper 16 bits of the port DWORDs undefined.
    const auto clean = ipv4Endpoints(10, 0x0000BB01U, 20, 0x0000D2C3U);
    const auto dirty = ipv4Endpoints(10, 0xDEADBB01U, 20, 0x1234D2C3U);
    EXPECT_EQ(estatsConnectionKey(clean), estatsConnectionKey(dirty));
}

TEST(EStatsConnectionKeyTest, SwappedEndpointsAreDifferentConnections)
{
    // Both ends of a loopback connection are in the table, owned by different processes.
    auto forward = ipv4Endpoints(1, 0x1111, 1, 0x2222);
    forward.localAddr = {127, 0, 0, 1};
    forward.remoteAddr = {127, 0, 0, 1};
    auto backward = forward;
    std::swap(backward.localPort, backward.remotePort);
    EXPECT_NE(estatsConnectionKey(forward), estatsConnectionKey(backward));

    const auto a = ipv4Endpoints(10, 0x1111, 20, 0x2222);
    TcpConnectionEndpoints b = a;
    std::swap(b.localAddr, b.remoteAddr);
    std::swap(b.localPort, b.remotePort);
    EXPECT_NE(estatsConnectionKey(a), estatsConnectionKey(b));
}

TEST(EStatsConnectionKeyTest, EachFieldDistinguishesConnections)
{
    const auto base = ipv4Endpoints(10, 0xBB01, 20, 0xD2C3);
    const std::uint64_t baseKey = estatsConnectionKey(base);
    EXPECT_NE(estatsConnectionKey(ipv4Endpoints(11, 0xBB01, 20, 0xD2C3)), baseKey);
    EXPECT_NE(estatsConnectionKey(ipv4Endpoints(10, 0xBB02, 20, 0xD2C3)), baseKey);
    EXPECT_NE(estatsConnectionKey(ipv4Endpoints(10, 0xBB01, 21, 0xD2C3)), baseKey);
    EXPECT_NE(estatsConnectionKey(ipv4Endpoints(10, 0xBB01, 20, 0xD2C4)), baseKey);

    TcpConnectionEndpoints v6;
    v6.family = TcpAddressFamily::IPv6;
    v6.localAddr = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    v6.remoteAddr = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02};
    v6.localPort = 0x1111;
    v6.remotePort = 0x2222;
    v6.localScopeId = 7;
    v6.remoteScopeId = 7;
    TcpConnectionEndpoints otherScope = v6;
    otherScope.remoteScopeId = 8; // the same link-local addresses on another interface
    EXPECT_NE(estatsConnectionKey(v6), estatsConnectionKey(otherScope));
}

TEST(EStatsConnectionKeyTest, Ipv4AndIpv6KeysNeverCollide)
{
    // The family is the key's top bit, so no IPv4 key can equal an IPv6 one -- not even for the
    // same address bytes and ports.
    constexpr std::uint64_t FAMILY_BIT = 1ULL << 63U;
    for (std::uint8_t last = 0; last < 64; ++last)
    {
        const auto v4 = ipv4Endpoints(last, 0x1111, last, 0x2222);
        TcpConnectionEndpoints v6 = v4;
        v6.family = TcpAddressFamily::IPv6;
        EXPECT_EQ(estatsConnectionKey(v4) & FAMILY_BIT, 0U);
        EXPECT_EQ(estatsConnectionKey(v6) & FAMILY_BIT, FAMILY_BIT);
    }
}

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

// ---------------------------------------------------------------------------
// EStats walks through Domain::SocketTrafficAccumulator (#1256): a process's network counter is
// monotonic, whatever its connections do between samples
// ---------------------------------------------------------------------------

/// Feeds fabricated EStats walks through makeSocketTrafficReading() and the accumulator
/// ProcessModel uses, as readSocketTraffic() and ProcessModel::refresh() do.
struct EStatsTrafficHarness
{
    static constexpr std::uint32_t PID = 4242;
    static constexpr std::uint64_t START_TICKS = 1'000;
    static constexpr std::uint64_t SECOND_NS = 1'000'000'000ULL;

    Domain::SocketTrafficAccumulator accumulator;
    std::uint64_t nowNs = 0;

    static EStatsConnectionRead good(std::uint64_t key, std::uint64_t received, std::uint64_t sent, std::uint32_t pid = PID)
    {
        return {.key = key, .pid = pid, .outcome = EStatsRowOutcome::Accumulated, .bytesReceived = received, .bytesSent = sent};
    }

    static EStatsConnectionRead failed(std::uint64_t key, EStatsRowOutcome outcome = EStatsRowOutcome::ReadFailed)
    {
        return {.key = key, .pid = PID, .outcome = outcome};
    }

    /// One sample: returns the process's (received, sent) totals.
    std::pair<std::uint64_t, std::uint64_t>
    sample(const std::vector<EStatsConnectionRead>& reads, bool complete = true, std::uint64_t startTicks = START_TICKS)
    {
        nowNs += SECOND_NS;
        std::vector<ProcessCounters> processes(1);
        processes[0].pid = static_cast<std::int32_t>(PID);
        processes[0].startTimeTicks = startTicks;
        accumulator.apply(makeSocketTrafficReading(reads, complete, nowNs), processes);
        return {processes[0].netReceivedBytes, processes[0].netSentBytes};
    }
};

TEST(EStatsSocketTrafficTest, ClosingAConnectionDoesNotLowerTheProcessTotal)
{
    // The old per-PID sum read 1'000 + 100 = 1'100, then 200 once B closed: a negative delta, and
    // the process showed 0 B/s however much A moved (#1256).
    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(1, 100, 10), EStatsTrafficHarness::good(2, 1'000, 100)}); // baseline
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 150, 20), EStatsTrafficHarness::good(2, 1'500, 200)}),
              std::make_pair(std::uint64_t{550}, std::uint64_t{110}));
    // B closed; A moved 50 more in each direction.
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 200, 70)}), std::make_pair(std::uint64_t{600}, std::uint64_t{160}));
    // Every connection closed: the total holds.
    EXPECT_EQ(h.sample({}), std::make_pair(std::uint64_t{600}, std::uint64_t{160}));
}

TEST(EStatsSocketTrafficTest, Ipv4AndIpv6ConnectionsOfOneProcessAddUp)
{
    // Before #1100 only IPv4 was walked; both families now feed the same reading.
    TcpConnectionEndpoints v4;
    v4.localPort = 0x1111;
    TcpConnectionEndpoints v6 = v4;
    v6.family = TcpAddressFamily::IPv6;
    const std::uint64_t key4 = estatsConnectionKey(v4);
    const std::uint64_t key6 = estatsConnectionKey(v6);

    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(key4, 0, 0), EStatsTrafficHarness::good(key6, 0, 0)});
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(key4, 1'000, 2'000), EStatsTrafficHarness::good(key6, 30'000, 40'000)}),
              std::make_pair(std::uint64_t{31'000}, std::uint64_t{42'000}));
}

TEST(EStatsSocketTrafficTest, AFailedRowReadDoesNotSpike)
{
    // A connection whose EStats read fails for one sample (or reads garbage) is reported unreadable
    // and keeps its baseline in Domain. Left out, it would look closed and then new: its 10'000
    // lifetime bytes would land in one interval.
    for (const EStatsRowOutcome outcome : {EStatsRowOutcome::ReadFailed, EStatsRowOutcome::Garbage})
    {
        EStatsTrafficHarness h;
        (void) h.sample({EStatsTrafficHarness::good(1, 10'000, 5'000)}); // baseline
        EXPECT_EQ(h.sample({EStatsTrafficHarness::failed(1, outcome)}), std::make_pair(std::uint64_t{0}, std::uint64_t{0}));
        EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 10'300, 5'030)}), std::make_pair(std::uint64_t{300}, std::uint64_t{30}));
    }
}

TEST(EStatsSocketTrafficTest, AFailedReadIsReportedUnreadable)
{
    // The probe keeps no per-connection state (#1256): a failed or garbage read is reported as an
    // unreadable sample of a connection still in the table, whatever came before it; rows not in
    // ESTABLISHED are left out.
    const std::vector<EStatsConnectionRead> reads{
        EStatsTrafficHarness::good(1, 100, 10),
        EStatsTrafficHarness::failed(7),
        EStatsTrafficHarness::failed(8, EStatsRowOutcome::Garbage),
        {.key = 9, .pid = 1, .outcome = EStatsRowOutcome::SkippedState},
    };
    const auto samples = buildSocketTrafficSamples(reads);
    ASSERT_EQ(samples.size(), 3U);
    EXPECT_TRUE(samples[0].readable);
    EXPECT_EQ(samples[0].bytesReceived, 100U);
    EXPECT_EQ(samples[0].bytesSent, 10U);
    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        EXPECT_FALSE(samples[i].readable);
        EXPECT_EQ(samples[i].pid, static_cast<std::int32_t>(EStatsTrafficHarness::PID));
    }
    EXPECT_EQ(samples[1].key, 7U);
    EXPECT_EQ(samples[2].key, 8U);
}

TEST(EStatsSocketTrafficTest, AConnectionFirstReadFailedDoesNotCreditItsLifetimeBytes)
{
    // A connection already open when its first read fails: once it reads, its 50'000 lifetime bytes
    // only set the baseline instead of landing in one interval (#1256).
    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(1, 0, 0)}); // baseline
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 10, 1), EStatsTrafficHarness::failed(2)}),
              std::make_pair(std::uint64_t{10}, std::uint64_t{1}));
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 20, 2), EStatsTrafficHarness::good(2, 50'000, 5'000)}),
              std::make_pair(std::uint64_t{20}, std::uint64_t{2}));
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 20, 2), EStatsTrafficHarness::good(2, 50'400, 5'040)}),
              std::make_pair(std::uint64_t{420}, std::uint64_t{42}));
}

TEST(EStatsSocketTrafficTest, ASampleWithAnUnreadableTableIsSkipped)
{
    // If the IPv4 or IPv6 table can't be read, its connections are missing from the walk. Reported,
    // they'd look closed and then new; the sample reports no reading instead, and the next complete
    // one measures from the last.
    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(1, 1'000, 100), EStatsTrafficHarness::good(2, 2'000, 200)}); // baseline

    const auto partial = makeSocketTrafficReading({}, false, 123);
    EXPECT_EQ(partial.sampleTimeNs, 0U);
    EXPECT_TRUE(partial.sockets.empty());

    // Only connection 1's table was read this sample: skipped, totals held.
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 1'100, 110)}, false), std::make_pair(std::uint64_t{0}, std::uint64_t{0}));
    // Both back: only the growth since the baseline is credited.
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 1'300, 130), EStatsTrafficHarness::good(2, 2'500, 250)}),
              std::make_pair(std::uint64_t{800}, std::uint64_t{80}));
}

TEST(EStatsSocketTrafficTest, AReusedPidStartsFromZero)
{
    // A process identified by PID and start time: a new process reusing the PID doesn't inherit
    // the old one's bytes (SocketTrafficAccumulator).
    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(1, 0, 0)});
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 500, 50)}), std::make_pair(std::uint64_t{500}, std::uint64_t{50}));
    constexpr std::uint64_t NEW_START = EStatsTrafficHarness::START_TICKS + 1;
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(2, 40, 4)}, true, NEW_START), std::make_pair(std::uint64_t{40}, std::uint64_t{4}));
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
