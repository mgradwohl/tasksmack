#include "WindowsDiskProbe.h"

#include "Platform/StorageTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "WindowsDiskProbeMath.h"

#include <cstdint>
#include <ratio>
#include <string>
#include <unordered_map>
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

    // read() runs on the sampler thread and swaps `disks` when it re-enumerates (#1159), while
    // capabilities() can be called from any thread, so it reads this flag rather than `disks`.
    std::atomic<bool> hasDisks{false};

    // Sampler-thread state for re-enumeration and rate-limited logging (#1159).
    std::chrono::steady_clock::time_point lastEnumeration;
    bool lastReadHadFailure = false;
    FailureLogLimiter openFailures;  // keyed by device path, e.g. "\\.\PhysicalDrive1"
    FailureLogLimiter readFailures;  // keyed by PDH instance name, e.g. "1 D:"
    FailureLogLimiter parseFailures; // keyed by PDH instance name

    // Busy-time baselines per disk, keyed by PDH instance name (#1108); sampler thread only.
    std::unordered_map<std::string, DiskBusyClock> busyClocks;

    void enumerate();
};

namespace
{

/// Opens \\.\PhysicalDriveN with query-only access (no admin rights required) and
/// verifies IOCTL_DISK_PERFORMANCE is usable on it. Returns INVALID_HANDLE_VALUE on
/// any failure, closing the handle first if it was opened but the probe query failed.
/// Disks are re-enumerated periodically, so a drive that cannot be opened warns on its
/// first failure and logs at debug level until it next succeeds (#1159).
[[nodiscard]] HANDLE openPhysicalDriveForPerfQuery(int driveIndex, FailureLogLimiter& failures)
{
    const std::wstring devicePath = L"\\\\.\\PhysicalDrive" + std::to_wstring(driveIndex);
    // Converted before the drive is opened: wideToUtf8 allocates and can throw, and nothing that
    // can throw may run while the raw handle below is not yet owned by a DiskHandle.
    const std::string path = WinString::wideToUtf8(devicePath);
    HANDLE handle = CreateFileW(devicePath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        // Capture GetLastError() before any other call can overwrite it - argument evaluation
        // order is unspecified, so inlining GetLastError() as a call argument risks logging the
        // wrong error.
        const DWORD lastError = GetLastError();
        const auto level = failures.recordFailure(path) ? spdlog::level::warn : spdlog::level::debug;
        spdlog::log(level, "WindowsDiskProbe: CreateFileW failed for {}, GetLastError={}", path, lastError);
        return INVALID_HANDLE_VALUE;
    }

    DISK_PERFORMANCE perf{};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(handle, IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof(perf), &bytesReturned, nullptr) == 0)
    {
        // Capture the error and close the handle before the heap-allocating log calls below, so
        // an allocation failure there can't skip CloseHandle and leak the handle.
        const DWORD lastError = GetLastError();
        CloseHandle(handle);
        const auto level = failures.recordFailure(path) ? spdlog::level::warn : spdlog::level::debug;
        spdlog::log(level, "WindowsDiskProbe: IOCTL_DISK_PERFORMANCE probe failed for {}, GetLastError={}", path, lastError);
        return INVALID_HANDLE_VALUE;
    }

    failures.recordSuccess(path); // Erasing from the set does not allocate, so cannot throw
    return handle;
}

} // namespace

/// (Re)build the disk list (#1159). It used to be built once, at construction, so a removed
/// disk failed every refresh forever and a disk attached later never appeared. The new list is
/// built aside and swapped in; if PDH itself fails, the current list is kept.
void WindowsDiskProbe::Impl::enumerate()
{
    lastEnumeration = std::chrono::steady_clock::now();
    std::vector<DiskHandle> found;
    bool enumerated = false;

    // PDH is used only to enumerate PhysicalDisk instance names (which encode the
    // drive-letter-to-index mapping, e.g. "0 C:") - not to read counter values. PDH's
    // PhysicalDisk counters (Disk Read Bytes/sec, etc.) are pre-computed rates, not the
    // cumulative counts DiskCounters documents and StorageModel's delta-then-rate math
    // requires; IOCTL_DISK_PERFORMANCE below provides the real cumulative source instead.
    // PdhEnumObjectItemsW answers from PDH's cached object list, so a disk attached after the
    // first enumeration never appeared. Refreshing the list (PdhEnumObjects with bRefresh = TRUE,
    // per PdhEnumObjectItems' remarks) first is what lets a rescan see it (#1159).
    DWORD objectListSize = 0;
    const PDH_STATUS refresh = PdhEnumObjectsW(nullptr, nullptr, nullptr, &objectListSize, PERF_DETAIL_WIZARD, TRUE);
    if (refresh != ERROR_SUCCESS && static_cast<DWORD>(refresh) != PDH_MORE_DATA)
    {
        spdlog::debug("WindowsDiskProbe: refreshing the PDH object list failed (0x{:08X}), keeping {} disks",
                      static_cast<DWORD>(refresh),
                      disks.size());
        return;
    }

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
            enumerated = true;
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
                    HANDLE handle = openPhysicalDriveForPerfQuery(*driveIndex, openFailures);
                    if (handle != INVALID_HANDLE_VALUE)
                    {
                        // DiskHandle takes RAII ownership of the handle immediately, before
                        // the name conversion or push_back below run, so either one throwing
                        // closes the handle automatically on unwind instead of leaking it.
                        DiskHandle diskHandle(handle);
                        diskHandle.instanceName = WinString::wideToUtf8(instanceName);
                        found.push_back(std::move(diskHandle));
                    }
                }
                else
                {
                    const std::string name = WinString::wideToUtf8(instanceName);
                    const auto level = parseFailures.recordFailure(name) ? spdlog::level::warn : spdlog::level::debug;
                    spdlog::log(level, "WindowsDiskProbe: could not parse a drive index from PDH instance name '{}'", name);
                }

                instance += instanceName.length() + 1;
            }
        }
    }

    if (!enumerated)
    {
        spdlog::debug("WindowsDiskProbe: PhysicalDisk enumeration failed, keeping {} disks", disks.size());
        return;
    }

    if (found.size() != disks.size())
    {
        spdlog::debug("WindowsDiskProbe: now tracking {} physical disks (was {})", found.size(), disks.size());
    }
    disks = std::move(found);
    hasDisks.store(!disks.empty(), std::memory_order_release);
    // The failures that prompted this rescan were in the old list; the next read of the new one
    // sets the flag again if any disk still fails. Left set, a rescan that removed the last failing
    // disk (falling back to logical drives, which never clears it) kept rescanning every 5 s (#1159).
    lastReadHadFailure = false;

    // Forget the busy-time baseline of every disk that is gone, so one re-attached under the same
    // instance name starts afresh instead of counting the time it was away as busy (#1108).
    std::erase_if(busyClocks,
                  [this](const auto& entry)
                  { return std::ranges::none_of(disks, [&](const DiskHandle& disk) { return disk.instanceName == entry.first; }); });
}

WindowsDiskProbe::WindowsDiskProbe() : m_Impl(std::make_unique<Impl>())
{
    m_Impl->enumerate();
    spdlog::debug("WindowsDiskProbe: initialized with {} disks", m_Impl->disks.size());
}

// Impl::DiskHandle owns its HANDLE via RAII (closes on destruction), so destroying
// m_Impl -- and with it the vector<DiskHandle> -- is the single teardown point; no
// manual CloseHandle loop here, which would otherwise double-close each handle.
WindowsDiskProbe::~WindowsDiskProbe() = default;

SystemDiskCounters WindowsDiskProbe::read()
{
    SystemDiskCounters result = readCounters();

    // Rescan after the disks are read, never before: StorageModel times each sample from before
    // read(), so a rescan's latency ahead of the queries (PDH enumeration and reopening every
    // drive) would land between that timestamp and the disks' busy-time readings and swing
    // utilisation up on that sample and down on the next (#1108, #1159). Done here, it delays only
    // the end of this read; a removed disk is also dropped before the next read rather than during it.
    if (m_Impl &&
        shouldReenumerate(std::chrono::steady_clock::now(), m_Impl->lastEnumeration, m_Impl->lastReadHadFailure, DISK_REENUMERATE_INTERVAL))
    {
        m_Impl->enumerate();
    }
    return result;
}

SystemDiskCounters WindowsDiskProbe::readCounters()
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

            const auto driveLetter = static_cast<wchar_t>('A' + i);
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

    bool anyFailure = false;
    for (const auto& diskHandle : m_Impl->disks)
    {
        DISK_PERFORMANCE perf{};
        DWORD bytesReturned = 0;
        const BOOL queried =
            DeviceIoControl(diskHandle.handle, IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof(perf), &bytesReturned, nullptr);
        const DWORD queryError = (queried == 0) ? GetLastError() : ERROR_SUCCESS; // Before any other call
        // This disk's busy-time clock (#1108), taken next to its own IdleTime sample: monotonic, in
        // DISK_PERFORMANCE's 100 ns units. One timestamp for the whole loop let an earlier disk's
        // slow query become a later, idle disk's "busy" time.
        const std::int64_t now100ns = std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count();
        if (queried == 0)
        {
            // Skip this disk for this cycle rather than pushing fabricated zero counters,
            // which would otherwise look like a real (and wildly out-of-range) delta on
            // the next sample. The next read() re-enumerates, dropping a removed disk, and
            // only the first failure in a row warns (#1159).
            const DWORD lastError = queryError;
            anyFailure = true;
            const auto level = m_Impl->readFailures.recordFailure(diskHandle.instanceName) ? spdlog::level::warn : spdlog::level::debug;
            spdlog::log(
                level, "WindowsDiskProbe: IOCTL_DISK_PERFORMANCE failed for {}, GetLastError={}", diskHandle.instanceName, lastError);
            continue;
        }
        m_Impl->readFailures.recordSuccess(diskHandle.instanceName);

        DiskCounters disk;
        disk.deviceName = diskHandle.instanceName;
        disk.sectorSize = 512;
        disk.isPhysicalDevice = true;

        // DISK_PERFORMANCE reports genuinely cumulative counters (since the disk's
        // performance counters started being tracked), matching the cumulative-counter
        // contract DiskCounters documents and that StorageModel::computeDiskSnapshot
        // relies on for its own delta-then-rate computation - unlike PDH's PhysicalDisk
        // object, which only exposes pre-computed rates.
        disk.readSectors = clampNonNegativeQuadPart(perf.BytesRead.QuadPart) / disk.sectorSize;
        disk.writeSectors = clampNonNegativeQuadPart(perf.BytesWritten.QuadPart) / disk.sectorSize;
        disk.readsCompleted = perf.ReadCount;
        disk.writesCompleted = perf.WriteCount;

        // ReadTime/WriteTime are cumulative, in 100-nanosecond units; convert to milliseconds.
        disk.readTimeMs = clampNonNegativeQuadPart(perf.ReadTime.QuadPart) / 10000ULL;
        disk.writeTimeMs = clampNonNegativeQuadPart(perf.WriteTime.QuadPart) / 10000ULL;

        // Busy time is the monotonic elapsed time less the growth in IdleTime: the time the disk had
        // I/O outstanding. ReadTime + WriteTime counts each queued request separately, so
        // overlapping I/O outran wall time and utilisation read 100 % (#1108); it remains only
        // the fallback for drivers that do not report IdleTime.
        if (const auto busy100ns = advanceDiskBusy(m_Impl->busyClocks[diskHandle.instanceName], now100ns, perf.IdleTime.QuadPart))
        {
            disk.ioTimeMs = *busy100ns / 10000ULL;
        }
        else
        {
            disk.ioTimeMs = disk.readTimeMs + disk.writeTimeMs;
        }

        result.disks.push_back(disk);
    }
    m_Impl->lastReadHadFailure = anyFailure;

    spdlog::debug("WindowsDiskProbe: read {} disks", result.disks.size());
    return result;
}

DiskCapabilities WindowsDiskProbe::capabilities() const
{
    DiskCapabilities caps;
    caps.hasDiskStats = true;
    const bool hasDisks = m_Impl && m_Impl->hasDisks.load(std::memory_order_acquire);
    caps.hasReadWriteBytes = hasDisks;
    caps.hasIoTime = hasDisks;
    caps.hasDeviceInfo = true;
    caps.canFilterPhysical = true;
    return caps;
}

} // namespace Platform
