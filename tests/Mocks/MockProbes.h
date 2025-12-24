/// @file MockProbes.h
/// @brief Shared mock implementations for platform probes used in unit tests.
///
/// This header provides reusable mock classes for IProcessProbe and ISystemProbe,
/// along with helper functions for creating test data.

#pragma once

#include "Platform/IProcessProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SystemTypes.h"

#include <atomic>
#include <cstdint>
#include <string>
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
        m_Counters.push_back(makeProcessCounters(pid, name));
        return *this;
    }

    MockProcessProbe& withProcess(Platform::ProcessCounters counter)
    {
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

    MockProcessProbe& withPriority(int32_t pid, int32_t nice, int32_t basePriority)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.pid == pid)
            {
                counter.nice = nice;
                counter.basePriority = basePriority;
                return *this;
            }
        }
        // If process doesn't exist, create it
        auto c = makeProcessCounters(pid, "process_" + std::to_string(pid));
        c.nice = nice;
        c.basePriority = basePriority;
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
    std::vector<Platform::ProcessCounters> m_Counters;
    uint64_t m_TotalCpuTime = 0;
    uint64_t m_SystemTotalMemory = 8ULL * 1024 * 1024 * 1024; // Default 8 GB
    Platform::ProcessCapabilities m_Capabilities;
    long m_TicksPerSecond = 100; // Standard HZ value
    std::atomic<int> m_EnumerateCount{0};
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

/// Create a complete SystemCounters struct.
inline Platform::SystemCounters makeSystemCounters(Platform::CpuCounters cpu,
                                                   Platform::MemoryCounters memory,
                                                   uint64_t uptime = 0,
                                                   std::vector<Platform::CpuCounters> perCore = {})
{
    Platform::SystemCounters s;
    s.cpuTotal = cpu;
    s.memory = memory;
    s.uptimeSeconds = uptime;
    s.cpuPerCore = std::move(perCore);
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
    return caps;
}

/// Create SystemCapabilities with all features enabled.
inline Platform::SystemCapabilities makeFullSystemCapabilities()
{
    Platform::SystemCapabilities caps;
    caps.hasPerCoreCpu = true;
    caps.hasSwap = true;
    caps.hasIoWait = true;
    return caps;
}

} // namespace TestMocks
