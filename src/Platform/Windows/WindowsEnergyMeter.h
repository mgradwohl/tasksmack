#pragma once

// The CPU package energy counters of the Energy Meter Interface (EMI) devices (#2587): the Windows
// twin of Linux's powercap RAPL zones, for the per-process Power column, Process Details' power
// figure and the System tab's Power chart. Each EMI device (GUID_DEVICE_ENERGY_METER, emi.h) names
// its channels in its metadata; the Intel driver's "RAPL_Package<N>_PKG" channels are the packages'
// (WindowsEnergyMeterMath.h's isPackageChannelName()), and IOCTL_EMI_GET_MEASUREMENT answers every
// channel's cumulative energy in picowatt-hours. This reads those raw counters only, converted to
// microjoules: ProcessModel's Attributor shares each interval's energy out by CPU time, as on Linux.
//
// The devices are enumerated (SetupAPI, which a slow ACPI call can stall) and opened once, by
// EnergyMeter::open() on WindowsProcessProbe's one-shot opener thread, never the UI thread; the
// handles are kept and polled each sample. The device calls are a table of function pointers, the
// system's by default, so tests feed fake metadata and measurements (test_WindowsEnergyMeter.cpp).

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include "WindowsHandles.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Platform::Windows
{

/// The device interface paths of the present energy meters (GUID_DEVICE_ENERGY_METER), through SetupAPI.
[[nodiscard]] std::vector<std::wstring> enumerateEnergyMeterDevicePaths();

/// The calls EnergyMeter makes: the system's, or a test's fakes. A handle createFile returns is closed
/// through closeHandle, so a fake handle never reaches the real CloseHandle.
struct EnergyMeterFunctions
{
    using EnumerateFn = std::vector<std::wstring> (*)();
    using CreateFileFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using DeviceIoControlFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);

    EnumerateFn enumerateMeters = &enumerateEnergyMeterDevicePaths;
    CreateFileFn createFile = &::CreateFileW;
    DeviceIoControlFn deviceIoControl = &::DeviceIoControl;
    CloseHandleFn closeHandle = &::CloseHandle;
};

/// One CPU package's channel: which opened device it is on, and its index in that device's measurement array.
struct PackageEnergyChannel
{
    std::size_t device = 0;
    std::size_t channel = 0;
    std::string name; ///< As the metadata names it ("RAPL_Package0_PKG")
};

/// The package channels of every EMI device that has one, opened once and read each sample.
/// Not thread-safe: WindowsProcessProbe reads it under its own lock, on the process sampler thread.
class EnergyMeter
{
  public:
    /// No device: hasPackageChannels() is false and nothing is read.
    EnergyMeter() = default;

    /// Enumerates the energy meters, opens each, reads its interface version and metadata, keeps the
    /// devices with a package channel open and closes the rest. Logs what it found once. Blocking (a
    /// SetupAPI enumeration and a few IOCTLs): call it off the UI thread.
    [[nodiscard]] static EnergyMeter open(const EnergyMeterFunctions& api);

    /// Closes the devices still open.
    ~EnergyMeter() = default;

    EnergyMeter(const EnergyMeter&) = delete;
    EnergyMeter& operator=(const EnergyMeter&) = delete;
    EnergyMeter(EnergyMeter&&) noexcept = default;
    EnergyMeter& operator=(EnergyMeter&&) noexcept = default;

    /// Whether any device has a package channel: the per-process power capability.
    [[nodiscard]] bool hasPackageChannels() const noexcept
    {
        return !m_Packages.empty();
    }

    /// The package channels, by name ("RAPL_Package0_PKG" first), so index 0 is the first package and
    /// the rest are PackageEnergyReading::otherPackages (#2444).
    [[nodiscard]] std::span<const PackageEnergyChannel> packageChannels() const noexcept
    {
        return m_Packages;
    }

    /// Reads every device's measurement and writes each package channel's cumulative energy, in
    /// microjoules, to @p energyUj in packageChannels() order. False, with @p energyUj cleared, if any
    /// device's read failed (a meter that went away): the sample is then unread as a whole, rather than
    /// one package's energy shared out over every package's CPU time. Never logs: the caller does, once.
    [[nodiscard]] bool readPackageEnergyUj(std::vector<std::uint64_t>& energyUj);

    /// GetLastError() of the read that last made readPackageEnergyUj() return false, for that one log line.
    [[nodiscard]] DWORD lastReadError() const noexcept
    {
        return m_LastReadError;
    }

  private:
    struct Device
    {
        InjectableHandle handle;
        std::vector<std::byte> measurement; ///< Sized for every channel the metadata named; reused each read
    };

    EnergyMeterFunctions m_Api;
    std::vector<Device> m_Devices;
    std::vector<PackageEnergyChannel> m_Packages;
    DWORD m_LastReadError = 0;
};

} // namespace Platform::Windows

#endif // _WIN32
