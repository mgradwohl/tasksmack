#include "WindowsDiskProbe.h"

#include "Platform/StorageTypes.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>

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

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{

// Pimpl struct containing Windows-specific types
struct WindowsDiskProbe::Impl
{
    struct DiskHandle
    {
        std::string instanceName;             // e.g. "0 C:" - kept for stable UI display
        HANDLE handle = INVALID_HANDLE_VALUE; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - Win32 handle type

        DiskHandle() = default;
        explicit DiskHandle(HANDLE h) : handle(h)
        {}

        DiskHandle(const DiskHandle&) = delete;
        DiskHandle& operator=(const DiskHandle&) = delete;

        DiskHandle(DiskHandle&& other) noexcept
            : instanceName(std::move(other.instanceName)), handle(std::exchange(other.handle, INVALID_HANDLE_VALUE))
        {}

        DiskHandle& operator=(DiskHandle&& other) noexcept
        {
            if (this != &other)
            {
                close();
                instanceName = std::move(other.instanceName);
                handle = std::exchange(other.handle, INVALID_HANDLE_VALUE);
            }
            return *this;
        }

        // RAII ownership: closes the handle on stack unwind (e.g. if name conversion or
        // vector growth throws mid-construction) as well as on normal teardown, so a
        // failure partway through enumerating disks can't leak this handle or any earlier
        // ones already stored in Impl::disks.
        ~DiskHandle()
        {
            close();
        }

      private:
        void close()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
            }
        }
    };

    std::vector<DiskHandle> disks;
};

namespace
{

/// PDH's PhysicalDisk instance names are formatted "<index> <driveletter(s)>", e.g.
/// "0 C:" or "1 D: E:" for a disk backing multiple volumes. Extract the leading index
/// so we can open the matching \\.\PhysicalDriveN device directly.
[[nodiscard]] std::optional<int> parsePhysicalDriveIndex(std::wstring_view instanceName)
{
    const auto spacePos = instanceName.find(L' ');
    const std::wstring_view indexPart = (spacePos == std::wstring_view::npos) ? instanceName : instanceName.substr(0, spacePos);
    if (indexPart.empty())
    {
        return std::nullopt;
    }

    int index = 0;
    for (const wchar_t ch : indexPart)
    {
        if (ch < L'0' || ch > L'9')
        {
            return std::nullopt;
        }
        index = (index * 10) + (ch - L'0');
    }
    return index;
}

/// Opens \\.\PhysicalDriveN with query-only access (no admin rights required) and
/// verifies IOCTL_DISK_PERFORMANCE is usable on it. Returns INVALID_HANDLE_VALUE on
/// any failure, closing the handle first if it was opened but the probe query failed.
[[nodiscard]] HANDLE openPhysicalDriveForPerfQuery(int driveIndex)
{
    const std::wstring devicePath = L"\\\\.\\PhysicalDrive" + std::to_wstring(driveIndex);
    HANDLE handle = CreateFileW(devicePath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        // Capture GetLastError() before any other call (including wideToUtf8's internal
        // WideCharToMultiByte) can overwrite it - argument evaluation order is unspecified,
        // so inlining GetLastError() as a call argument risks logging the wrong error.
        const DWORD lastError = GetLastError();
        spdlog::warn("WindowsDiskProbe: CreateFileW failed for {}, GetLastError={}", WinString::wideToUtf8(devicePath), lastError);
        return INVALID_HANDLE_VALUE;
    }

    DISK_PERFORMANCE perf{};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(handle, IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof(perf), &bytesReturned, nullptr) == 0)
    {
        const DWORD lastError = GetLastError();
        spdlog::warn(
            "WindowsDiskProbe: IOCTL_DISK_PERFORMANCE probe failed for {}, GetLastError={}", WinString::wideToUtf8(devicePath), lastError);
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }

    return handle;
}

} // namespace

WindowsDiskProbe::WindowsDiskProbe() : m_Impl(std::make_unique<Impl>())
{
    spdlog::debug("WindowsDiskProbe: initialized");

    // PDH is used only to enumerate PhysicalDisk instance names (which encode the
    // drive-letter-to-index mapping, e.g. "0 C:") - not to read counter values. PDH's
    // PhysicalDisk counters (Disk Read Bytes/sec, etc.) are pre-computed rates, not the
    // cumulative counts DiskCounters documents and StorageModel's delta-then-rate math
    // requires; IOCTL_DISK_PERFORMANCE below provides the real cumulative source instead.
    DWORD counterBufferSize = 0;
    DWORD instanceBufferSize = 0;
    PDH_STATUS status = PdhEnumObjectItemsW(
        nullptr, nullptr, L"PhysicalDisk", nullptr, &counterBufferSize, nullptr, &instanceBufferSize, PERF_DETAIL_WIZARD, 0);

    if (static_cast<DWORD>(status) == PDH_MORE_DATA && instanceBufferSize != 0U)
    {
        std::vector<wchar_t> counterBuffer(counterBufferSize);
        std::vector<wchar_t> instanceBuffer(instanceBufferSize);
        DWORD counterSize = counterBufferSize;
        DWORD instanceSize = instanceBufferSize;

        status = PdhEnumObjectItemsW(nullptr,
                                     nullptr,
                                     L"PhysicalDisk",
                                     counterBuffer.data(),
                                     &counterSize,
                                     instanceBuffer.data(),
                                     &instanceSize,
                                     PERF_DETAIL_WIZARD,
                                     0);

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

                if (const auto driveIndex = parsePhysicalDriveIndex(instanceName))
                {
                    HANDLE handle = openPhysicalDriveForPerfQuery(*driveIndex);
                    if (handle != INVALID_HANDLE_VALUE)
                    {
                        // DiskHandle takes RAII ownership of the handle immediately, before
                        // the name conversion or push_back below run, so either one throwing
                        // closes the handle automatically on unwind instead of leaking it.
                        Impl::DiskHandle diskHandle(handle);
                        diskHandle.instanceName = WinString::wideToUtf8(instanceName);
                        m_Impl->disks.push_back(std::move(diskHandle));
                    }
                }
                else
                {
                    spdlog::warn("WindowsDiskProbe: could not parse a drive index from PDH instance name '{}'",
                                 WinString::wideToUtf8(instanceName));
                }

                instance += instanceName.length() + 1;
            }
        }
    }

    spdlog::debug("WindowsDiskProbe: initialized with {} disks", m_Impl->disks.size());
}

WindowsDiskProbe::~WindowsDiskProbe()
{
    if (m_Impl)
    {
        for (const auto& disk : m_Impl->disks)
        {
            if (disk.handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(disk.handle);
            }
        }
    }
}

SystemDiskCounters WindowsDiskProbe::read()
{
    SystemDiskCounters result;

    if (!m_Impl || m_Impl->disks.empty())
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

    for (const auto& diskHandle : m_Impl->disks)
    {
        DISK_PERFORMANCE perf{};
        DWORD bytesReturned = 0;
        if (DeviceIoControl(diskHandle.handle, IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof(perf), &bytesReturned, nullptr) == 0)
        {
            // Skip this disk for this cycle rather than pushing fabricated zero counters,
            // which would otherwise look like a real (and wildly out-of-range) delta on
            // the next sample.
            spdlog::warn(
                "WindowsDiskProbe: IOCTL_DISK_PERFORMANCE failed for {}, GetLastError={}", diskHandle.instanceName, GetLastError());
            continue;
        }

        DiskCounters disk;
        disk.deviceName = diskHandle.instanceName;
        disk.sectorSize = 512;
        disk.isPhysicalDevice = true;

        // DISK_PERFORMANCE reports genuinely cumulative counters (since the disk's
        // performance counters started being tracked), matching the cumulative-counter
        // contract DiskCounters documents and that StorageModel::computeDiskSnapshot
        // relies on for its own delta-then-rate computation - unlike PDH's PhysicalDisk
        // object, which only exposes pre-computed rates.
        disk.readSectors = static_cast<uint64_t>(perf.BytesRead.QuadPart) / disk.sectorSize;
        disk.writeSectors = static_cast<uint64_t>(perf.BytesWritten.QuadPart) / disk.sectorSize;
        disk.readsCompleted = perf.ReadCount;
        disk.writesCompleted = perf.WriteCount;

        // ReadTime/WriteTime are cumulative, in 100-nanosecond units; convert to milliseconds.
        disk.readTimeMs = static_cast<uint64_t>(perf.ReadTime.QuadPart) / 10000ULL;
        disk.writeTimeMs = static_cast<uint64_t>(perf.WriteTime.QuadPart) / 10000ULL;

        // DISK_PERFORMANCE has no direct cumulative "device busy" counter, so approximate
        // it as the sum of cumulative read+write service time. Under concurrent I/O (queue
        // depth > 1) this can overestimate wall-clock busy time, but
        // StorageModel::computeDiskSnapshot already clamps the resulting utilization to
        // [0, 100], so it saturates rather than misreporting.
        disk.ioTimeMs = disk.readTimeMs + disk.writeTimeMs;

        result.disks.push_back(disk);
    }

    spdlog::debug("WindowsDiskProbe: read {} disks", result.disks.size());
    return result;
}

DiskCapabilities WindowsDiskProbe::capabilities() const
{
    DiskCapabilities caps;
    caps.hasDiskStats = true;
    caps.hasReadWriteBytes = (m_Impl && !m_Impl->disks.empty());
    caps.hasIoTime = (m_Impl && !m_Impl->disks.empty());
    caps.hasDeviceInfo = true;
    caps.canFilterPhysical = true;
    return caps;
}

} // namespace Platform
