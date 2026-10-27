#include "WindowsDiskProbe.h"

#include "Platform/StorageTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>
// clang-format on

#pragma comment(lib, "pdh.lib")

#include "PerfProviderApartment.h"
#include "WinString.h"
#include "WindowsDiskProbeMath.h"
#include "WindowsHandles.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
    explicit Impl(const WindowsDiskProbeFunctions& functions) : fns(functions)
    {}

    /// The disk and volume notification registrations (#3151), unregistered on destruction.
    ///
    /// CM_Unregister_Notification waits for any callback still running ("CM_Unregister_Notification waits
    /// for pending callbacks to finish", its documentation's remarks), and none starts after it returns,
    /// so once this is destroyed no callback can touch the flag it was given. Impl declares it after that
    /// flag, so it is destroyed first. Never destroyed from a callback, which is the deadlock those
    /// remarks warn of: the callback only sets the flag.
    class DeviceNotifications
    {
      public:
        DeviceNotifications() = default;
        ~DeviceNotifications()
        {
            reset();
        }
        DeviceNotifications(const DeviceNotifications&) = delete;
        DeviceNotifications& operator=(const DeviceNotifications&) = delete;
        DeviceNotifications(DeviceNotifications&&) = delete;
        DeviceNotifications& operator=(DeviceNotifications&&) = delete;

        /// Own @p handle, to be unregistered through @p unregister. Holds CAPACITY handles; the probe
        /// never adds more.
        void add(HCMNOTIFICATION handle, decltype(&::CM_Unregister_Notification) unregister) noexcept
        {
            m_Unregister = unregister;
            if (m_Count < m_Handles.size())
            {
                m_Handles[m_Count++] = handle;
            }
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return m_Count == 0;
        }

        /// Unregister every handle, newest first; each call returns once that registration's
        /// callbacks have finished. Its result goes unchecked: every handle here came from a
        /// registration that succeeded, and a destructor would have no recourse anyway.
        void reset() noexcept
        {
            while (m_Count > 0)
            {
                (void) m_Unregister(m_Handles[--m_Count]);
            }
        }

        /// The disk interface's registration and the volume interface's.
        static constexpr std::size_t CAPACITY = 2;

      private:
        std::array<HCMNOTIFICATION, CAPACITY> m_Handles{};
        std::size_t m_Count = 0;
        decltype(&::CM_Unregister_Notification) m_Unregister = nullptr;
    };

    WindowsDiskProbeFunctions fns; // Every OS call goes through these (#1719)

    /// One physical disk opened for IOCTL_DISK_PERFORMANCE. The UniqueHandle closes it on stack
    /// unwind (e.g. if name conversion or vector growth throws mid-enumeration) as well as on normal
    /// teardown, so a failure partway through enumerating disks can't leak this handle or any earlier
    /// ones already stored in Impl::disks.
    struct OpenDisk
    {
        std::string instanceName; // e.g. "0 C:" - kept for stable UI display
        Windows::UniqueHandle handle;
    };

    std::vector<OpenDisk> disks;

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

    // The PhysicalDisk object's localized name, once a lookup has found it (#2994); empty until then,
    // so a lookup that failed is tried again by the next rescan. Sampler thread only.
    std::wstring physicalDiskObject;

    // Set by the notification callback, on a CfgMgr32 thread-pool thread, when a disk or volume
    // arrives or leaves; read() takes it on the sampler thread (#3151).
    std::atomic<bool> deviceChanged{false};
    // When read() first took a device change since the last rescan; the rescan waits
    // DISK_DEVICE_CHANGE_SETTLE_DELAY from it. Sampler thread only.
    std::optional<std::chrono::steady_clock::time_point> deviceChangedAt;
    // Last, so it is destroyed first: unregistering waits for any callback still setting
    // deviceChanged, and none runs after it (see DeviceNotifications).
    DeviceNotifications notifications;

    void enumerate();
    /// The name to enumerate PhysicalDisk by: the localized one, or the English one if it can't be found.
    [[nodiscard]] std::wstring physicalDiskObjectName();
    /// Register for disk and volume interface arrivals and removals; on any failure, none (#3151).
    void registerDeviceNotifications();
};

namespace
{

/// PhysicalDisk's index in the performance-counter name table (Perflib\009's Counter value), the same
/// in every language. PdhEnumObjectItemsW takes the object's localized name, which this resolves (#2994).
constexpr DWORD PHYSICAL_DISK_OBJECT_INDEX = 234;
/// For a system whose localized name can't be looked up; an English system's name is this.
constexpr const wchar_t* PHYSICAL_DISK_OBJECT_ENGLISH = L"PhysicalDisk";

// GUID_DEVINTERFACE_DISK and GUID_DEVINTERFACE_VOLUME (winioctl.h), spelled out so no translation
// unit needs <initguid.h>. A disk's interface says it arrived or left; its volumes' say when it is
// partitioned and lettered, which PDH's instance name ("1 D: E:") carries (#3151).
constexpr GUID DISK_INTERFACE_GUID{
    .Data1 = 0x53f56307, .Data2 = 0xb6bf, .Data3 = 0x11d0, .Data4 = {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
constexpr GUID VOLUME_INTERFACE_GUID{
    .Data1 = 0x53f5630d, .Data2 = 0xb6bf, .Data3 = 0x11d0, .Data4 = {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

/// The CM_Register_Notification callback, on a CfgMgr32 thread-pool thread: it only sets the
/// probe's flag, never logging or rescanning there, and read() takes the flag (#3151). @p context
/// is that std::atomic<bool>, which outlives every callback (Impl::DeviceNotifications).
DWORD CALLBACK onDeviceInterfaceChange(HCMNOTIFICATION /*notification*/,
                                       PVOID context,
                                       CM_NOTIFY_ACTION action,
                                       PCM_NOTIFY_EVENT_DATA /*eventData*/,
                                       DWORD /*eventDataSize*/) noexcept
{
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
    {
        static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_release);
    }
    return ERROR_SUCCESS;
}

/// The name PDH has, in the system's language, for the counter object at @p index; nothing if the
/// lookup fails. Every PDH name fits PDH_MAX_COUNTER_NAME, so the first call normally fills the buffer.
/// If it reports PDH_MORE_DATA all the same, the size it returned for a too-small buffer isn't reliable
/// (per PdhLookupPerfNameByIndexW's documentation), so a size-only call asks for the right one.
[[nodiscard]] std::optional<std::wstring> lookupLocalizedPerfName(const WindowsDiskProbeFunctions::Pdh& pdh, DWORD index)
{
    std::vector<wchar_t> buffer(PDH_MAX_COUNTER_NAME + 1);
    auto size = static_cast<DWORD>(buffer.size());
    PDH_STATUS status = pdh.lookupPerfNameByIndexW(nullptr, index, buffer.data(), &size);
    if (static_cast<DWORD>(status) == PDH_MORE_DATA)
    {
        size = 0;
        status = pdh.lookupPerfNameByIndexW(nullptr, index, nullptr, &size);
        if (static_cast<DWORD>(status) == PDH_MORE_DATA && size > buffer.size())
        {
            buffer.resize(size);
            status = pdh.lookupPerfNameByIndexW(nullptr, index, buffer.data(), &size);
        }
        else if (status == ERROR_SUCCESS)
        {
            status = static_cast<PDH_STATUS>(PDH_MORE_DATA); // A size-only call filled nothing
        }
    }
    if (status != ERROR_SUCCESS)
    {
        spdlog::debug("WindowsDiskProbe: looking up perf object {}'s name failed (0x{:08X})", index, static_cast<DWORD>(status));
        return std::nullopt;
    }
    // Up to the terminator, and never past the buffer if a lookup left it unterminated.
    const auto end = std::ranges::find(buffer, L'\0');
    if (end == buffer.begin() || end == buffer.end())
    {
        spdlog::debug("WindowsDiskProbe: perf object {}'s name lookup returned no name", index);
        return std::nullopt;
    }
    return std::wstring(buffer.begin(), end);
}

/// Opens \\.\PhysicalDriveN with query-only access (no admin rights required) and
/// verifies IOCTL_DISK_PERFORMANCE is usable on it. Returns an empty handle on any
/// failure, closing the handle first if it was opened but the probe query failed.
/// Disks are re-enumerated periodically, so a drive that cannot be opened warns on its
/// first failure and logs at debug level until it next succeeds (#1159).
[[nodiscard]] Windows::UniqueHandle
openPhysicalDriveForPerfQuery(const WindowsDiskProbeFunctions::Drive& fns, int driveIndex, FailureLogLimiter& failures)
{
    // The UTF-8 path, for the failure logs below.
    const std::string path = R"(\\.\PhysicalDrive)" + std::to_string(driveIndex);
    // CreateFileW fails with INVALID_HANDLE_VALUE, which UniqueHandle treats as empty.
    Windows::UniqueHandle handle = fns.openPhysicalDriveQueryOnly(driveIndex);
    if (!handle)
    {
        // Capture GetLastError() before any other call can overwrite it - argument evaluation
        // order is unspecified, so inlining GetLastError() as a call argument risks logging the
        // wrong error.
        const DWORD lastError = GetLastError();
        const auto level = failures.recordFailure(path) ? spdlog::level::warn : spdlog::level::debug;
        spdlog::log(level, "WindowsDiskProbe: CreateFileW failed for {}, GetLastError={}", path, lastError);
        return {};
    }

    DISK_PERFORMANCE perf{};
    DWORD bytesReturned = 0;
    if (fns.deviceIoControl(handle.get(), IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof(perf), &bytesReturned, nullptr) == 0)
    {
        // Capture the error, then close the handle before logging: the drive isn't usable, so
        // there is no reason to keep it open while the log call runs.
        const DWORD lastError = GetLastError();
        handle.reset();
        const auto level = failures.recordFailure(path) ? spdlog::level::warn : spdlog::level::debug;
        spdlog::log(level, "WindowsDiskProbe: IOCTL_DISK_PERFORMANCE probe failed for {}, GetLastError={}", path, lastError);
        return {};
    }

    failures.recordSuccess(path); // Erasing from the set does not allocate, so cannot throw
    return handle;
}

} // namespace

std::wstring WindowsDiskProbe::Impl::physicalDiskObjectName()
{
    if (physicalDiskObject.empty())
    {
        if (auto localized = lookupLocalizedPerfName(fns.pdh, PHYSICAL_DISK_OBJECT_INDEX))
        {
            physicalDiskObject = std::move(*localized);
            spdlog::debug("WindowsDiskProbe: the PhysicalDisk object is '{}'", WinString::wideToUtf8(physicalDiskObject));
        }
        else
        {
            return PHYSICAL_DISK_OBJECT_ENGLISH; // Not kept: the next rescan looks it up again
        }
    }
    return physicalDiskObject;
}

void WindowsDiskProbe::Impl::registerDeviceNotifications()
{
    for (const GUID& interfaceClass : {DISK_INTERFACE_GUID, VOLUME_INTERFACE_GUID})
    {
        CM_NOTIFY_FILTER filter{};
        filter.cbSize = sizeof(filter);
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - FilterType selects DeviceInterface
        filter.u.DeviceInterface.ClassGuid = interfaceClass;
        HCMNOTIFICATION handle = nullptr;
        const CONFIGRET result = fns.devices.registerNotification(&filter, &deviceChanged, &onDeviceInterfaceChange, &handle);
        if (result != CR_SUCCESS)
        {
            // Both or neither: without the disks' own, a new disk would wait out the safety interval.
            notifications.reset();
            spdlog::info("WindowsDiskProbe: CM_Register_Notification failed (CONFIGRET {}); rescanning disks every {} s",
                         result,
                         DISK_REENUMERATE_INTERVAL.count());
            return;
        }
        notifications.add(handle, fns.devices.unregisterNotification);
    }
    spdlog::debug("WindowsDiskProbe: rescanning disks when a disk or volume arrives or leaves");
}

/// (Re)build the disk list (#1159). It used to be built once, at construction, so a removed
/// disk failed every refresh forever and a disk attached later never appeared. The new list is
/// built aside and swapped in; if PDH itself fails, the current list is kept.
void WindowsDiskProbe::Impl::enumerate()
{
    lastEnumeration = fns.now();
    // This rescan answers every device change read() has taken; one arriving from now on sets the
    // flag again, and the next read() takes it (#3151).
    deviceChangedAt.reset();
    std::vector<OpenDisk> found;
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
    const PDH_STATUS refresh = fns.pdh.enumObjectsW(nullptr, nullptr, nullptr, &objectListSize, PERF_DETAIL_WIZARD, TRUE);
    if (refresh != ERROR_SUCCESS && static_cast<DWORD>(refresh) != PDH_MORE_DATA)
    {
        spdlog::debug("WindowsDiskProbe: refreshing the PDH object list failed (0x{:08X}), keeping {} disks",
                      static_cast<DWORD>(refresh),
                      disks.size());
        return;
    }

    // By its localized name: an English-only "PhysicalDisk" is no object on a localized Windows, and
    // every enumeration failed there (#2994).
    const std::wstring objectName = physicalDiskObjectName();
    DWORD counterBufferSize = 0;
    DWORD instanceBufferSize = 0;
    PDH_STATUS status = fns.pdh.enumObjectItemsW(
        nullptr, nullptr, objectName.c_str(), nullptr, &counterBufferSize, nullptr, &instanceBufferSize, PERF_DETAIL_WIZARD, 0);

    if (static_cast<DWORD>(status) == PDH_MORE_DATA && instanceBufferSize != 0U)
    {
        std::vector<wchar_t> counterBuffer(counterBufferSize);
        std::vector<wchar_t> instanceBuffer(instanceBufferSize);
        DWORD counterSize = counterBufferSize;
        DWORD instanceSize = instanceBufferSize;

        status = fns.pdh.enumObjectItemsW(nullptr,
                                          nullptr,
                                          objectName.c_str(),
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
                    // The handle is owned from the moment it is opened, so the name conversion or
                    // push_back below throwing closes it on unwind instead of leaking it.
                    Windows::UniqueHandle handle = openPhysicalDriveForPerfQuery(fns.drive, *driveIndex, openFailures);
                    if (handle)
                    {
                        found.push_back(OpenDisk{.instanceName = WinString::wideToUtf8(instanceName), .handle = std::move(handle)});
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
        spdlog::debug("WindowsDiskProbe: enumerating PhysicalDisk ('{}') failed (0x{:08X}), keeping {} disks",
                      WinString::wideToUtf8(objectName),
                      static_cast<DWORD>(status),
                      disks.size());
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
                  { return std::ranges::none_of(disks, [&](const OpenDisk& disk) { return disk.instanceName == entry.first; }); });
}

WindowsDiskProbe::WindowsDiskProbe() : WindowsDiskProbe(systemFunctions())
{
    // The enumeration connected PDH, which may have loaded perf providers holding COM objects (#1898).
    Windows::keepComForPerfProviders();
}

WindowsDiskProbeFunctions WindowsDiskProbe::systemFunctions()
{
    WindowsDiskProbeFunctions fns;
    fns.pdh.lookupPerfNameByIndexW = &PdhLookupPerfNameByIndexW;
    fns.pdh.enumObjectsW = &PdhEnumObjectsW;
    fns.pdh.enumObjectItemsW = &PdhEnumObjectItemsW;
    fns.drive.openPhysicalDriveQueryOnly = &Windows::openPhysicalDriveQueryOnly;
    fns.drive.deviceIoControl = &DeviceIoControl;
    fns.drive.getLogicalDrives = &GetLogicalDrives;
    fns.drive.getDriveTypeW = &GetDriveTypeW;
    fns.devices.registerNotification = &CM_Register_Notification;
    fns.devices.unregisterNotification = &CM_Unregister_Notification;
    fns.now = &std::chrono::steady_clock::now;
    return fns;
}

WindowsDiskProbe::WindowsDiskProbe(const WindowsDiskProbeFunctions& functions) : m_Impl(std::make_unique<Impl>(functions))
{
    // Before the first enumeration, so a disk that arrives during it still asks for a rescan. On the
    // thread that builds the probe, the disk sampler's (#2731); the callbacks run on CfgMgr32's.
    m_Impl->registerDeviceNotifications();
    m_Impl->enumerate();
    spdlog::debug("WindowsDiskProbe: initialized with {} disks", m_Impl->disks.size());
}

// Impl::OpenDisk owns its HANDLE via RAII (closes on destruction), so destroying
// m_Impl -- and with it the vector<OpenDisk> -- is the single teardown point; no
// manual CloseHandle loop here, which would otherwise double-close each handle. Impl's
// DeviceNotifications, its last member, unregisters first, waiting for any callback in flight.
WindowsDiskProbe::~WindowsDiskProbe() = default;

SystemDiskCounters WindowsDiskProbe::read()
{
    SystemDiskCounters result = readCounters();

    // Rescan after the disks are read, never before: StorageModel times each sample from before
    // read(), so a rescan's latency ahead of the queries (PDH enumeration and reopening every
    // drive) would land between that timestamp and the disks' busy-time readings and swing
    // utilisation up on that sample and down on the next (#1108, #1159). Done here, it delays only
    // the end of this read; a removed disk is also dropped before the next read rather than during it.
    if (!m_Impl)
    {
        return result;
    }
    Impl& impl = *m_Impl;
    const auto now = impl.fns.now();
    // A disk or volume arrived or left (#3151). The rescan waits from the first change it hasn't
    // answered, so the disk's volumes are lettered by then; later ones until it runs are the same rescan.
    if (impl.deviceChanged.exchange(false, std::memory_order_acq_rel) && !impl.deviceChangedAt.has_value())
    {
        impl.deviceChangedAt = now;
        spdlog::debug("WindowsDiskProbe: a disk or volume arrived or left; rescanning in {} s", DISK_DEVICE_CHANGE_SETTLE_DELAY.count());
    }
    if (shouldReenumerate(
            now, impl.lastEnumeration, impl.lastReadHadFailure, diskReenumerateInterval(!impl.notifications.empty()), impl.deviceChangedAt))
    {
        impl.enumerate();
    }
    return result;
}

SystemDiskCounters WindowsDiskProbe::readCounters()
{
    SystemDiskCounters result;

    if (!m_Impl || m_Impl->disks.empty())
    {
        // Fallback: enumerate logical drives
        const DWORD drives = m_Impl->fns.drive.getLogicalDrives();
        if (drives == 0)
        {
            spdlog::warn("WindowsDiskProbe: GetLogicalDrives failed");
            return result;
        }

        for (int i = 0; i < 26; ++i)
        {
            if ((drives & (1U << static_cast<unsigned>(i))) == 0U)
            {
                continue;
            }

            const auto driveLetter = static_cast<wchar_t>('A' + i);
            const std::wstring drivePath = std::wstring{driveLetter} + L":\\";

            const UINT driveType = m_Impl->fns.drive.getDriveTypeW(drivePath.c_str());

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
            // Nothing was read: the Domain records these as no reading, not as idle disks (#2994).
            disk.hasIoCounters = false;

            result.disks.push_back(disk);
        }

        return result;
    }

    bool anyFailure = false;
    for (const auto& diskHandle : m_Impl->disks)
    {
        DISK_PERFORMANCE perf{};
        DWORD bytesReturned = 0;
        const BOOL queried = m_Impl->fns.drive.deviceIoControl(
            diskHandle.handle.get(), IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof(perf), &bytesReturned, nullptr);
        const DWORD queryError = (queried == 0) ? GetLastError() : ERROR_SUCCESS; // Before any other call
        // This disk's busy-time clock (#1108), taken next to its own IdleTime sample: monotonic, in
        // DISK_PERFORMANCE's 100 ns units. One timestamp for the whole loop let an earlier disk's
        // slow query become a later, idle disk's "busy" time.
        const std::int64_t now100ns =
            std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(m_Impl->fns.now().time_since_epoch())
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
