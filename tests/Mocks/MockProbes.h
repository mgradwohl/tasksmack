/// @file MockProbes.h
/// @brief Shared mock implementations for platform probes used in unit tests.
///
/// This header provides reusable mock classes for IProcessProbe and ISystemProbe,
/// along with helper functions for creating test data, and the Services and Startup tabs' probes and
/// actions (#1721), which the panels call on their sampler and action threads.

#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/PowerTypes.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SystemTypes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace TestMocks
{

// =============================================================================
// Process Counter Helpers (forward declarations needed by MockProcessProbe)
// =============================================================================

/// Create a ProcessCounters struct with common test values.
inline Platform::ProcessCounters makeProcessCounters(int32_t pid,
                                                     const std::string& name,
                                                     char state = 'R',
                                                     uint64_t userTime = 100,
                                                     uint64_t systemTime = 50,
                                                     uint64_t startTimeTicks = 1000,
                                                     uint64_t rssBytes = 1024 * 1024,
                                                     int32_t parentPid = 1)
{
    Platform::ProcessCounters c;
    c.pid = pid;
    c.name = name;
    c.state = state;
    c.userTime = userTime;
    c.systemTime = systemTime;
    c.startTimeTicks = startTimeTicks;
    c.rssBytes = rssBytes;
    c.virtualBytes = rssBytes * 2;
    c.threadCount = 1;
    c.parentPid = parentPid;
    return c;
}

// =============================================================================
// Mock Process Probe
// =============================================================================

/// Mock implementation of IProcessProbe for testing.
/// Allows controlled injection of process data and tracks call counts.
/// Supports fluent builder API for convenient test setup.
class MockProcessProbe : public Platform::IProcessProbe
{
  public:
    // Builder pattern methods for fluent API
    MockProcessProbe& withProcess(int32_t pid, const std::string& name)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.pid == pid)
            {
                counter = makeProcessCounters(pid, name);
                return *this;
            }
        }

        m_Counters.push_back(makeProcessCounters(pid, name));
        return *this;
    }

    MockProcessProbe& withProcess(Platform::ProcessCounters counter)
    {
        for (auto& existing : m_Counters)
        {
            if (existing.pid == counter.pid)
            {
                existing = std::move(counter);
                return *this;
            }
        }

        m_Counters.push_back(std::move(counter));
        return *this;
    }

    MockProcessProbe& withCpuTime(int32_t pid, uint64_t userTime, uint64_t systemTime)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.pid == pid)
            {
                counter.userTime = userTime;
                counter.systemTime = systemTime;
                return *this;
            }
        }
        // If process doesn't exist, create it
        auto c = makeProcessCounters(pid, "process_" + std::to_string(pid));
        c.userTime = userTime;
        c.systemTime = systemTime;
        m_Counters.push_back(c);
        return *this;
    }

    MockProcessProbe& withMemory(int32_t pid, uint64_t rssBytes, uint64_t virtualBytes = 0)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.pid == pid)
            {
                counter.rssBytes = rssBytes;
                counter.virtualBytes = virtualBytes > 0 ? virtualBytes : rssBytes * 2;
                return *this;
            }
        }
        // If process doesn't exist, create it
        auto c = makeProcessCounters(pid, "process_" + std::to_string(pid));
        c.rssBytes = rssBytes;
        c.virtualBytes = virtualBytes > 0 ? virtualBytes : rssBytes * 2;
        m_Counters.push_back(c);
        return *this;
    }

    /// The Private and Swap columns' figures (#1879, #1885): ProcessCounters::privateBytes and swapBytes.
    MockProcessProbe& withMemoryBreakdown(int32_t pid, uint64_t privateBytes, uint64_t swapBytes)
    {
        findOrCreateProcess(pid,
                            [privateBytes, swapBytes](Platform::ProcessCounters& c)
                            {
                                c.privateBytes = privateBytes;
                                c.swapBytes = swapBytes;
                            });
        return *this;
    }

    MockProcessProbe& withState(int32_t pid, char state)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.pid == pid)
            {
                counter.state = state;
                return *this;
            }
        }
        // If process doesn't exist, create it
        auto c = makeProcessCounters(pid, "process_" + std::to_string(pid));
        c.state = state;
        m_Counters.push_back(c);
        return *this;
    }

    MockProcessProbe& withThreadCount(int32_t pid, int32_t threadCount)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.pid == pid)
            {
                counter.threadCount = threadCount;
                return *this;
            }
        }
        // If process doesn't exist, create it
        auto c = makeProcessCounters(pid, "process_" + std::to_string(pid));
        c.threadCount = threadCount;
        m_Counters.push_back(c);
        return *this;
    }

    MockProcessProbe& withNetworkCounters(int32_t pid, uint64_t sentBytes, uint64_t receivedBytes)
    {
        findOrCreateProcess(pid,
                            [sentBytes, receivedBytes](Platform::ProcessCounters& c)
                            {
                                c.netSentBytes = sentBytes;
                                c.netReceivedBytes = receivedBytes;
                            });
        return *this;
    }

    /// When the network counters were read (ProcessCounters::netSampleTimeNs), for a probe that caches them.
    MockProcessProbe& withNetworkSampleTime(int32_t pid, uint64_t sampleTimeNs)
    {
        findOrCreateProcess(pid, [sampleTimeNs](Platform::ProcessCounters& c) { c.netSampleTimeNs = sampleTimeNs; });
        return *this;
    }

    MockProcessProbe& withIoCounters(int32_t pid, uint64_t readBytes, uint64_t writeBytes)
    {
        findOrCreateProcess(pid,
                            [readBytes, writeBytes](Platform::ProcessCounters& c)
                            {
                                c.readBytes = readBytes;
                                c.writeBytes = writeBytes;
                            });
        return *this;
    }

    MockProcessProbe& withParent(int32_t pid, int32_t parentPid)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.pid == pid)
            {
                counter.parentPid = parentPid;
                return *this;
            }
        }
        // If process doesn't exist, create it
        auto c = makeProcessCounters(pid, "process_" + std::to_string(pid));
        c.parentPid = parentPid;
        m_Counters.push_back(c);
        return *this;
    }

    MockProcessProbe& withPowerUsage(int32_t pid, uint64_t energyMicrojoules)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.pid == pid)
            {
                counter.energyMicrojoules = energyMicrojoules;
                return *this;
            }
        }
        // If process doesn't exist, create it
        auto c = makeProcessCounters(pid, "process_" + std::to_string(pid));
        c.energyMicrojoules = energyMicrojoules;
        m_Counters.push_back(c);
        return *this;
    }

    // Backward compatibility: legacy setters
    void setCounters(std::vector<Platform::ProcessCounters> counters)
    {
        m_Counters = std::move(counters);
    }

    void setTotalCpuTime(uint64_t time)
    {
        m_TotalCpuTime = time;
    }

    void setCapabilities(Platform::ProcessCapabilities caps)
    {
        m_Capabilities = caps;
    }

    void setTicksPerSecond(long tps)
    {
        m_TicksPerSecond = tps;
    }

    [[nodiscard]] std::vector<Platform::ProcessCounters> enumerate() override
    {
        m_EnumerateCount.fetch_add(1);
        return m_Counters;
    }

    [[nodiscard]] uint64_t totalCpuTime() const override
    {
        return m_TotalCpuTime;
    }

    [[nodiscard]] Platform::ProcessCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

    [[nodiscard]] long ticksPerSecond() const override
    {
        return m_TicksPerSecond;
    }

    [[nodiscard]] uint64_t systemTotalMemory() const override
    {
        return m_SystemTotalMemory;
    }

    /// Package energy for ProcessModel to share out per interval (#1093); nullopt (the default)
    /// means the probe reports per-process energy itself.
    void setPackageEnergy(std::optional<Platform::PackageEnergyReading> reading)
    {
        m_PackageEnergy = std::move(reading);
    }

    [[nodiscard]] std::optional<Platform::PackageEnergyReading> readPackageEnergy() const override
    {
        return m_PackageEnergy;
    }

    /// The PSS/USS readMemoryRollup() gives for @p pid (#1885); it also says the probe can read them
    /// (ProcessCapabilities::hasMemoryRollup).
    void setMemoryRollup(std::int32_t pid, Platform::MemoryRollup rollup)
    {
        m_Capabilities.hasMemoryRollup = true;
        m_MemoryRollups[pid] = rollup;
    }

    /// What processFromFiles() builds from a hidden process's files (#2728); unset, it builds nothing.
    void setProcessFromFiles(std::function<std::optional<Platform::ProcessCounters>(std::int32_t, const Platform::ProcFileTexts&)> build)
    {
        m_ProcessFromFiles = std::move(build);
    }

    [[nodiscard]] std::optional<Platform::ProcessCounters> processFromFiles(std::int32_t pid,
                                                                            const Platform::ProcFileTexts& files) const override
    {
        return m_ProcessFromFiles ? m_ProcessFromFiles(pid, files) : std::nullopt;
    }

    /// readMemoryRollup() refuses @p pid as the OS refuses another user's process (#2309).
    void setMemoryRollupDenied(std::int32_t pid)
    {
        m_Capabilities.hasMemoryRollup = true;
        m_MemoryRollupsDenied.push_back(pid);
    }

    [[nodiscard]] Platform::MemoryRollupRead readMemoryRollup(std::int32_t pid, std::uint64_t startTimeTicks) const override
    {
        ++m_MemoryRollupReads;
        m_LastMemoryRollupStartTime = startTimeTicks;
        if (std::ranges::find(m_MemoryRollupsDenied, pid) != m_MemoryRollupsDenied.end())
        {
            return std::unexpected(Platform::MemoryRollupFailure::PermissionDenied);
        }
        const auto it = m_MemoryRollups.find(pid);
        if (it == m_MemoryRollups.end())
        {
            return std::unexpected(Platform::MemoryRollupFailure::Unavailable);
        }
        return it->second;
    }

    /// The start time the last readMemoryRollup() was asked to verify.
    [[nodiscard]] std::uint64_t lastMemoryRollupStartTime() const
    {
        return m_LastMemoryRollupStartTime.load();
    }

    /// How many times readMemoryRollup() was called.
    [[nodiscard]] int memoryRollupReads() const
    {
        return m_MemoryRollupReads.load();
    }

    /// Per-connection network reading for ProcessModel to accumulate (#1099); the default (empty,
    /// sampleTimeNs 0) means the probe reports per-process network counters itself.
    void setSocketTraffic(Platform::SocketTrafficReading reading)
    {
        m_SocketTraffic = std::move(reading);
    }

    /// Capabilities the probe switches to during its next readSocketTraffic(), as the Windows probe
    /// does when a real EStats sample proves its per-process network counters unusable (#1161).
    void switchCapabilitiesOnNextSocketRead(Platform::ProcessCapabilities caps)
    {
        m_CapabilitiesAfterSocketRead = caps;
    }

    [[nodiscard]] Platform::SocketTrafficReading readSocketTraffic() const override
    {
        if (m_CapabilitiesAfterSocketRead.has_value())
        {
            m_Capabilities = *m_CapabilitiesAfterSocketRead;
            m_CapabilitiesAfterSocketRead.reset();
        }
        return m_SocketTraffic;
    }

    void setSystemTotalMemory(uint64_t bytes)
    {
        m_SystemTotalMemory = bytes;
    }

    /// Get number of times enumerate() was called (thread-safe).
    [[nodiscard]] int enumerateCount() const
    {
        return m_EnumerateCount.load();
    }

    /// Reset the enumerate call counter.
    void resetEnumerateCount()
    {
        m_EnumerateCount.store(0);
    }

  private:
    /// Helper to find existing process counter by PID or create a new one.
    /// Applies the given setter function to set specific counter fields.
    template<typename SetterFunc> void findOrCreateProcess(int32_t pid, SetterFunc setter)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.pid == pid)
            {
                setter(counter);
                return;
            }
        }
        // If process doesn't exist, create it
        auto c = makeProcessCounters(pid, "process_" + std::to_string(pid));
        setter(c);
        m_Counters.push_back(c);
    }

    std::vector<Platform::ProcessCounters> m_Counters;
    uint64_t m_TotalCpuTime = 0;
    uint64_t m_SystemTotalMemory = 8ULL * 1024 * 1024 * 1024; // Default 8 GB
    mutable Platform::ProcessCapabilities m_Capabilities;     // readSocketTraffic() may switch it
    std::function<std::optional<Platform::ProcessCounters>(std::int32_t, const Platform::ProcFileTexts&)> m_ProcessFromFiles;
    mutable std::optional<Platform::ProcessCapabilities> m_CapabilitiesAfterSocketRead;
    long m_TicksPerSecond = 100; // Standard HZ value
    std::atomic<int> m_EnumerateCount{0};
    std::optional<Platform::PackageEnergyReading> m_PackageEnergy;
    Platform::SocketTrafficReading m_SocketTraffic;
    std::map<std::int32_t, Platform::MemoryRollup> m_MemoryRollups;
    std::vector<std::int32_t> m_MemoryRollupsDenied;
    mutable std::atomic<int> m_MemoryRollupReads{0};
    mutable std::atomic<std::uint64_t> m_LastMemoryRollupStartTime{0};
};

// =============================================================================
// Mock System Probe
// =============================================================================

/// Mock implementation of ISystemProbe for testing.
/// Allows controlled injection of system metrics data.
class MockSystemProbe : public Platform::ISystemProbe
{
  public:
    void setCounters(Platform::SystemCounters counters)
    {
        m_Counters = std::move(counters);
    }

    void setCapabilities(Platform::SystemCapabilities caps)
    {
        m_Capabilities = caps;
    }

    void setTicksPerSecond(long tps)
    {
        m_TicksPerSecond = tps;
    }

    [[nodiscard]] Platform::SystemCounters read() override
    {
        m_ReadCount.fetch_add(1);
        return m_Counters;
    }

    [[nodiscard]] Platform::SystemCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

    [[nodiscard]] long ticksPerSecond() const override
    {
        return m_TicksPerSecond;
    }

    /// Get number of times read() was called (thread-safe).
    [[nodiscard]] int readCount() const
    {
        return m_ReadCount.load();
    }

    /// Reset the read call counter.
    void resetReadCount()
    {
        m_ReadCount.store(0);
    }

  private:
    Platform::SystemCounters m_Counters;
    Platform::SystemCapabilities m_Capabilities;
    long m_TicksPerSecond = 100;
    std::atomic<int> m_ReadCount{0};
};

// =============================================================================
// Mock Power Probe
// =============================================================================

/// Mock implementation of IPowerProbe for testing.
class MockPowerProbe : public Platform::IPowerProbe
{
  public:
    void setCounters(Platform::PowerCounters counters)
    {
        m_Counters = std::move(counters);
    }

    void setCapabilities(Platform::PowerCapabilities caps)
    {
        m_Capabilities = caps;
    }

    [[nodiscard]] Platform::PowerCounters read() override
    {
        return m_Counters;
    }

    [[nodiscard]] Platform::PowerCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

  private:
    Platform::PowerCounters m_Counters;
    Platform::PowerCapabilities m_Capabilities;
};

// =============================================================================
// Mock Process Actions
// =============================================================================

/// Mock implementation of IProcessActions for testing.
/// Allows controlled injection of per-method results/capabilities and tracks the pid
/// (and, for setPriority, nice value; for setIoPriority, class and level) each method was last called
/// with, plus a call count. getIoPriority() returns whatever setIoPriorityReadResult() gave it.
class MockProcessActions : public Platform::IProcessActions
{
  public:
    void setCapabilities(Platform::ProcessActionCapabilities caps)
    {
        m_Capabilities = caps;
    }

    void setTerminateResult(Platform::ProcessActionResult result)
    {
        m_TerminateResult = std::move(result);
    }

    void setKillResult(Platform::ProcessActionResult result)
    {
        m_KillResult = std::move(result);
    }

    void setStopResult(Platform::ProcessActionResult result)
    {
        m_StopResult = std::move(result);
    }

    void setResumeResult(Platform::ProcessActionResult result)
    {
        m_ResumeResult = std::move(result);
    }
    void setSendSignalResult(Platform::ProcessActionResult result)
    {
        m_SendSignalResult = std::move(result);
    }

    void setPriorityResult(Platform::ProcessActionResult result)
    {
        m_SetPriorityResult = std::move(result);
    }

    void setIoPriorityResult(Platform::ProcessActionResult result)
    {
        m_SetIoPriorityResult = std::move(result);
    }

    /// What getIoPriority() returns: a value, or an error message.
    void setIoPriorityReadResult(Platform::IoPriorityReadResult result)
    {
        m_IoPriorityReadResult = std::move(result);
    }

    [[nodiscard]] Platform::ProcessActionCapabilities actionCapabilities() const override
    {
        return m_Capabilities;
    }

    [[nodiscard]] Platform::ProcessActionResult terminate(const Platform::ProcessTarget& target) override
    {
        m_LastTerminatePid = target.pid;
        m_LastTarget = target;
        ++m_TerminateCount;
        return m_TerminateResult;
    }

    [[nodiscard]] Platform::ProcessActionResult kill(const Platform::ProcessTarget& target) override
    {
        m_LastKillPid = target.pid;
        m_LastTarget = target;
        ++m_KillCount;
        return m_KillResult;
    }

    [[nodiscard]] Platform::ProcessActionResult stop(const Platform::ProcessTarget& target) override
    {
        m_LastStopPid = target.pid;
        m_LastTarget = target;
        ++m_StopCount;
        return m_StopResult;
    }

    [[nodiscard]] Platform::ProcessActionResult resume(const Platform::ProcessTarget& target) override
    {
        m_LastResumePid = target.pid;
        m_LastTarget = target;
        ++m_ResumeCount;
        return m_ResumeResult;
    }

    [[nodiscard]] Platform::ProcessActionResult sendSignal(const Platform::ProcessTarget& target, Platform::ProcessSignal signal) override
    {
        m_LastTarget = target;
        m_LastSignal = signal;
        ++m_SendSignalCount;
        return m_SendSignalResult;
    }

    [[nodiscard]] Platform::ProcessActionResult setPriority(const Platform::ProcessTarget& target, int32_t nice) override
    {
        m_LastSetPriorityPid = target.pid;
        m_LastTarget = target;
        m_LastSetPriorityNice = nice;
        ++m_SetPriorityCount;
        return m_SetPriorityResult;
    }

    [[nodiscard]] Platform::ProcessActionResult
    setIoPriority(const Platform::ProcessTarget& target, Platform::IoPriorityClass ioClass, int32_t level) override
    {
        m_LastTarget = target;
        m_LastSetIoPriority = {.ioClass = ioClass, .level = level};
        ++m_SetIoPriorityCount;
        return m_SetIoPriorityResult;
    }

    [[nodiscard]] Platform::IoPriorityReadResult getIoPriority(const Platform::ProcessTarget& target) override
    {
        m_LastIoPriorityReadTarget = target;
        ++m_GetIoPriorityCount;
        return m_IoPriorityReadResult;
    }

    /// setAffinity() (#1887): records the target and CPUs, and succeeds.
    [[nodiscard]] Platform::ProcessActionResult setAffinity(const Platform::ProcessTarget& target,
                                                            const Platform::CpuAffinity& cpus) override
    {
        m_LastTarget = target;
        m_LastAffinity = cpus;
        ++m_SetAffinityCount;
        return Platform::ProcessActionResult::ok();
    }
    [[nodiscard]] int setAffinityCount() const
    {
        return m_SetAffinityCount;
    }

    /// runAuthorized() (#2308): records the action and returns this.
    void setRunAuthorizedResult(Platform::ProcessActionResult result)
    {
        m_RunAuthorizedResult = std::move(result);
    }
    [[nodiscard]] Platform::ProcessActionResult runAuthorized(const Platform::AuthorizedAction& action,
                                                              const std::stop_token& /*stopToken*/) override
    {
        m_LastAuthorizedAction = action;
        ++m_RunAuthorizedCount;
        return m_RunAuthorizedResult;
    }
    [[nodiscard]] int runAuthorizedCount() const
    {
        return m_RunAuthorizedCount;
    }
    [[nodiscard]] const Platform::AuthorizedAction& lastAuthorizedAction() const
    {
        return m_LastAuthorizedAction;
    }
    [[nodiscard]] const Platform::CpuAffinity& lastAffinity() const
    {
        return m_LastAffinity;
    }

    [[nodiscard]] int32_t lastTerminatePid() const
    {
        return m_LastTerminatePid;
    }
    [[nodiscard]] int32_t lastKillPid() const
    {
        return m_LastKillPid;
    }
    [[nodiscard]] int32_t lastStopPid() const
    {
        return m_LastStopPid;
    }
    [[nodiscard]] int32_t lastResumePid() const
    {
        return m_LastResumePid;
    }
    [[nodiscard]] int32_t lastSetPriorityPid() const
    {
        return m_LastSetPriorityPid;
    }
    [[nodiscard]] int32_t lastSetPriorityNice() const
    {
        return m_LastSetPriorityNice;
    }
    /// Target passed to whichever action was called most recently.
    [[nodiscard]] Platform::ProcessTarget lastTarget() const
    {
        return m_LastTarget;
    }

    [[nodiscard]] int terminateCount() const
    {
        return m_TerminateCount;
    }
    [[nodiscard]] int killCount() const
    {
        return m_KillCount;
    }
    [[nodiscard]] int stopCount() const
    {
        return m_StopCount;
    }
    [[nodiscard]] int resumeCount() const
    {
        return m_ResumeCount;
    }
    [[nodiscard]] int sendSignalCount() const
    {
        return m_SendSignalCount;
    }
    [[nodiscard]] Platform::ProcessSignal lastSignal() const
    {
        return m_LastSignal;
    }
    [[nodiscard]] int setPriorityCount() const
    {
        return m_SetPriorityCount;
    }
    [[nodiscard]] int setIoPriorityCount() const
    {
        return m_SetIoPriorityCount;
    }
    /// Class and level setIoPriority() was last called with.
    [[nodiscard]] Platform::IoPriority lastSetIoPriority() const
    {
        return m_LastSetIoPriority;
    }
    [[nodiscard]] int getIoPriorityCount() const
    {
        return m_GetIoPriorityCount;
    }
    /// Target getIoPriority() was last called with (reads are not actions, so not in lastTarget()).
    [[nodiscard]] Platform::ProcessTarget lastIoPriorityReadTarget() const
    {
        return m_LastIoPriorityReadTarget;
    }

    void setSyscallTraceResult(Platform::ProcessActionResult result)
    {
        m_SyscallTraceResult = std::move(result);
    }

    [[nodiscard]] Platform::ProcessActionResult launchSyscallTrace(const Platform::ProcessTarget& target) override
    {
        m_LastTarget = target;
        ++m_SyscallTraceCount;
        return m_SyscallTraceResult;
    }

    [[nodiscard]] int syscallTraceCount() const
    {
        return m_SyscallTraceCount;
    }

    /// What openFileLocation() returns (#3064).
    void setOpenFileLocationResult(Platform::ProcessActionResult result)
    {
        m_OpenFileLocationResult = std::move(result);
    }

    [[nodiscard]] Platform::ProcessActionResult openFileLocation(const Platform::ProcessTarget& target) override
    {
        m_LastTarget = target;
        ++m_OpenFileLocationCount;
        return m_OpenFileLocationResult;
    }

    [[nodiscard]] int openFileLocationCount() const
    {
        return m_OpenFileLocationCount;
    }

    /// What runProgram() returns (#3065).
    void setRunProgramResult(Platform::ProcessActionResult result)
    {
        m_RunProgramResult = std::move(result);
    }

    [[nodiscard]] Platform::ProcessActionResult runProgram(const Platform::RunProgramRequest& request) override
    {
        m_LastRunProgram = request;
        ++m_RunProgramCount;
        return m_RunProgramResult;
    }

    [[nodiscard]] int runProgramCount() const
    {
        return m_RunProgramCount;
    }
    [[nodiscard]] const Platform::RunProgramRequest& lastRunProgram() const
    {
        return m_LastRunProgram;
    }

    /// What createDump() returns (#807).
    void setDumpResult(Platform::ProcessDumpResult result)
    {
        m_DumpResult = std::move(result);
    }
    /// createDump() blocks until its stop token is requested, then reports itself cancelled, as the
    /// Windows one does: for tests of Cancel.
    void setDumpWaitsForCancel(bool wait)
    {
        m_DumpWaitsForCancel = wait;
    }
    /// createDump() blocks until releaseDump() (or its stop token: then it reports itself cancelled),
    /// then returns setDumpResult()'s result: for tests of a dump that is still being written while
    /// something else happens (#2117). Call before the dump starts.
    void holdDump()
    {
        m_DumpGate = std::make_shared<DumpGate>();
    }
    /// Lets a dump held by holdDump() finish. Safe from the test's thread while the worker waits.
    void releaseDump()
    {
        if (m_DumpGate == nullptr)
        {
            return;
        }
        {
            const std::scoped_lock lock(m_DumpGate->mutex);
            m_DumpGate->released = true;
        }
        m_DumpGate->wake.notify_all();
    }

    /// Runs on ProcessDumpView's worker. What it records is read by a test only after the worker's
    /// result was taken in (the future's get() orders the two), so it needs no lock.
    [[nodiscard]] Platform::ProcessDumpResult
    createDump(const Platform::ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken) override
    {
        m_LastTarget = target;
        m_LastDumpDirectory = directory;
        ++m_CreateDumpCount;
        if (m_DumpWaitsForCancel)
        {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            wake.wait(lock, stopToken, [] { return false; });
            return std::unexpected(std::string("Cancelled; the partial dump file was deleted"));
        }
        if (m_DumpGate != nullptr)
        {
            std::unique_lock lock(m_DumpGate->mutex);
            if (!m_DumpGate->wake.wait(lock, stopToken, [this] { return m_DumpGate->released; }))
            {
                return std::unexpected(std::string("Cancelled; the partial dump file was deleted"));
            }
        }
        return m_DumpResult;
    }
    [[nodiscard]] int createDumpCount() const
    {
        return m_CreateDumpCount;
    }
    [[nodiscard]] const std::filesystem::path& lastDumpDirectory() const
    {
        return m_LastDumpDirectory;
    }

    /// What dumpSpace() returns (#2001): unknown by default.
    void setDumpSpace(Platform::DumpSpace space)
    {
        m_DumpSpace = space;
    }
    /// Called on the confirm's check worker (#3155), when the dump's confirm opens or its folder changes.
    [[nodiscard]] Platform::DumpSpace dumpSpace(const Platform::ProcessTarget& target, const std::filesystem::path& directory) override
    {
        m_LastTarget = target;
        m_LastSpaceDirectory = directory;
        ++m_DumpSpaceCount;
        return m_DumpSpace;
    }
    [[nodiscard]] int dumpSpaceCount() const
    {
        return m_DumpSpaceCount;
    }
    [[nodiscard]] const std::filesystem::path& lastSpaceDirectory() const
    {
        return m_LastSpaceDirectory;
    }

    /// What chooseDumpFolder() returns (#2001): cancelled by default.
    void setFolderChoice(Platform::FolderChoice choice)
    {
        m_FolderChoice = std::move(choice);
    }
    /// Runs on ProcessDumpView's folder-dialog worker; read by a test only after its answer was taken
    /// in (the future's get() orders the two), so it needs no lock.
    [[nodiscard]] Platform::FolderChoice chooseDumpFolder(const std::filesystem::path& initial, void* ownerWindow) override
    {
        m_LastFolderInitial = initial;
        m_LastFolderOwner = ownerWindow;
        ++m_ChooseFolderCount;
        return m_FolderChoice;
    }
    [[nodiscard]] int chooseFolderCount() const
    {
        return m_ChooseFolderCount;
    }
    [[nodiscard]] const std::filesystem::path& lastFolderInitial() const
    {
        return m_LastFolderInitial;
    }
    [[nodiscard]] void* lastFolderOwner() const
    {
        return m_LastFolderOwner;
    }

  private:
    Platform::ProcessActionCapabilities m_Capabilities;
    Platform::DumpSpace m_DumpSpace;
    int m_DumpSpaceCount = 0;
    std::filesystem::path m_LastSpaceDirectory;
    Platform::FolderChoice m_FolderChoice = std::optional<std::filesystem::path>{};
    int m_ChooseFolderCount = 0;
    std::filesystem::path m_LastFolderInitial;
    void* m_LastFolderOwner = nullptr;
    Platform::ProcessDumpResult m_DumpResult = std::unexpected(std::string("no dump result set"));
    bool m_DumpWaitsForCancel = false;
    /// holdDump()'s latch; shared, so the mock stays copyable.
    struct DumpGate
    {
        std::mutex mutex;
        std::condition_variable_any wake;
        bool released = false;
    };
    std::shared_ptr<DumpGate> m_DumpGate;
    int m_CreateDumpCount = 0;
    std::filesystem::path m_LastDumpDirectory;
    Platform::ProcessActionResult m_TerminateResult = Platform::ProcessActionResult::ok();
    Platform::ProcessActionResult m_KillResult = Platform::ProcessActionResult::ok();
    Platform::ProcessActionResult m_StopResult = Platform::ProcessActionResult::ok();
    Platform::ProcessActionResult m_ResumeResult = Platform::ProcessActionResult::ok();
    Platform::ProcessActionResult m_SetPriorityResult = Platform::ProcessActionResult::ok();
    Platform::ProcessActionResult m_SetIoPriorityResult = Platform::ProcessActionResult::ok();
    Platform::IoPriorityReadResult m_IoPriorityReadResult = Platform::IoPriority{};

    int32_t m_LastTerminatePid = 0;
    int32_t m_LastKillPid = 0;
    int32_t m_LastStopPid = 0;
    int32_t m_LastResumePid = 0;
    int32_t m_LastSetPriorityPid = 0;
    int32_t m_LastSetPriorityNice = 0;
    Platform::ProcessTarget m_LastTarget;

    int m_TerminateCount = 0;
    int m_KillCount = 0;
    int m_StopCount = 0;
    int m_ResumeCount = 0;
    int m_SendSignalCount = 0;
    Platform::ProcessSignal m_LastSignal = Platform::ProcessSignal::Hangup;
    Platform::ProcessActionResult m_SendSignalResult = Platform::ProcessActionResult::ok();
    int m_SetPriorityCount = 0;
    int m_SetIoPriorityCount = 0;
    int m_SetAffinityCount = 0;
    Platform::CpuAffinity m_LastAffinity;
    Platform::ProcessActionResult m_RunAuthorizedResult = Platform::ProcessActionResult::ok();
    int m_RunAuthorizedCount = 0;
    Platform::AuthorizedAction m_LastAuthorizedAction;
    int m_GetIoPriorityCount = 0;
    Platform::IoPriority m_LastSetIoPriority;
    Platform::ProcessTarget m_LastIoPriorityReadTarget;
    int m_SyscallTraceCount = 0;
    Platform::ProcessActionResult m_SyscallTraceResult = Platform::ProcessActionResult::ok();
    int m_OpenFileLocationCount = 0;
    Platform::ProcessActionResult m_OpenFileLocationResult = Platform::ProcessActionResult::ok();
    int m_RunProgramCount = 0;
    Platform::RunProgramRequest m_LastRunProgram;
    Platform::ProcessActionResult m_RunProgramResult = Platform::ProcessActionResult::ok();
};

/// MockProcessActions whose affinityLayout() (#3182) returns a set layout, counts its calls and notes the
/// thread of the last one, and can be held until released (a test then sees the read pending) or made
/// to throw. Thread-safe: the read runs on a worker while the test's thread looks on.
class MockAffinityLayoutActions : public MockProcessActions
{
  public:
    /// What affinityLayout() returns.
    void setAffinityLayout(Platform::CpuAffinityLayout layout)
    {
        const std::scoped_lock lock(m_Layout->mutex);
        m_Layout->layout = std::move(layout);
    }

    /// affinityLayout() waits until releaseAffinityLayout().
    void holdAffinityLayout()
    {
        const std::scoped_lock lock(m_Layout->mutex);
        m_Layout->held = true;
    }

    void releaseAffinityLayout()
    {
        {
            const std::scoped_lock lock(m_Layout->mutex);
            m_Layout->held = false;
        }
        m_Layout->wake.notify_all();
    }

    /// affinityLayout() throws, as a read that failed would.
    void throwFromAffinityLayout()
    {
        const std::scoped_lock lock(m_Layout->mutex);
        m_Layout->throws = true;
    }

    [[nodiscard]] Platform::CpuAffinityLayout affinityLayout() const override
    {
        std::unique_lock lock(m_Layout->mutex);
        ++m_Layout->calls;
        m_Layout->thread = std::this_thread::get_id();
        m_Layout->wake.wait(lock, [state = m_Layout.get()] { return !state->held; });
        if (m_Layout->throws)
        {
            throw std::runtime_error("no CPU layout");
        }
        return m_Layout->layout;
    }

    [[nodiscard]] int affinityLayoutCount() const
    {
        const std::scoped_lock lock(m_Layout->mutex);
        return m_Layout->calls;
    }

    /// The thread the last affinityLayout() ran on.
    [[nodiscard]] std::thread::id affinityLayoutThread() const
    {
        const std::scoped_lock lock(m_Layout->mutex);
        return m_Layout->thread;
    }

  private:
    struct LayoutState
    {
        std::mutex mutex;
        std::condition_variable wake;
        Platform::CpuAffinityLayout layout;
        std::thread::id thread;
        int calls = 0;
        bool held = false;
        bool throws = false;
    };
    // Shared, so the mock stays copyable.
    std::shared_ptr<LayoutState> m_Layout = std::make_shared<LayoutState>();
};

// =============================================================================
// Additional Process Counter Helpers
// =============================================================================

/// Create a minimal ProcessCounters with just PID and name.
inline Platform::ProcessCounters makeSimpleProcess(int32_t pid, const std::string& name)
{
    return makeProcessCounters(pid, name);
}

// =============================================================================
// CPU Counter Helpers
// =============================================================================

/// Create CpuCounters with specific values.
inline Platform::CpuCounters
makeCpuCounters(uint64_t user, uint64_t nice, uint64_t system, uint64_t idle, uint64_t iowait = 0, uint64_t steal = 0)
{
    Platform::CpuCounters c;
    c.user = user;
    c.nice = nice;
    c.system = system;
    c.idle = idle;
    c.iowait = iowait;
    c.steal = steal;
    return c;
}

/// Create CpuCounters representing idle CPU.
inline Platform::CpuCounters makeIdleCpu(uint64_t totalTicks = 10000)
{
    return makeCpuCounters(0, 0, 0, totalTicks);
}

/// Create CpuCounters representing a specific CPU usage percentage.
/// @param usagePercent Percentage of CPU in use (0-100)
/// @param totalTicks Total ticks to distribute
inline Platform::CpuCounters makeCpuAtUsage(double usagePercent, uint64_t totalTicks = 10000)
{
    auto activeTicks = static_cast<uint64_t>(static_cast<double>(totalTicks) * usagePercent / 100.0);
    auto idleTicks = totalTicks - activeTicks;
    // Split active between user and system (2:1 ratio)
    auto userTicks = activeTicks * 2 / 3;
    auto sysTicks = activeTicks - userTicks;
    return makeCpuCounters(userTicks, 0, sysTicks, idleTicks);
}

// =============================================================================
// Memory Counter Helpers
// =============================================================================

/// Create MemoryCounters with specific values.
inline Platform::MemoryCounters makeMemoryCounters(uint64_t total,
                                                   uint64_t available,
                                                   uint64_t free = 0,
                                                   uint64_t cached = 0,
                                                   uint64_t buffers = 0,
                                                   uint64_t swapTotal = 0,
                                                   uint64_t swapFree = 0)
{
    Platform::MemoryCounters m;
    m.totalBytes = total;
    m.availableBytes = available;
    m.hasAvailableBytes = true;
    m.freeBytes = free;
    m.cachedBytes = cached;
    m.buffersBytes = buffers;
    m.swapTotalBytes = swapTotal;
    m.swapFreeBytes = swapFree;
    return m;
}

/// Create MemoryCounters representing a specific memory usage percentage.
/// @param usagePercent Percentage of memory in use (0-100)
/// @param totalBytes Total memory in bytes
inline Platform::MemoryCounters makeMemoryAtUsage(double usagePercent, uint64_t totalBytes = 16ULL * 1024 * 1024 * 1024)
{
    auto availableBytes = static_cast<uint64_t>(static_cast<double>(totalBytes) * (100.0 - usagePercent) / 100.0);
    return makeMemoryCounters(totalBytes, availableBytes);
}

// =============================================================================
// System Counter Helpers
// =============================================================================

/// Create an InterfaceCounters struct for testing.
inline Platform::SystemCounters::InterfaceCounters
makeInterfaceCounters(const std::string& name, uint64_t rxBytes = 0, uint64_t txBytes = 0, bool isUp = true, uint64_t linkSpeedMbps = 1000)
{
    Platform::SystemCounters::InterfaceCounters iface;
    iface.name = name;
    iface.displayName = name;
    iface.rxBytes = rxBytes;
    iface.txBytes = txBytes;
    iface.isUp = isUp;
    iface.linkSpeedMbps = linkSpeedMbps;
    return iface;
}

/// Create a complete SystemCounters struct.
/// Per-core entries are given contiguous core ids 0..N-1 by position, as a probe with every core
/// online reports them; set cpuPerCore[i].coreId afterwards to model an offline core (#1229).
inline Platform::SystemCounters makeSystemCounters(const Platform::CpuCounters& cpu,
                                                   const Platform::MemoryCounters& memory,
                                                   uint64_t uptime = 0,
                                                   std::vector<Platform::CpuCounters> perCore = {},
                                                   uint64_t netRxBytes = 0,
                                                   uint64_t netTxBytes = 0,
                                                   std::vector<Platform::SystemCounters::InterfaceCounters> networkInterfaces = {})
{
    Platform::SystemCounters s;
    s.cpuTotal = cpu;
    s.memory = memory;
    s.uptimeSeconds = uptime;
    s.cpuPerCore = std::move(perCore);
    for (std::size_t i = 0; i < s.cpuPerCore.size(); ++i)
    {
        s.cpuPerCore[i].coreId = i;
    }
    s.netRxBytes = netRxBytes;
    s.netTxBytes = netTxBytes;
    s.networkInterfaces = std::move(networkInterfaces);
    return s;
}

/// Create SystemCounters with default/minimal values.
inline Platform::SystemCounters makeSimpleSystemCounters()
{
    return makeSystemCounters(makeIdleCpu(), makeMemoryAtUsage(50.0));
}

// =============================================================================
// Capabilities Helpers
// =============================================================================

/// Create ProcessCapabilities with all features enabled.
inline Platform::ProcessCapabilities makeFullProcessCapabilities()
{
    Platform::ProcessCapabilities caps;
    caps.hasIoCounters = true;
    caps.hasThreadCount = true;
    caps.hasUserSystemTime = true;
    caps.hasStartTime = true;
    caps.hasPowerUsage = true;
    return caps;
}

/// Create SystemCapabilities with all features enabled.
inline Platform::SystemCapabilities makeFullSystemCapabilities()
{
    Platform::SystemCapabilities caps;
    caps.hasPerCoreCpu = true;
    caps.hasSwap = true;
    caps.hasIoWait = true;
    caps.hasNetworkCounters = true;
    return caps;
}

// =============================================================================
// Mock Process Environment Reader
// =============================================================================

/// Mock IProcessEnvironmentReader (#179): returns a configured result and records each read's
/// target and how many reads there were, so a test can check the on-demand cadence.
class MockProcessEnvironmentReader : public Platform::IProcessEnvironmentReader
{
  public:
    void setHasEnvironment(bool hasEnvironment)
    {
        m_HasEnvironment = hasEnvironment;
    }

    void setResult(Platform::EnvironmentReadResult result)
    {
        m_Result = std::move(result);
    }

    [[nodiscard]] bool hasEnvironment() const override
    {
        return m_HasEnvironment;
    }

    /// Runs @p hook at the start of every read, on the reader's thread (a test can block it there).
    void setBeforeRead(std::function<void()> hook)
    {
        m_BeforeRead = std::move(hook);
    }

    [[nodiscard]] Platform::EnvironmentReadResult readEnvironment(const Platform::ProcessTarget& target) override
    {
        if (m_BeforeRead)
        {
            m_BeforeRead();
        }
        ++m_ReadCount;
        m_LastTarget = target;
        return m_Result;
    }

    [[nodiscard]] int readCount() const
    {
        return m_ReadCount;
    }

    [[nodiscard]] const Platform::ProcessTarget& lastTarget() const
    {
        return m_LastTarget;
    }

    /// readAuthorized() (#2309): offered when set, returning @p result and counting its calls.
    void setAuthorizedResult(Platform::EnvironmentReadResult result)
    {
        m_AuthorizedResult = std::move(result);
        m_CanReadAuthorized = true;
    }
    [[nodiscard]] bool canReadAuthorized() const override
    {
        return m_CanReadAuthorized;
    }
    [[nodiscard]] Platform::EnvironmentReadResult readAuthorized(const Platform::ProcessTarget& target,
                                                                 const std::stop_token& /*stopToken*/) override
    {
        ++m_AuthorizedReadCount;
        m_LastTarget = target;
        return m_AuthorizedResult;
    }
    [[nodiscard]] int authorizedReadCount() const
    {
        return m_AuthorizedReadCount;
    }

  private:
    bool m_HasEnvironment = true;
    bool m_CanReadAuthorized = false;
    int m_AuthorizedReadCount = 0;
    Platform::EnvironmentReadResult m_AuthorizedResult;
    Platform::EnvironmentReadResult m_Result{.status = Platform::EnvironmentReadStatus::Ok, .variables = {}};
    int m_ReadCount = 0;
    Platform::ProcessTarget m_LastTarget{};
    std::function<void()> m_BeforeRead;
};

// =============================================================================
// Mock Process Connections Reader
// =============================================================================

/// Mock IProcessConnectionsReader (#799): returns a configured result and records each read's
/// target and how many reads there were, so a test can check the on-demand cadence.
class MockProcessConnectionsReader : public Platform::IProcessConnectionsReader
{
  public:
    void setHasConnections(bool hasConnections)
    {
        m_HasConnections = hasConnections;
    }

    void setResult(Platform::ConnectionsReadResult result)
    {
        m_Result = std::move(result);
    }

    [[nodiscard]] bool hasConnections() const override
    {
        return m_HasConnections;
    }

    [[nodiscard]] Platform::ConnectionsReadResult readConnections(const Platform::ProcessTarget& target) override
    {
        ++m_ReadCount;
        m_LastTarget = target;
        return m_Result;
    }

    [[nodiscard]] int readCount() const
    {
        return m_ReadCount;
    }

    [[nodiscard]] const Platform::ProcessTarget& lastTarget() const
    {
        return m_LastTarget;
    }

    /// readAuthorized() (#2309): offered when set, returning @p result and counting its calls.
    void setAuthorizedResult(Platform::ConnectionsReadResult result)
    {
        m_AuthorizedResult = std::move(result);
        m_CanReadAuthorized = true;
    }
    [[nodiscard]] bool canReadAuthorized() const override
    {
        return m_CanReadAuthorized;
    }
    [[nodiscard]] Platform::ConnectionsReadResult readAuthorized(const Platform::ProcessTarget& /*target*/,
                                                                 const std::stop_token& /*stopToken*/) override
    {
        ++m_AuthorizedReadCount;
        return m_AuthorizedResult;
    }
    [[nodiscard]] int authorizedReadCount() const
    {
        return m_AuthorizedReadCount;
    }

  private:
    bool m_CanReadAuthorized = false;
    int m_AuthorizedReadCount = 0;
    Platform::ConnectionsReadResult m_AuthorizedResult;
    bool m_HasConnections = true;
    Platform::ConnectionsReadResult m_Result{.status = Platform::ConnectionsReadStatus::Ok, .connections = {}, .detail = {}};
    int m_ReadCount = 0;
    Platform::ProcessTarget m_LastTarget{};
};

/// Mock IProcessModulesReader (#802): returns a configured result and counts the reads.
class MockProcessModulesReader : public Platform::IProcessModulesReader
{
  public:
    void setResult(Platform::ModulesReadResult result)
    {
        m_Result = std::move(result);
    }

    [[nodiscard]] bool hasModules() const override
    {
        return true;
    }

    [[nodiscard]] Platform::ModulesReadResult readModules(const Platform::ProcessTarget& /*target*/) override
    {
        ++m_ReadCount;
        return m_Result;
    }

    [[nodiscard]] int readCount() const
    {
        return m_ReadCount;
    }

    /// readAuthorized() (#2309): offered when set, returning @p result and counting its calls.
    void setAuthorizedResult(Platform::ModulesReadResult result)
    {
        m_AuthorizedResult = std::move(result);
        m_CanReadAuthorized = true;
    }
    [[nodiscard]] bool canReadAuthorized() const override
    {
        return m_CanReadAuthorized;
    }
    [[nodiscard]] Platform::ModulesReadResult readAuthorized(const Platform::ProcessTarget& /*target*/,
                                                             const std::stop_token& /*stopToken*/) override
    {
        ++m_AuthorizedReadCount;
        return m_AuthorizedResult;
    }
    [[nodiscard]] int authorizedReadCount() const
    {
        return m_AuthorizedReadCount;
    }

  private:
    bool m_CanReadAuthorized = false;
    int m_AuthorizedReadCount = 0;
    Platform::ModulesReadResult m_AuthorizedResult;
    Platform::ModulesReadResult m_Result{.status = Platform::ModulesReadStatus::Ok, .modules = {}, .detail = {}};
    int m_ReadCount = 0;
};

/// Mock IProcessSecurityReader (#1526): returns the result it was given and counts the reads.
class MockProcessSecurityReader : public Platform::IProcessSecurityReader
{
  public:
    void setResult(Platform::SecurityReadResult result)
    {
        m_Result = std::move(result);
    }

    [[nodiscard]] bool hasSecurity() const override
    {
        return true;
    }

    [[nodiscard]] Platform::SecurityReadResult readSecurity(const Platform::ProcessTarget& /*target*/) override
    {
        ++m_ReadCount;
        return m_Result;
    }

    [[nodiscard]] int readCount() const
    {
        return m_ReadCount;
    }

  private:
    Platform::SecurityReadResult m_Result{.status = Platform::SecurityReadStatus::Ok, .security = {}, .detail = {}};
    int m_ReadCount = 0;
};

/// Mock IProcessOpenFilesReader (#183): returns a configured result and counts the reads.
class MockProcessOpenFilesReader : public Platform::IProcessOpenFilesReader
{
  public:
    void setResult(Platform::OpenFilesReadResult result)
    {
        m_Result = std::move(result);
    }

    [[nodiscard]] bool hasOpenFiles() const override
    {
        return true;
    }

    [[nodiscard]] Platform::OpenFilesReadResult readOpenFiles(const Platform::ProcessTarget& /*target*/) override
    {
        ++m_ReadCount;
        return m_Result;
    }

    [[nodiscard]] int readCount() const
    {
        return m_ReadCount;
    }

    /// readAuthorized() (#2309): offered when set, returning @p result and counting its calls.
    void setAuthorizedResult(Platform::OpenFilesReadResult result)
    {
        m_AuthorizedResult = std::move(result);
        m_CanReadAuthorized = true;
    }
    [[nodiscard]] bool canReadAuthorized() const override
    {
        return m_CanReadAuthorized;
    }
    [[nodiscard]] Platform::OpenFilesReadResult readAuthorized(const Platform::ProcessTarget& /*target*/,
                                                               const std::stop_token& /*stopToken*/) override
    {
        ++m_AuthorizedReadCount;
        return m_AuthorizedResult;
    }
    [[nodiscard]] int authorizedReadCount() const
    {
        return m_AuthorizedReadCount;
    }

  private:
    bool m_CanReadAuthorized = false;
    int m_AuthorizedReadCount = 0;
    Platform::OpenFilesReadResult m_AuthorizedResult;
    Platform::OpenFilesReadResult m_Result;
    int m_ReadCount = 0;
};

// =============================================================================
// Services and Startup tabs (#1721): probes read on a sampler thread, actions run on a worker
// =============================================================================

/// How long a mock's wait gives up after: a bound for a broken build only, never reached when the
/// code under test works. Every wait below is woken by the event itself, never by polling or sleeping.
inline constexpr std::chrono::seconds MOCK_WAIT_LIMIT{10};

/// Counts calls made on another thread and lets a test wait, without sleeping, until there have been
/// at least n.
class CallSignal
{
  public:
    void bump()
    {
        {
            const std::scoped_lock lock(m_Mutex);
            ++m_Count;
        }
        m_Changed.notify_all();
    }

    [[nodiscard]] int count() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Count;
    }

    /// True once there have been at least @p atLeast calls; false after MOCK_WAIT_LIMIT.
    [[nodiscard]] bool waitFor(int atLeast) const
    {
        std::unique_lock lock(m_Mutex);
        return m_Changed.wait_for(lock, MOCK_WAIT_LIMIT, [&] { return m_Count >= atLeast; });
    }

  private:
    mutable std::mutex m_Mutex;
    mutable std::condition_variable m_Changed;
    int m_Count = 0;
};

/// A service probe answering what setEnumeration() gave it (thread-safe: the sampler thread reads it),
/// counting enumerate() and forgetCachedConfig() calls. Its capabilities are fixed at construction, as
/// the model reads them once.
class MockServiceProbe : public Platform::IServiceProbe
{
  public:
    explicit MockServiceProbe(Platform::ServiceCapabilities capabilities) : m_Capabilities(std::move(capabilities))
    {}

    void setEnumeration(Platform::ServiceEnumeration enumeration)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Enumeration = std::move(enumeration);
    }

    [[nodiscard]] Platform::ServiceCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

    [[nodiscard]] Platform::ServiceEnumeration enumerate() override
    {
        Platform::ServiceEnumeration copy;
        {
            const std::scoped_lock lock(m_Mutex);
            copy = m_Enumeration;
        }
        enumerations.bump();
        return copy;
    }

    void forgetCachedConfig() override
    {
        configForgets.bump();
    }

    CallSignal enumerations;  ///< enumerate() calls
    CallSignal configForgets; ///< forgetCachedConfig() calls

  private:
    Platform::ServiceCapabilities m_Capabilities;
    std::mutex m_Mutex;
    Platform::ServiceEnumeration m_Enumeration = Platform::ServiceEnumeration::succeeded({});
};

/// A startup probe answering what setEntries() gave it (thread-safe), counting enumerate() calls.
class MockStartupProbe : public Platform::IStartupProbe
{
  public:
    explicit MockStartupProbe(Platform::StartupCapabilities capabilities) : m_Capabilities(std::move(capabilities))
    {}

    void setEntries(std::vector<Platform::StartupEntry> entries)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Entries = std::move(entries);
    }

    [[nodiscard]] Platform::StartupCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

    [[nodiscard]] std::vector<Platform::StartupEntry> enumerate() override
    {
        std::vector<Platform::StartupEntry> copy;
        {
            const std::scoped_lock lock(m_Mutex);
            copy = m_Entries;
        }
        enumerations.bump();
        return copy;
    }

    CallSignal enumerations; ///< enumerate() calls

  private:
    Platform::StartupCapabilities m_Capabilities;
    std::mutex m_Mutex;
    std::vector<Platform::StartupEntry> m_Entries;
};

/// Service actions that never touch a real service: each answers setResult()'s result and records the
/// call. With blockUntilCancelled(), Start / Stop / Restart instead wait, as a slow service's state
/// change does, until their stop token is stopped, then answer stopRequested() as WindowsServiceActions
/// does.
class MockServiceActions : public Platform::IServiceActions
{
  public:
    explicit MockServiceActions(Platform::ServiceActionCapabilities capabilities) : m_Capabilities(capabilities)
    {}

    void setResult(Platform::ServiceActionResult result)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Result = std::move(result);
    }

    void blockUntilCancelled()
    {
        m_Block = true;
    }

    [[nodiscard]] std::string lastName() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_LastName;
    }

    [[nodiscard]] Platform::ServiceActionCapabilities capabilities() const override
    {
        return m_Capabilities;
    }
    [[nodiscard]] Platform::ServiceActionResult start(std::string_view name, const std::stop_token& stopToken) override
    {
        return answer(name, stopToken, starts);
    }
    [[nodiscard]] Platform::ServiceActionResult stop(std::string_view name, const std::stop_token& stopToken) override
    {
        return answer(name, stopToken, stops);
    }
    [[nodiscard]] Platform::ServiceActionResult restart(std::string_view name, const std::stop_token& stopToken) override
    {
        return answer(name, stopToken, restarts);
    }
    [[nodiscard]] Platform::ServiceActionResult setStartType(std::string_view name, Platform::ServiceStartType /*type*/) override
    {
        return answer(name, std::stop_token{}, startTypes);
    }

    CallSignal starts;
    CallSignal stops;
    CallSignal restarts;
    CallSignal startTypes;
    std::atomic<bool> sawStop{false}; ///< A blocked action ended because its stop token was stopped.

  private:
    [[nodiscard]] Platform::ServiceActionResult answer(std::string_view name, const std::stop_token& stopToken, CallSignal& calls)
    {
        {
            const std::scoped_lock lock(m_Mutex);
            m_LastName = name;
        }
        calls.bump();
        if (m_Block && stopToken.stop_possible())
        {
            std::unique_lock lock(m_Mutex);
            // Woken by the stop request itself; the predicate is the stop, so a spurious wake waits on.
            static_cast<void>(m_Wake.wait_for(lock, stopToken, MOCK_WAIT_LIMIT, [] { return false; }));
            if (!stopToken.stop_requested())
            {
                return Platform::ServiceActionResult::failed("not cancelled");
            }
            sawStop = true;
            return Platform::ServiceActionResult::stopRequested();
        }
        const std::scoped_lock lock(m_Mutex);
        return m_Result;
    }

    Platform::ServiceActionCapabilities m_Capabilities;
    std::atomic<bool> m_Block{false};
    mutable std::mutex m_Mutex;
    std::condition_variable_any m_Wake;
    Platform::ServiceActionResult m_Result = Platform::ServiceActionResult::succeeded();
    std::string m_LastName;
};

/// Startup actions that never touch the registry or a Startup folder: setEnabled() answers setResult()'s
/// result and records the call. With holdUntilReleased(), it first waits for release() (a write in
/// flight).
class MockStartupActions : public Platform::IStartupActions
{
  public:
    explicit MockStartupActions(Platform::StartupActionCapabilities capabilities) : m_Capabilities(capabilities)
    {}

    void setResult(Platform::StartupActionResult result)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Result = std::move(result);
    }

    void holdUntilReleased()
    {
        m_Hold = true;
    }

    void release()
    {
        {
            const std::scoped_lock lock(m_Mutex);
            m_Released = true;
        }
        m_Wake.notify_all();
    }

    [[nodiscard]] std::string lastName() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_LastName;
    }

    [[nodiscard]] Platform::StartupActionCapabilities capabilities() const override
    {
        return m_Capabilities;
    }
    [[nodiscard]] Platform::StartupActionResult setEnabled(const Platform::StartupEntry& entry, bool enabled) override
    {
        {
            const std::scoped_lock lock(m_Mutex);
            m_LastName = entry.name;
        }
        (enabled ? enables : disables).bump();
        std::unique_lock lock(m_Mutex);
        if (m_Hold)
        {
            static_cast<void>(m_Wake.wait_for(lock, MOCK_WAIT_LIMIT, [this] { return m_Released; }));
        }
        returned = true;
        return m_Result;
    }

    CallSignal enables;
    CallSignal disables;
    std::atomic<bool> returned{false}; ///< setEnabled() has finished

  private:
    Platform::StartupActionCapabilities m_Capabilities;
    std::atomic<bool> m_Hold{false};
    mutable std::mutex m_Mutex;
    std::condition_variable m_Wake;
    bool m_Released = false;
    Platform::StartupActionResult m_Result = Platform::StartupActionResult::succeeded();
    std::string m_LastName;
};

} // namespace TestMocks
