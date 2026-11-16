#include "WindowsBatteryInfo.h"

#include "WinString.h"
#include "WindowsPowerProbeMath.h"

// clang-format off
#include <setupapi.h>
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

// GUID_DEVICE_BATTERY (poclass.h), spelled out so no translation unit needs <initguid.h>.
constexpr GUID BATTERY_INTERFACE_GUID{
    .Data1 = 0x72631e54,
    .Data2 = 0x78A4,
    .Data3 = 0x11d0,
    .Data4 = {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a},
};
// More batteries than any machine has; bounds the enumeration.
constexpr DWORD MAX_BATTERIES = 8;
// Characters for a name query; the driver's strings are short.
constexpr std::size_t NAME_CHARS = 128;

/// Closes a device handle through the injected CloseHandle.
struct DeviceCloser
{
    BatteryDeviceFunctions::CloseHandleFn close;
    void operator()(HANDLE handle) const noexcept
    {
        close(handle);
    }
};
using DeviceHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, DeviceCloser>;

/// A name query's string, or empty when the battery does not report it.
[[nodiscard]] std::string queryName(const BatteryDeviceFunctions& api, HANDLE device, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level)
{
    BATTERY_QUERY_INFORMATION query{};
    query.BatteryTag = tag;
    query.InformationLevel = level;
    std::array<wchar_t, NAME_CHARS> name{};
    DWORD bytes = 0;
    constexpr auto CAPACITY = static_cast<DWORD>((NAME_CHARS - 1) * sizeof(wchar_t)); // Keeps a terminating zero
    const BOOL ok =
        api.deviceIoControl(device, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), name.data(), CAPACITY, &bytes, nullptr);
    return ok != FALSE ? WinString::wideToUtf8(std::wstring_view(name.data())) : std::string();
}

} // namespace

std::vector<std::wstring> enumerateBatteryDevicePaths()
{
    std::vector<std::wstring> paths;
    HDEVINFO devices = SetupDiGetClassDevsW(&BATTERY_INTERFACE_GUID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE)
    {
        return paths;
    }
    for (DWORD index = 0; index < MAX_BATTERIES; ++index)
    {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (SetupDiEnumDeviceInterfaces(devices, nullptr, &BATTERY_INTERFACE_GUID, index, &iface) == FALSE)
        {
            break; // ERROR_NO_MORE_ITEMS
        }
        DWORD needed = 0;
        (void) SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &needed, nullptr);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
        {
            continue;
        }
        // DWORD-aligned storage for the variable-length detail struct
        std::vector<DWORD> buffer((needed + sizeof(DWORD) - 1) / sizeof(DWORD));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, needed, nullptr, nullptr) != FALSE)
        {
            paths.emplace_back(static_cast<const wchar_t*>(detail->DevicePath));
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return paths;
}

BatteryScan scanBatteries(const BatteryDeviceFunctions& api)
{
    BatteryScan scan;
    BatteryInfo& info = scan.info;
    std::vector<BatteryCapacityReading> readings;
    for (const std::wstring& path : api.enumerateBatteries())
    {
        HANDLE opened = api.createFile(path.c_str(),
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
        if (opened == INVALID_HANDLE_VALUE || opened == nullptr)
        {
            continue;
        }
        DeviceHandle device(opened, DeviceCloser{.close = api.closeHandle});

        ULONG wait = 0; // Don't wait for a battery to arrive
        ULONG tag = 0;
        DWORD bytes = 0;
        if (api.deviceIoControl(device.get(), IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &tag, sizeof(tag), &bytes, nullptr) == FALSE ||
            tag == 0)
        {
            continue; // No battery in this slot
        }

        BATTERY_QUERY_INFORMATION query{};
        query.BatteryTag = tag;
        query.InformationLevel = BatteryInformation;
        BATTERY_INFORMATION battery{};
        if (api.deviceIoControl(
                device.get(), IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), &battery, sizeof(battery), &bytes, nullptr) == FALSE)
        {
            continue;
        }

        readings.push_back(BatteryCapacityReading{
            .designed = battery.DesignedCapacity,
            .fullCharged = battery.FullChargedCapacity,
            .capabilities = battery.Capabilities,
            .cycleCount = battery.CycleCount,
        });
        if (!info.found) // The text describes the first battery that answers
        {
            info.found = true;
            info.technology = chemistryText(
                std::string_view(reinterpret_cast<const char*>(static_cast<const UCHAR*>(battery.Chemistry)), sizeof(battery.Chemistry)));
            info.manufacturer = queryName(api, device.get(), tag, BatteryManufactureName);
            info.model = queryName(api, device.get(), tag, BatteryDeviceName);
        }
        // Kept open, with its tag, for the samples' status queries until the next rescan (#4030)
        scan.devices.push_back(BatteryDevice{
            .handle = std::shared_ptr<void>(std::move(device)),
            .tag = tag,
            .capabilities = battery.Capabilities,
        });
    }

    // Every battery's capacities, as the charge percent from GetSystemPowerStatus covers them all (#2151)
    const BatteryCapacityTotals totals = combineBatteryCapacities(readings);
    info.relativeCapacity = totals.relativeCapacity;
    info.designWh = totals.designWh;
    info.fullChargeWh = totals.fullChargeWh;
    info.healthPercent = totals.healthPercent;
    info.cycleCount = totals.cycleCount;
    return scan;
}

BatteryInfo readBatteryInfo(const BatteryDeviceFunctions& api)
{
    return scanBatteries(api).info;
}

BatteryStatusOutcome queryBatteryStatus(const BatteryDeviceFunctions& api, const BatteryDevice& device, BatteryStatusReading& reading)
{
    BATTERY_WAIT_STATUS wait{};
    wait.BatteryTag = device.tag;
    wait.Timeout = 0; // The status now, not when it next changes
    BATTERY_STATUS status{};
    DWORD bytes = 0;
    if (api.deviceIoControl(
            device.handle.get(), IOCTL_BATTERY_QUERY_STATUS, &wait, sizeof(wait), &status, sizeof(status), &bytes, nullptr) == FALSE)
    {
        // A tag that no longer matches the battery in the slot -- taken out, or swapped for another -- fails
        // with one of these; anything else is a passing failure
        const DWORD error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_SUCH_DEVICE) ? BatteryStatusOutcome::Stale
                                                                                : BatteryStatusOutcome::Failed;
    }
    reading = BatteryStatusReading{
        .rate = static_cast<std::int32_t>(status.Rate),
        .capacity = status.Capacity,
        .voltage = status.Voltage,
        .relative = (device.capabilities & BATTERY_CAPACITY_RELATIVE) != 0,
    };
    return BatteryStatusOutcome::Read;
}

} // namespace Platform::Windows
