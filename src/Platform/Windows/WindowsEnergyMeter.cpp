#include "WindowsEnergyMeter.h"

#include "WindowsEnergyMeterMath.h"
#include "WindowsHandles.h"

#include <spdlog/spdlog.h>

// clang-format off
#include <winioctl.h>
#include <emi.h>
#include <setupapi.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

// The byte layouts WindowsEnergyMeterMath.h parses are the SDK's.
static_assert(offsetof(EMI_METADATA_V1, MeteredHardwareName) == EMI_METADATA_V1_HEADER_BYTES);
static_assert(offsetof(EMI_METADATA_V2, Channels) == EMI_METADATA_V2_HEADER_BYTES);
static_assert(offsetof(EMI_CHANNEL_V2, ChannelName) == EMI_CHANNEL_V2_HEADER_BYTES);
static_assert(sizeof(EMI_CHANNEL_MEASUREMENT_DATA) == EMI_MEASUREMENT_BYTES);
static_assert(offsetof(EMI_CHANNEL_MEASUREMENT_DATA, AbsoluteEnergy) == 0);
static_assert(static_cast<std::uint32_t>(EmiMeasurementUnitPicowattHours) == EMI_UNIT_PICOWATT_HOURS);
static_assert(EMI_VERSION_V1 == EMI_INTERFACE_V1 && EMI_VERSION_V2 == EMI_INTERFACE_V2);
static_assert(EMI_NAME_MAX == EMI_NAME_CHARS);

// GUID_DEVICE_ENERGY_METER (emi.h), spelled out so no translation unit needs <initguid.h>.
constexpr GUID ENERGY_METER_INTERFACE_GUID{
    .Data1 = 0x45bd8344, .Data2 = 0x7ed6, .Data3 = 0x49cf, .Data4 = {0xa4, 0x40, 0xc2, 0x76, 0xc9, 0x33, 0xb0, 0x53}};
// More energy meters than any machine has; bounds the enumeration.
constexpr DWORD MAX_ENERGY_METERS = 16;

/// One IOCTL with a fixed-size answer; false if the call failed or answered short.
template<typename T> [[nodiscard]] bool query(const EnergyMeterFunctions& api, HANDLE device, DWORD code, T& out)
{
    DWORD bytes = 0;
    return api.deviceIoControl(device, code, nullptr, 0, &out, static_cast<DWORD>(sizeof(T)), &bytes, nullptr) != FALSE &&
           bytes >= sizeof(T);
}

/// The channels a device's metadata names, or nullopt if its version, size or block couldn't be read.
[[nodiscard]] std::optional<std::vector<EmiChannel>> readChannels(const EnergyMeterFunctions& api, HANDLE device, std::uint16_t& version)
{
    EMI_VERSION emiVersion{};
    EMI_METADATA_SIZE size{};
    if (!query(api, device, IOCTL_EMI_GET_VERSION, emiVersion) || !query(api, device, IOCTL_EMI_GET_METADATA_SIZE, size) ||
        size.MetadataSize == 0 || size.MetadataSize > EMI_METADATA_SIZE_CAP_BYTES)
    {
        return std::nullopt;
    }
    std::vector<std::byte> metadata(size.MetadataSize);
    DWORD bytes = 0;
    if (api.deviceIoControl(device, IOCTL_EMI_GET_METADATA, nullptr, 0, metadata.data(), size.MetadataSize, &bytes, nullptr) == FALSE)
    {
        return std::nullopt;
    }
    version = emiVersion.EmiVersion;
    return parseEmiMetadata(version, std::span<const std::byte>(metadata.data(), std::min<std::size_t>(bytes, metadata.size())));
}

} // namespace

std::vector<std::wstring> enumerateEnergyMeterDevicePaths()
{
    std::vector<std::wstring> paths;
    HDEVINFO devices = SetupDiGetClassDevsW(&ENERGY_METER_INTERFACE_GUID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE)
    {
        return paths;
    }
    for (DWORD index = 0; index < MAX_ENERGY_METERS; ++index)
    {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (SetupDiEnumDeviceInterfaces(devices, nullptr, &ENERGY_METER_INTERFACE_GUID, index, &iface) == FALSE)
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

EnergyMeter EnergyMeter::open(const EnergyMeterFunctions& api)
{
    EnergyMeter meter;
    meter.m_Api = api;
    std::size_t metersFound = 0;
    std::string otherChannels; // What a meter without a package channel offers instead, for the log line
    for (const std::wstring& path : api.enumerateMeters())
    {
        HANDLE opened = api.createFile(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (opened == INVALID_HANDLE_VALUE || opened == nullptr)
        {
            continue;
        }
        ++metersFound;
        InjectableHandle device(opened, InjectedHandleCloser{.close = api.closeHandle});
        std::uint16_t version = 0;
        const std::optional<std::vector<EmiChannel>> channels = readChannels(api, device.get(), version);
        if (!channels.has_value())
        {
            continue; // Closed: not an energy meter this reads
        }
        const std::size_t deviceIndex = meter.m_Devices.size();
        std::size_t packagesOnDevice = 0;
        for (std::size_t i = 0; i < channels->size(); ++i)
        {
            const EmiChannel& channel = (*channels)[i];
            if (channel.unit == EMI_UNIT_PICOWATT_HOURS && isPackageChannelName(channel.name))
            {
                meter.m_Packages.push_back(PackageEnergyChannel{.device = deviceIndex, .channel = i, .name = channel.name});
                ++packagesOnDevice;
            }
            else
            {
                otherChannels += otherChannels.empty() ? "" : ", ";
                otherChannels += channel.name;
            }
        }
        if (packagesOnDevice == 0)
        {
            continue; // Closed: a meter for something else (a battery, a display)
        }
        // V1 answers one EMI_CHANNEL_MEASUREMENT_DATA; V2 one per channel the metadata named.
        const std::size_t channelCount = (version == EMI_INTERFACE_V1) ? 1 : channels->size();
        meter.m_Devices.push_back(
            Device{.handle = std::move(device), .measurement = std::vector<std::byte>(channelCount * EMI_MEASUREMENT_BYTES)});
    }

    // Package0 first, then the rest: the first is PackageEnergyReading's own counter (#2444).
    std::ranges::sort(meter.m_Packages, {}, &PackageEnergyChannel::name);

    if (meter.hasPackageChannels())
    {
        std::string names;
        for (const PackageEnergyChannel& package : meter.m_Packages)
        {
            names += names.empty() ? "" : ", ";
            names += package.name;
        }
        spdlog::info("Per-process power available via EMI: {} CPU package channel(s) ({}) on {} energy meter device(s)",
                     meter.m_Packages.size(),
                     names,
                     meter.m_Devices.size());
    }
    else if (metersFound > 0)
    {
        spdlog::info("Per-process power not available: {} EMI energy meter device(s), none with a CPU package channel (channels: {})",
                     metersFound,
                     otherChannels.empty() ? "none readable" : otherChannels);
    }
    else
    {
        spdlog::debug("Per-process power not available (no EMI energy meter device)");
    }
    return meter;
}

bool EnergyMeter::readPackageEnergyUj(std::vector<std::uint64_t>& energyUj)
{
    energyUj.clear();
    if (m_Packages.empty())
    {
        return false;
    }
    for (Device& device : m_Devices)
    {
        DWORD bytes = 0;
        if (m_Api.deviceIoControl(device.handle.get(),
                                  IOCTL_EMI_GET_MEASUREMENT,
                                  nullptr,
                                  0,
                                  device.measurement.data(),
                                  static_cast<DWORD>(device.measurement.size()),
                                  &bytes,
                                  nullptr) == FALSE)
        {
            m_LastReadError = GetLastError();
            return false;
        }
        if (bytes < device.measurement.size())
        {
            m_LastReadError = ERROR_INSUFFICIENT_BUFFER; // Fewer channels answered than its metadata named
            return false;
        }
    }
    energyUj.reserve(m_Packages.size());
    for (const PackageEnergyChannel& package : m_Packages)
    {
        const std::optional<std::uint64_t> picowattHours = emiChannelEnergy(m_Devices[package.device].measurement, package.channel);
        if (!picowattHours.has_value())
        {
            energyUj.clear();
            m_LastReadError = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
        energyUj.push_back(picowattHoursToMicrojoules(*picowattHours));
    }
    return true;
}

} // namespace Platform::Windows
