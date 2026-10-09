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

/// One battery's static facts. A field the battery did not report is 0 or empty.
struct BatteryInfo
{
    bool found = false;            ///< A battery answered IOCTL_BATTERY_QUERY_INFORMATION
    bool relativeCapacity = false; ///< BATTERY_CAPACITY_RELATIVE: capacities are not mWh, so not reported
    double designWh = 0.0;
    double fullChargeWh = 0.0;
    int healthPercent = -1;
    std::uint64_t cycleCount = 0; ///< 0: not reported (the driver's "no cycle counter")
    std::string technology;       ///< Chemistry as text ("Li-ion")
    std::string manufacturer;
    std::string model;
};

/// Reads the first battery that answers. The serial number is an identifier and is not read.
[[nodiscard]] BatteryInfo readBatteryInfo(const BatteryDeviceFunctions& api);

} // namespace Platform::Windows

#endif // _WIN32
