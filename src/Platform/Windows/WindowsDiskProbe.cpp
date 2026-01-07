#include "WindowsDiskProbe.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>
// clang-format on

#pragma comment(lib, "pdh.lib")

#include "WinString.h"

#include <algorithm>
#include <string>
#include <vector>

namespace Platform
{

// Pimpl struct containing Windows-specific types
struct WindowsDiskProbe::Impl
{
    struct DiskCounterSet
    {
        std::string instanceName;
        PDH_HCOUNTER readBytesCounter = nullptr;
        PDH_HCOUNTER writeBytesCounter = nullptr;
        PDH_HCOUNTER readsCounter = nullptr;
        PDH_HCOUNTER writesCounter = nullptr;
        PDH_HCOUNTER idleTimeCounter = nullptr;
    };

    PDH_HQUERY query = nullptr;
    std::vector<DiskCounterSet> diskCounters;
};

namespace
{

/// Helper to extract double value from PDH counter union.
/// @param counter PDH counter handle
/// @return The double value on success, 0.0 on failure (and logs on failure)
[[nodiscard]] double getPdhDoubleValue(PDH_HCOUNTER counter)
{
    PDH_FMT_COUNTERVALUE value{}; // Zero-initialize for safety
    const PDH_STATUS status = PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value);
    if (status == ERROR_SUCCESS)
    {
        return value.doubleValue; // NOLINT(cppcoreguidelines-pro-type-union-access)
    }

    spdlog::error("WindowsDiskProbe: PdhGetFormattedCounterValue failed with status {}", status);
    return 0.0;
}

} // namespace

WindowsDiskProbe::WindowsDiskProbe() : m_Impl(std::make_unique<Impl>())
{
    spdlog::debug("WindowsDiskProbe: initialized");

    // Initialize PDH query
    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &m_Impl->query);
    if (status != ERROR_SUCCESS)
    {
        spdlog::error("WindowsDiskProbe: PdhOpenQuery failed with status {}", status);
        m_Impl->query = nullptr;
        return;
    }

    // Enumerate physical disks
    // PdhEnumObjectItemsW requires both counter and instance buffers on the second call.
    // First call: determine buffer sizes needed
    DWORD counterBufferSize = 0;
    DWORD instanceBufferSize = 0;
    status = PdhEnumObjectItemsW(
        nullptr, nullptr, L"PhysicalDisk", nullptr, &counterBufferSize, nullptr, &instanceBufferSize, PERF_DETAIL_WIZARD, 0);

    if (static_cast<DWORD>(status) == PDH_MORE_DATA && instanceBufferSize != 0U)
    {
        // Second call: retrieve actual data with properly sized buffers
        std::vector<wchar_t> counterBuffer(counterBufferSize);
        std::vector<wchar_t> instanceBuffer(instanceBufferSize);
        DWORD counterSize = counterBufferSize;
        DWORD instanceSize = instanceBufferSize;

        status = PdhEnumObjectItemsW(
            nullptr, nullptr, L"PhysicalDisk", counterBuffer.data(), &counterSize, instanceBuffer.data(), &instanceSize, PERF_DETAIL_WIZARD, 0);

        if (status == ERROR_SUCCESS)
        {
            // Parse instance names (null-separated list)
            const wchar_t* instance = instanceBuffer.data();
            while (*instance != L'\0')
            {
                const std::wstring instanceName(instance);

                // Skip "_Total" instance
                if (instanceName == L"_Total")
                {
                    instance += instanceName.length() + 1;
                    continue;
                }

                Impl::DiskCounterSet counterSet;
                counterSet.instanceName = WinString::wideToUtf8(instanceName);

                // Add counters for this disk
                const std::wstring readBytesPath = L"\\PhysicalDisk(" + instanceName + L")\\Disk Read Bytes/sec";
                const std::wstring writeBytesPath = L"\\PhysicalDisk(" + instanceName + L")\\Disk Write Bytes/sec";
                const std::wstring readsPath = L"\\PhysicalDisk(" + instanceName + L")\\Disk Reads/sec";
                const std::wstring writesPath = L"\\PhysicalDisk(" + instanceName + L")\\Disk Writes/sec";
                const std::wstring idleTimePath = L"\\PhysicalDisk(" + instanceName + L")\\% Idle Time";

                PdhAddCounterW(m_Impl->query, readBytesPath.c_str(), 0, &counterSet.readBytesCounter);
                PdhAddCounterW(m_Impl->query, writeBytesPath.c_str(), 0, &counterSet.writeBytesCounter);
                PdhAddCounterW(m_Impl->query, readsPath.c_str(), 0, &counterSet.readsCounter);
                PdhAddCounterW(m_Impl->query, writesPath.c_str(), 0, &counterSet.writesCounter);
                PdhAddCounterW(m_Impl->query, idleTimePath.c_str(), 0, &counterSet.idleTimeCounter);

                m_Impl->diskCounters.push_back(counterSet);

                instance += instanceName.length() + 1;
            }
        }
    }

    // Collect initial sample
    if (m_Impl->query != nullptr)
    {
        PdhCollectQueryData(m_Impl->query);
    }

    spdlog::debug("WindowsDiskProbe: initialized with {} disks", m_Impl->diskCounters.size());
}

WindowsDiskProbe::~WindowsDiskProbe()
{
    if (m_Impl && m_Impl->query != nullptr)
    {
        PdhCloseQuery(m_Impl->query);
    }
}

SystemDiskCounters WindowsDiskProbe::read()
{
    SystemDiskCounters result;

    if (!m_Impl || m_Impl->query == nullptr || m_Impl->diskCounters.empty())
    {
        // Fallback: enumerate logical drives
        const DWORD drives = GetLogicalDrives();
        if (drives == 0)
        {
            spdlog::warn("WindowsDiskProbe: GetLogicalDrives failed");
            return result;
        }

        for (int i = 0; i < 26; ++i)
        {
            if ((drives & (1U << i)) == 0U)
            {
                continue;
            }

            const wchar_t driveLetter = static_cast<wchar_t>('A' + i);
            const std::wstring drivePath = std::wstring{driveLetter} + L":\\";

            const UINT driveType = GetDriveTypeW(drivePath.c_str());

            // Only include fixed drives
            if (driveType != DRIVE_FIXED)
            {
                continue;
            }

            DiskCounters disk;
            disk.deviceName = WinString::wideToUtf8(std::wstring{driveLetter} + L":");
            disk.readsCompleted = 0;
            disk.readSectors = 0;
            disk.readTimeMs = 0;
            disk.writesCompleted = 0;
            disk.writeSectors = 0;
            disk.writeTimeMs = 0;
            disk.ioInProgressMs = 0;
            disk.ioTimeMs = 0;
            disk.weightedIoTimeMs = 0;
            disk.sectorSize = 512;
            disk.isPhysicalDevice = true;

            result.disks.push_back(disk);
        }

        return result;
    }

    // Collect new sample from PDH
    PDH_STATUS status = PdhCollectQueryData(m_Impl->query);
    if (status != ERROR_SUCCESS)
    {
        spdlog::warn("WindowsDiskProbe: PdhCollectQueryData failed with status {}", status);
        return result;
    }

    // Read counter values
    for (const auto& counterSet : m_Impl->diskCounters)
    {
        DiskCounters disk;
        disk.deviceName = counterSet.instanceName;
        disk.sectorSize = 512;
        disk.isPhysicalDevice = true;

        // Read bytes/sec
        const double readBytesPerSec = getPdhDoubleValue(counterSet.readBytesCounter);
        disk.readSectors = static_cast<uint64_t>(readBytesPerSec / static_cast<double>(disk.sectorSize));

        // Write bytes/sec
        const double writeBytesPerSec = getPdhDoubleValue(counterSet.writeBytesCounter);
        disk.writeSectors = static_cast<uint64_t>(writeBytesPerSec / static_cast<double>(disk.sectorSize));

        // Read operations/sec
        const double readsPerSec = getPdhDoubleValue(counterSet.readsCounter);
        disk.readsCompleted = static_cast<uint64_t>(readsPerSec);

        // Write operations/sec
        const double writesPerSec = getPdhDoubleValue(counterSet.writesCounter);
        disk.writesCompleted = static_cast<uint64_t>(writesPerSec);

        // Idle time (convert to busy time for ioTimeMs)
        const double idlePercent = getPdhDoubleValue(counterSet.idleTimeCounter);
        // Handle all valid idle percentages, including 0.0 (100% busy)
        if (idlePercent >= 0.0)
        {
            // Idle time is a percentage; busy time = 100 - idle
            const double busyPercent = std::clamp(100.0 - idlePercent, 0.0, 100.0);
            // Convert to milliseconds (approximate based on sample interval)
            disk.ioTimeMs = static_cast<uint64_t>(busyPercent * 10.0); // Rough approximation
        }

        result.disks.push_back(disk);
    }

    spdlog::debug("WindowsDiskProbe: read {} disks", result.disks.size());
    return result;
}

DiskCapabilities WindowsDiskProbe::capabilities() const
{
    DiskCapabilities caps;
    caps.hasDiskStats = true;
    caps.hasReadWriteBytes = (m_Impl && m_Impl->query != nullptr && !m_Impl->diskCounters.empty());
    caps.hasIoTime = (m_Impl && m_Impl->query != nullptr && !m_Impl->diskCounters.empty());
    caps.hasDeviceInfo = true;
    caps.canFilterPhysical = true;
    return caps;
}

} // namespace Platform
