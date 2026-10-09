/// @file MockProbes.h
/// @brief Shared mock implementations for platform probes used in unit tests.
///
/// This header provides reusable mock classes for IProcessProbe and ISystemProbe,
/// along with helper functions for creating test data.

#pragma once

#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/PowerTypes.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SystemTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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
        m_PackageEnergy = reading;
    }

    [[nodiscard]] std::optional<Platform::PackageEnergyReading> readPackageEnergy() const override
    {
        return m_PackageEnergy;
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
    mutable std::optional<Platform::ProcessCapabilities> m_CapabilitiesAfterSocketRead;
    long m_TicksPerSecond = 100; // Standard HZ value
    std::atomic<int> m_EnumerateCount{0};
    std::optional<Platform::PackageEnergyReading> m_PackageEnergy;
    Platform::SocketTrafficReading m_SocketTraffic;
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

  private:
    Platform::ProcessActionCapabilities m_Capabilities;
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
    int m_SetPriorityCount = 0;
    int m_SetIoPriorityCount = 0;
    int m_GetIoPriorityCount = 0;
    Platform::IoPriority m_LastSetIoPriority;
    Platform::ProcessTarget m_LastIoPriorityReadTarget;
    int m_SyscallTraceCount = 0;
    Platform::ProcessActionResult m_SyscallTraceResult = Platform::ProcessActionResult::ok();
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

    [[nodiscard]] Platform::EnvironmentReadResult readEnvironment(const Platform::ProcessTarget& target) override
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

  private:
    bool m_HasEnvironment = true;
    Platform::EnvironmentReadResult m_Result{.status = Platform::EnvironmentReadStatus::Ok, .variables = {}};
    int m_ReadCount = 0;
    Platform::ProcessTarget m_LastTarget{};
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

  private:
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

  private:
    Platform::ModulesReadResult m_Result{.status = Platform::ModulesReadStatus::Ok, .modules = {}, .detail = {}};
    int m_ReadCount = 0;
};

} // namespace TestMocks
