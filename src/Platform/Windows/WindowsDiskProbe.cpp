// Keep this translation unit parseable on non-Windows platforms
#if defined(_WIN32)

#include "WindowsDiskProbe.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

// clang-format off
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <pdh.h>
#include <pdhmsg.h>
// clang-format on

#pragma comment(lib, "pdh.lib")

namespace Platform
{

namespace
{

// Helper to convert wide string to narrow string
std::string wideToNarrow(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.length()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.length()), result.data(), size, nullptr, nullptr);
    return result;
}

} // namespace

WindowsDiskProbe::WindowsDiskProbe()
{
    spdlog::debug("WindowsDiskProbe: initialized");

    // Initialize PDH query
    PDH_STATUS status = PdhOpenQuery(nullptr, 0, &m_Query);
    if (status != ERROR_SUCCESS)
    {
        spdlog::error("WindowsDiskProbe: PdhOpenQuery failed with status {}", status);
        m_Query = nullptr;
        return;
    }

    // Enumerate physical disks
    DWORD bufferSize = 0;
    status = PdhEnumObjectItems(nullptr, nullptr, L"PhysicalDisk", nullptr, &bufferSize, nullptr, nullptr, PERF_DETAIL_WIZARD, 0);

    if (status == PDH_MORE_DATA && bufferSize > 0)
    {
        std::vector<wchar_t> instanceBuffer(bufferSize);
        DWORD instanceSize = bufferSize;
        DWORD counterSize = 0;

        status = PdhEnumObjectItems(
            nullptr, nullptr, L"PhysicalDisk", nullptr, &counterSize, instanceBuffer.data(), &instanceSize, PERF_DETAIL_WIZARD, 0);

        if (status == ERROR_SUCCESS)
        {
            // Parse instance names (null-separated list)
            const wchar_t* instance = instanceBuffer.data();
            while (*instance != L'\0')
            {
                std::wstring instanceName(instance);

                // Skip "_Total" instance
                if (instanceName == L"_Total")
                {
                    instance += instanceName.length() + 1;
                    continue;
                }

                DiskCounterSet counterSet;
                counterSet.instanceName = wideToNarrow(instanceName);

                // Add counters for this disk
                std::wstring readBytesPath = L"\\PhysicalDisk(" + instanceName + L")\\Disk Read Bytes/sec";
                std::wstring writeBytesPath = L"\\PhysicalDisk(" + instanceName + L")\\Disk Write Bytes/sec";
                std::wstring readsPath = L"\\PhysicalDisk(" + instanceName + L")\\Disk Reads/sec";
                std::wstring writesPath = L"\\PhysicalDisk(" + instanceName + L")\\Disk Writes/sec";
                std::wstring idleTimePath = L"\\PhysicalDisk(" + instanceName + L")\\% Idle Time";

                PdhAddCounter(m_Query, readBytesPath.c_str(), 0, &counterSet.readBytesCounter);
                PdhAddCounter(m_Query, writeBytesPath.c_str(), 0, &counterSet.writeBytesCounter);
                PdhAddCounter(m_Query, readsPath.c_str(), 0, &counterSet.readsCounter);
                PdhAddCounter(m_Query, writesPath.c_str(), 0, &counterSet.writesCounter);
                PdhAddCounter(m_Query, idleTimePath.c_str(), 0, &counterSet.idleTimeCounter);

                m_DiskCounters.push_back(counterSet);

                instance += instanceName.length() + 1;
            }
        }
    }

    // Collect initial sample
    if (m_Query != nullptr)
    {
        PdhCollectQueryData(m_Query);
    }

    spdlog::debug("WindowsDiskProbe: initialized with {} disks", m_DiskCounters.size());
}

WindowsDiskProbe::~WindowsDiskProbe()
{
    if (m_Query != nullptr)
    {
        PdhCloseQuery(m_Query);
    }
}

SystemDiskCounters WindowsDiskProbe::read()
{
    SystemDiskCounters result;

    if (m_Query == nullptr || m_DiskCounters.empty())
    {
        // Fallback: enumerate logical drives
        DWORD drives = GetLogicalDrives();
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

            char driveLetter = static_cast<char>('A' + i);
            std::string drivePath = std::string{driveLetter} + ":\\";

            UINT driveType = GetDriveTypeA(drivePath.c_str());

            // Only include fixed drives
            if (driveType != DRIVE_FIXED)
            {
                continue;
            }

            DiskCounters disk;
            disk.deviceName = std::string{driveLetter} + ":";
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
    PDH_STATUS status = PdhCollectQueryData(m_Query);
    if (status != ERROR_SUCCESS)
    {
        spdlog::warn("WindowsDiskProbe: PdhCollectQueryData failed with status {}", status);
        return result;
    }

    // Read counter values
    for (const auto& counterSet : m_DiskCounters)
    {
        DiskCounters disk;
        disk.deviceName = counterSet.instanceName;
        disk.sectorSize = 512;
        disk.isPhysicalDevice = true;

        PDH_FMT_COUNTERVALUE value;

        // Read bytes/sec
        if (PdhGetFormattedCounterValue(counterSet.readBytesCounter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS)
        {
            // Convert bytes/sec to cumulative sectors (approximate)
            disk.readSectors = static_cast<uint64_t>(value.doubleValue / disk.sectorSize);
        }

        if (PdhGetFormattedCounterValue(counterSet.writeBytesCounter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS)
        {
            disk.writeSectors = static_cast<uint64_t>(value.doubleValue / disk.sectorSize);
        }

        // Read operations/sec
        if (PdhGetFormattedCounterValue(counterSet.readsCounter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS)
        {
            disk.readsCompleted = static_cast<uint64_t>(value.doubleValue);
        }

        if (PdhGetFormattedCounterValue(counterSet.writesCounter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS)
        {
            disk.writesCompleted = static_cast<uint64_t>(value.doubleValue);
        }

        // Idle time (convert to busy time for ioTimeMs)
        if (PdhGetFormattedCounterValue(counterSet.idleTimeCounter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS)
        {
            // Idle time is a percentage; busy time = 100 - idle
            double busyPercent = std::clamp(100.0 - value.doubleValue, 0.0, 100.0);
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
    caps.hasReadWriteBytes = (m_Query != nullptr && !m_DiskCounters.empty());
    caps.hasIoTime = (m_Query != nullptr && !m_DiskCounters.empty());
    caps.hasDeviceInfo = true;
    caps.canFilterPhysical = true;
    return caps;
}

} // namespace Platform

#endif // _WIN32
