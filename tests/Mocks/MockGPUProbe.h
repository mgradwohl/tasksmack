/// @file MockGPUProbe.h
/// @brief Mock implementation of IGPUProbe for unit testing.

#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace TestMocks
{

/// Create a GPUInfo struct with common test values.
inline Platform::GPUInfo
makeGPUInfo(const std::string& id, const std::string& name, const std::string& vendor = "Test", bool isIntegrated = false)
{
    Platform::GPUInfo info;
    info.id = id;
    info.name = name;
    info.vendor = vendor;
    info.isIntegrated = isIntegrated;
    info.driverVersion = "1.0.0";
    info.deviceIndex = 0;
    return info;
}

/// Create a GPUCounters struct with common test values.
inline Platform::GPUCounters makeGPUCounters(const std::string& gpuId,
                                             double utilization = 50.0,
                                             std::uint64_t memoryUsed = 1024 * 1024 * 1024,
                                             std::uint64_t memoryTotal = 4ULL * 1024 * 1024 * 1024)
{
    Platform::GPUCounters c;
    c.gpuId = gpuId;
    c.utilizationPercent = utilization;
    // memoryUtilPercent removed - computed by Domain layer
    c.memoryUsedBytes = memoryUsed;
    c.memoryTotalBytes = memoryTotal;
    c.temperatureC = 60;
    c.hotspotTempC = 65;
    c.powerDrawWatts = 150.0;
    c.powerLimitWatts = 250.0;
    c.gpuClockMHz = 1500;
    c.memoryClockMHz = 7000;
    c.fanSpeedRaw = 55;
    c.fanSpeedMaxRaw = 100;
    c.pcieTxBytes = 0;
    c.pcieRxBytes = 0;
    c.computeUtilPercent = 0.0;
    c.encoderUtilPercent = 0.0;
    c.decoderUtilPercent = 0.0;
    return c;
}

/// Create a ProcessGPUCounters struct with common test values.
inline Platform::ProcessGPUCounters
makeProcessGPUCounters(std::int32_t pid, const std::string& gpuId, std::uint64_t memoryBytes = 512 * 1024 * 1024)
{
    Platform::ProcessGPUCounters c;
    c.pid = pid;
    c.gpuId = gpuId;
    c.gpuMemoryBytes = memoryBytes;
    c.gpuUtilPercent = 25.0;
    c.encoderUtilPercent = 0.0;
    c.decoderUtilPercent = 0.0;
    c.activeEngines = {"3D"};
    return c;
}

/// Mock implementation of IGPUProbe for testing.
/// Allows controlled injection of GPU data and tracks call counts.
/// Supports fluent builder API for convenient test setup.
class MockGPUProbe : public Platform::IGPUProbe
{
  public:
    // Builder pattern methods for fluent API
    MockGPUProbe& withGPU(const std::string& id, const std::string& name, const std::string& vendor = "Test", bool isIntegrated = false)
    {
        m_GPUInfo.push_back(makeGPUInfo(id, name, vendor, isIntegrated));
        m_Counters.push_back(makeGPUCounters(id));
        return *this;
    }

    MockGPUProbe& withGPUCounters(const std::string& gpuId, Platform::GPUCounters counters)
    {
        for (auto& existing : m_Counters)
        {
            if (existing.gpuId == gpuId)
            {
                existing = std::move(counters);
                return *this;
            }
        }
        m_Counters.push_back(std::move(counters));
        return *this;
    }

    /// Stop reporting counters for `gpuId`, as if it had dropped out (it stays enumerated).
    MockGPUProbe& withoutGPUCounters(const std::string& gpuId)
    {
        std::erase_if(m_Counters, [&gpuId](const auto& counter) { return counter.gpuId == gpuId; });
        return *this;
    }

    MockGPUProbe& withUtilization(const std::string& gpuId, double util)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.gpuId == gpuId)
            {
                counter.utilizationPercent = util;
                return *this;
            }
        }
        return *this;
    }

    MockGPUProbe& withMemory(const std::string& gpuId, std::uint64_t used, std::uint64_t total)
    {
        for (auto& counter : m_Counters)
        {
            if (counter.gpuId == gpuId)
            {
                counter.memoryUsedBytes = used;
                counter.memoryTotalBytes = total;
                return *this;
            }
        }
        return *this;
    }

    MockGPUProbe& withProcessGPU(std::int32_t pid, const std::string& gpuId, std::uint64_t memoryBytes)
    {
        m_ProcessCounters.push_back(makeProcessGPUCounters(pid, gpuId, memoryBytes));
        return *this;
    }

    /// A per-process entry with every field chosen by the test (shared memory, utilization, ...).
    MockGPUProbe& withProcessGPUCounters(Platform::ProcessGPUCounters counters)
    {
        m_ProcessCounters.push_back(std::move(counters));
        return *this;
    }

    MockGPUProbe& withCapabilities(Platform::GPUCapabilities caps)
    {
        m_Capabilities = caps;
        return *this;
    }

    /// Makes capabilities() throw once (simulating a transient probe query failure),
    /// resetting the throw-once flag when it fires so subsequent calls succeed normally.
    MockGPUProbe& withCapabilitiesQueryThrowingOnce()
    {
        m_ThrowOnNextCapabilitiesQuery = true;
        return *this;
    }

    /// Makes enumerateGPUs() throw, simulating a probe that cannot list its devices.
    MockGPUProbe& withEnumerationThrowing()
    {
        m_ThrowOnEnumerate = true;
        return *this;
    }

    /// Undoes withEnumerationThrowing(): enumerateGPUs() succeeds again.
    MockGPUProbe& withEnumerationSucceeding()
    {
        m_ThrowOnEnumerate = false;
        return *this;
    }

    /// Removes `gpuId` entirely: no longer enumerated and no longer reporting counters.
    MockGPUProbe& withoutGPU(const std::string& gpuId)
    {
        std::erase_if(m_GPUInfo, [&gpuId](const auto& info) { return info.id == gpuId; });
        return withoutGPUCounters(gpuId);
    }

    /// Sets the per-adapter sensor set enumerateGPUs() reports for `gpuId` (#1112, #1289).
    MockGPUProbe& withSensorCapabilities(const std::string& gpuId, std::optional<Platform::GPUCapabilities> sensors)
    {
        for (auto& info : m_GPUInfo)
        {
            if (info.id == gpuId)
            {
                info.sensorCapabilities = sensors;
            }
        }
        return *this;
    }

    /// Makes the next rescanGPUs() report a change (once), as a probe does when its GPU set or an
    /// adapter's GPUInfo may have changed (#1116).
    MockGPUProbe& withRescanReportingChange()
    {
        m_RescanReportsChange = true;
        return *this;
    }

    /// Makes readProcessGPUCounters() throw on every call, simulating a per-process query that
    /// keeps failing (#1142).
    MockGPUProbe& withProcessCountersThrowing()
    {
        m_ThrowOnReadProcessCounters = true;
        return *this;
    }

    // IGPUProbe interface implementation
    [[nodiscard]] std::vector<Platform::GPUInfo> enumerateGPUs() override
    {
        ++m_EnumerateCount;
        if (m_ThrowOnEnumerate)
        {
            throw std::runtime_error("MockGPUProbe: simulated enumerateGPUs() failure");
        }
        return m_GPUInfo;
    }

    [[nodiscard]] std::vector<Platform::GPUCounters> readGPUCounters() override
    {
        ++m_ReadCountersCount;
        if (m_BlockReadCounters.load(std::memory_order_acquire))
        {
            m_EnteredBlockedReadCounters.store(true, std::memory_order_release);
            std::unique_lock lock(m_BlockMutex);
            m_BlockCv.wait(lock, [this] { return m_ReleaseRequested; });
        }
        return m_Counters;
    }

    [[nodiscard]] std::vector<Platform::ProcessGPUCounters> readProcessGPUCounters() override
    {
        ++m_ReadProcessCountersCount;
        if (m_ThrowOnReadProcessCounters)
        {
            throw std::runtime_error("MockGPUProbe: simulated readProcessGPUCounters() failure");
        }
        return m_ProcessCounters;
    }

    [[nodiscard]] bool rescanGPUs(Platform::GPURescan depth) override
    {
        if (depth == Platform::GPURescan::Full)
        {
            ++m_FullRescanCount;
        }
        else
        {
            ++m_QuickRescanCount;
        }
        return std::exchange(m_RescanReportsChange, false);
    }

    [[nodiscard]] Platform::GPUCapabilities capabilities() const override
    {
        if (m_ThrowOnNextCapabilitiesQuery)
        {
            m_ThrowOnNextCapabilitiesQuery = false;
            throw std::runtime_error("MockGPUProbe: simulated capabilities() query failure");
        }
        return m_Capabilities;
    }

    // Test helper methods
    void clearGPUs()
    {
        m_GPUInfo.clear();
        m_Counters.clear();
        m_ProcessCounters.clear();
    }

    [[nodiscard]] std::uint32_t enumerateCallCount() const
    {
        return m_EnumerateCount.load();
    }
    [[nodiscard]] std::uint32_t readCountersCallCount() const
    {
        return m_ReadCountersCount.load();
    }
    [[nodiscard]] std::uint32_t readProcessCountersCallCount() const
    {
        return m_ReadProcessCountersCount.load();
    }
    [[nodiscard]] std::uint32_t quickRescanCount() const
    {
        return m_QuickRescanCount;
    }
    [[nodiscard]] std::uint32_t fullRescanCount() const
    {
        return m_FullRescanCount;
    }

    /// Makes the next (and all subsequent, until released) readGPUCounters() call block
    /// indefinitely once entered, so a test can hold whatever lock the caller (GPUModel)
    /// takes around that call from a background thread, for as long as it needs to. Resets
    /// state left over from a prior arm/release cycle first, so this mock can be re-armed
    /// and reused within a single test.
    void armBlockingReadGPUCounters()
    {
        {
            const std::scoped_lock lock(m_BlockMutex);
            m_ReleaseRequested = false;
        }
        m_EnteredBlockedReadCounters.store(false, std::memory_order_release);
        m_BlockReadCounters.store(true, std::memory_order_release);
    }

    /// Releases a call currently blocked inside readGPUCounters() (see
    /// armBlockingReadGPUCounters()) and lets future calls proceed without blocking.
    void releaseBlockedReadGPUCounters()
    {
        m_BlockReadCounters.store(false, std::memory_order_release);
        {
            const std::scoped_lock lock(m_BlockMutex);
            m_ReleaseRequested = true;
        }
        m_BlockCv.notify_all();
    }

    /// True once a readGPUCounters() call armed by armBlockingReadGPUCounters() has
    /// actually entered its blocking wait -- i.e. the caller is now holding whatever lock
    /// it takes around the call. Lets a test avoid racing its own timing measurement
    /// against the background thread merely being scheduled.
    [[nodiscard]] bool hasEnteredBlockedReadGPUCounters() const
    {
        return m_EnteredBlockedReadCounters.load(std::memory_order_acquire);
    }

  private:
    std::vector<Platform::GPUInfo> m_GPUInfo;
    std::vector<Platform::GPUCounters> m_Counters;
    std::vector<Platform::ProcessGPUCounters> m_ProcessCounters;
    Platform::GPUCapabilities m_Capabilities;
    mutable bool m_ThrowOnNextCapabilitiesQuery = false;
    bool m_ThrowOnEnumerate = false;
    bool m_RescanReportsChange = false;
    std::uint32_t m_QuickRescanCount = 0;
    std::uint32_t m_FullRescanCount = 0;
    bool m_ThrowOnReadProcessCounters = false;

    std::atomic<std::uint32_t> m_EnumerateCount{0};
    std::atomic<std::uint32_t> m_ReadCountersCount{0};
    std::atomic<std::uint32_t> m_ReadProcessCountersCount{0};

    std::atomic<bool> m_BlockReadCounters{false};
    std::atomic<bool> m_EnteredBlockedReadCounters{false};
    std::mutex m_BlockMutex;
    std::condition_variable m_BlockCv;
    bool m_ReleaseRequested = false;
};

} // namespace TestMocks
