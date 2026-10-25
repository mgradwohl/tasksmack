#pragma once

// A battery's static facts (#1523): design and full-charge capacity, cycle count, chemistry,
// manufacturer and model, from the battery class driver's IOCTL_BATTERY_QUERY_INFORMATION. The
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

#include <cstdint>
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

/// Reads every battery that answers. The serial number is an identifier and is not read.
[[nodiscard]] BatteryInfo readBatteryInfo(const BatteryDeviceFunctions& api);

} // namespace Platform::Windows

#endif // _WIN32
