#pragma once

// A battery's static facts (#1523): design and full-charge capacity, cycle count, chemistry,
// manufacturer and model, from the battery class driver's IOCTL_BATTERY_QUERY_INFORMATION; and its
// live status (#4030), IOCTL_BATTERY_QUERY_STATUS on a device a rescan opened and tagged. The
// device calls are a table of function pointers, kernel32's and SetupAPI's by default, so tests can
// feed fixed answers and failures (test_WindowsBatteryInfo.cpp). No admin rights are needed.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <batclass.h>
// clang-format on

#include "WindowsPowerProbeMath.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Platform::Windows
{

/// The device interface paths of the present batteries (GUID_DEVICE_BATTERY), through SetupAPI.
[[nodiscard]] std::vector<std::wstring> enumerateBatteryDevicePaths();

/// The calls readBatteryInfo() makes: the system's, or a test's fakes.
struct BatteryDeviceFunctions
{
    using EnumerateFn = std::vector<std::wstring> (*)();
    using CreateFileFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using DeviceIoControlFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);

    EnumerateFn enumerateBatteries = &enumerateBatteryDevicePaths;
    CreateFileFn createFile = &::CreateFileW;
    DeviceIoControlFn deviceIoControl = &::DeviceIoControl;
    CloseHandleFn closeHandle = &::CloseHandle;
};

/// The batteries' static facts. A field no battery reported is 0 or empty. The capacities, health and
/// cycle count cover every battery (combineBatteryCapacities(), #2151), as the charge percent does;
/// the chemistry and names are the first answering battery's.
struct BatteryInfo
{
    bool found = false;            ///< A battery answered IOCTL_BATTERY_QUERY_INFORMATION
    bool relativeCapacity = false; ///< BATTERY_CAPACITY_RELATIVE on every battery: no mWh, so not reported
    double designWh = 0.0;         ///< Summed over the batteries that report both capacities (#2409)
    double fullChargeWh = 0.0;     ///< Summed over the same batteries as designWh
    int healthPercent = -1;        ///< From the two sums
    std::uint64_t cycleCount = 0;  ///< The most-cycled battery's; 0: not reported (the driver's "no cycle counter")
    std::string technology;        ///< Chemistry as text ("Li-ion")
    std::string manufacturer;
    std::string model;
};

/// An open battery device and the tag it answered with, kept from one rescan to the next (#4030) so a
/// sample sends only IOCTL_BATTERY_QUERY_STATUS: no SetupAPI enumeration, open or tag query per sample.
/// Copies share the handle, which the injected CloseHandle closes when the last copy goes.
struct BatteryDevice
{
    std::shared_ptr<void> handle;
    ULONG tag = 0;
    ULONG capabilities = 0; ///< BATTERY_INFORMATION::Capabilities: BATTERY_CAPACITY_RELATIVE applies to the status too
};

/// One rescan: the batteries' static facts, and the devices that answered, still open.
struct BatteryScan
{
    BatteryInfo info;
    std::vector<BatteryDevice> devices;
};

/// Enumerates, opens and tags every battery, and reads its static facts. A battery that answers
/// IOCTL_BATTERY_QUERY_INFORMATION is kept open in devices; every other handle is closed. The serial
/// number is an identifier and is not read.
[[nodiscard]] BatteryScan scanBatteries(const BatteryDeviceFunctions& api);

/// scanBatteries()'s static facts, with every device closed.
[[nodiscard]] BatteryInfo readBatteryInfo(const BatteryDeviceFunctions& api);

/// What one cached device's IOCTL_BATTERY_QUERY_STATUS gave.
enum class BatteryStatusOutcome : std::uint8_t
{
    Read,   ///< The reading is filled in
    Failed, ///< No answer this time; the device is kept
    Stale,  ///< The tag no longer names a battery in this slot (ERROR_FILE_NOT_FOUND, ERROR_NO_SUCH_DEVICE):
            ///< the battery was taken out or swapped, so the device is dropped until a rescan opens it again
};

/// The cached device's live status: rate, remaining capacity and voltage. One IOCTL, no wait.
[[nodiscard]] BatteryStatusOutcome
queryBatteryStatus(const BatteryDeviceFunctions& api, const BatteryDevice& device, BatteryStatusReading& reading);

} // namespace Platform::Windows

#endif // _WIN32
