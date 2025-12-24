/// @file MockDiskProbe.h
/// @brief Mock implementation of IDiskProbe for testing

#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/StorageTypes.h"

#include <atomic>

namespace Mocks
{

/// Mock implementation of IDiskProbe for testing.
/// Allows controlled injection of disk I/O data.
class MockDiskProbe : public Platform::IDiskProbe
{
  public:
    MockDiskProbe() = default;
    ~MockDiskProbe() override = default;

    MockDiskProbe(const MockDiskProbe&) = delete;
    MockDiskProbe& operator=(const MockDiskProbe&) = delete;
    MockDiskProbe(MockDiskProbe&&) = delete;
    MockDiskProbe& operator=(MockDiskProbe&&) = delete;

    [[nodiscard]] Platform::SystemDiskCounters read() override
    {
        m_ReadCount++;
        return m_NextCounters;
    }

    [[nodiscard]] Platform::DiskCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

    // Test control methods
    void setNextCounters(const Platform::SystemDiskCounters& counters)
    {
        m_NextCounters = counters;
    }

    void setCapabilities(const Platform::DiskCapabilities& caps)
    {
        m_Capabilities = caps;
    }

    [[nodiscard]] int readCount() const
    {
        return m_ReadCount.load();
    }

  private:
    Platform::SystemDiskCounters m_NextCounters;
    Platform::DiskCapabilities m_Capabilities{
        .hasDiskStats = true, .hasReadWriteBytes = true, .hasIoTime = true, .hasDeviceInfo = true, .canFilterPhysical = true};
    std::atomic<int> m_ReadCount{0};
};

} // namespace Mocks
