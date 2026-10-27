#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/StorageTypes.h"

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
#include <cfgmgr32.h>
#include <pdh.h>
// clang-format on

#include "Platform/Windows/WindowsHandles.h"

namespace Platform
{

/// The calls WindowsDiskProbe makes (#1719): the system's by default (WindowsDiskProbe::systemFunctions()),
/// fakes in tests, so the logical-drive fallback, failed opens and IOCTL_DISK_PERFORMANCE reads,
/// rescans and the busy-time clock can be tested without a disk that fails. Every member must be set.
struct WindowsDiskProbeFunctions
{
    /// PhysicalDisk instance enumeration
    struct Pdh
    {
        /// Resolves the PhysicalDisk object's name in the system's language (#2994): PdhEnumObjectItemsW
        /// takes the localized name, so the English one fails on a localized Windows.
        PDH_STATUS(WINAPI* lookupPerfNameByIndexW)(LPCWSTR, DWORD, LPWSTR, LPDWORD) = nullptr;
        PDH_STATUS(WINAPI* enumObjectsW)(LPCWSTR, LPCWSTR, PZZWSTR, LPDWORD, DWORD, BOOL) = nullptr;
        PDH_STATUS(WINAPI* enumObjectItemsW)(LPCWSTR, LPCWSTR, LPCWSTR, PZZWSTR, LPDWORD, PZZWSTR, LPDWORD, DWORD, DWORD) = nullptr;
    } pdh;

    struct Drive
    {
        /// Opens \\.\PhysicalDriveN query-only (Windows::openPhysicalDriveQueryOnly); empty on failure.
        Windows::UniqueHandle (*openPhysicalDriveQueryOnly)(int index) = nullptr;
        /// Asked for IOCTL_DISK_PERFORMANCE only.
        BOOL(WINAPI* deviceIoControl)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED) = nullptr;
        DWORD(WINAPI* getLogicalDrives)() = nullptr;
        UINT(WINAPI* getDriveTypeW)(LPCWSTR) = nullptr;
    } drive;

    /// Disk and volume arrival and removal notifications, which say when to rescan (#3151).
    struct Devices
    {
        /// CM_Register_Notification. A fake keeps the callback and context to simulate a change.
        decltype(&::CM_Register_Notification) registerNotification = nullptr;
        /// CM_Unregister_Notification, which waits for any callback still running.
        decltype(&::CM_Unregister_Notification) unregisterNotification = nullptr;
    } devices;

    /// The monotonic clock for rescans and each disk's busy time.
    std::chrono::steady_clock::time_point (*now)() noexcept = nullptr;
};

/// Windows implementation of IDiskProbe. Uses IOCTL_DISK_PERFORMANCE for cumulative
/// disk I/O counters; Performance Data Helper (PDH) is used only to enumerate physical disk
/// instance names (e.g. "0 C:"), at construction and again from read() so removed and added disks
/// are picked up (#1159): after a disk fails, shortly after a disk or volume arrives or leaves
/// (CM_Register_Notification, registered by the constructor on the thread that builds the probe,
/// the disk sampler's), and otherwise every DISK_REENUMERATE_SAFETY_INTERVAL, or every
/// DISK_REENUMERATE_INTERVAL when the notifications couldn't be registered (#3151).
class WindowsDiskProbe : public IDiskProbe
{
  public:
    WindowsDiskProbe();
    /// Make every call through @p functions rather than the system's (tests, #1719).
    explicit WindowsDiskProbe(const WindowsDiskProbeFunctions& functions);
    ~WindowsDiskProbe() override;

    /// The real calls.
    [[nodiscard]] static WindowsDiskProbeFunctions systemFunctions();

    WindowsDiskProbe(const WindowsDiskProbe&) = delete;
    WindowsDiskProbe& operator=(const WindowsDiskProbe&) = delete;
    WindowsDiskProbe(WindowsDiskProbe&&) = delete;
    WindowsDiskProbe& operator=(WindowsDiskProbe&&) = delete;

    [[nodiscard]] SystemDiskCounters read() override;
    [[nodiscard]] DiskCapabilities capabilities() const override;

  private:
    /// The disks' counters, without rescanning; read() rescans afterwards when it is due (#1159).
    [[nodiscard]] SystemDiskCounters readCounters();

    // Opaque implementation, holding the disks and the functions above
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
