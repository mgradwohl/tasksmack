/// @file MockGPUProbe.h
/// @brief Mock implementation of IGPUProbe for unit testing.

#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
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
    c.fanSpeedRPMPercent = 1200;
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
    MockGPUProbe& withGPU(const std::string& id, const std::string& name, const std::string& vendor = "Test")
    {
        m_GPUInfo.push_back(makeGPUInfo(id, name, vendor));
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

    MockGPUProbe& withCapabilities(Platform::GPUCapabilities caps)
    {
        m_Capabilities = caps;
        return *this;
    }

    // IGPUProbe interface implementation
    [[nodiscard]] std::vector<Platform::GPUInfo> enumerateGPUs() override
    {
        ++m_EnumerateCount;
        return m_GPUInfo;
    }

    [[nodiscard]] std::vector<Platform::GPUCounters> readGPUCounters() override
    {
        ++m_ReadCountersCount;
        return m_Counters;
    }

    [[nodiscard]] std::vector<Platform::ProcessGPUCounters> readProcessGPUCounters() override
    {
        ++m_ReadProcessCountersCount;
        return m_ProcessCounters;
    }

    [[nodiscard]] Platform::GPUCapabilities capabilities() const override
    {
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

  private:
    std::vector<Platform::GPUInfo> m_GPUInfo;
    std::vector<Platform::GPUCounters> m_Counters;
    std::vector<Platform::ProcessGPUCounters> m_ProcessCounters;
    Platform::GPUCapabilities m_Capabilities;

    std::atomic<std::uint32_t> m_EnumerateCount{0};
    std::atomic<std::uint32_t> m_ReadCountersCount{0};
    std::atomic<std::uint32_t> m_ReadProcessCountersCount{0};
};

} // namespace TestMocks
