#include "WindowsSystemInfoProbe.h"

#include "Platform/ISystemInfoProbe.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <windows.h>
#include <lm.h>
#include <security.h>
#include <powerbase.h> // PowerDeterminePlatformRoleEx (powrprof)
#include <setupapi.h>
#include <devpropdef.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <winsvc.h>
#include <winver.h>
// clang-format on

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "version.lib")

#include "ComPtr.h"
#include "ComScope.h"
#include "DXGIAdapterLocation.h"
#include "Platform/SmbiosParser.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"
#include "WinString.h"
#include "WindowsCommitPaging.h"
#include "WindowsCrashEvents.h"
#include "WindowsDevices.h"
#include "WindowsDrivers.h"
#include "WindowsGraphics.h"
#include "WindowsHandles.h"
#include "WindowsInstalledMemory.h"
#include "WindowsOsInfoMath.h"
#include "WindowsServiceConfig.h"
#include "WindowsStorage.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{

namespace
{

constexpr const wchar_t* CURRENT_VERSION_KEY = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

/// A REG_SZ value of the CurrentVersion key (64-bit view), UTF-8; empty when absent.
[[nodiscard]] std::string readVersionString(const wchar_t* name)
{
    DWORD bytes = 0;
    constexpr DWORD FLAGS = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, CURRENT_VERSION_KEY, name, FLAGS, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
    {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, CURRENT_VERSION_KEY, name, FLAGS, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
    {
        return {};
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return WinString::wideToUtf8(value);
}

/// A REG_DWORD value of the CurrentVersion key; nullopt when absent.
[[nodiscard]] std::optional<std::uint32_t> readVersionDword(const wchar_t* name) noexcept
{
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, CURRENT_VERSION_KEY, name, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &value, &bytes) !=
        ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    return value;
}

/// GetComputerNameExW / GetUserNameExW: on success `size` is the length; on ERROR_MORE_DATA it is the
/// size needed (terminator included), and the call is retried once with that much room.
template<typename Fill> [[nodiscard]] std::string readSizedString(Fill fill)
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        auto size = static_cast<ULONG>(buffer.size());
        if (fill(buffer.data(), size) != FALSE)
        {
            buffer.resize(size);
            return WinString::wideToUtf8(buffer);
        }
        if (GetLastError() != ERROR_MORE_DATA || size <= buffer.size())
        {
            return {};
        }
        buffer.resize(size);
    }
    return {};
}

/// GetSystemDirectoryW / GetSystemWindowsDirectoryW: the length on success, 0 on failure.
template<typename Get> [[nodiscard]] std::string readDirectory(Get get)
{
    std::wstring buffer(MAX_PATH, L'\0');
    UINT length = get(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length > buffer.size())
    {
        buffer.resize(length);
        length = get(buffer.data(), static_cast<UINT>(buffer.size()));
    }
    if (length == 0 || length > buffer.size())
    {
        return {};
    }
    buffer.resize(length);
    return WinString::wideToUtf8(buffer);
}

void readJoinInformation(OsInfo& info)
{
    LPWSTR name = nullptr;
    NETSETUP_JOIN_STATUS status = NetSetupUnknownStatus;
    if (NetGetJoinInformation(nullptr, &name, &status) != NERR_Success)
    {
        return;
    }
    if (name != nullptr && (status == NetSetupDomainName || status == NetSetupWorkgroupName))
    {
        info.domainOrWorkgroup = WinString::wideToUtf8(name);
        info.joinedToDomain = status == NetSetupDomainName;
    }
    NetApiBufferFree(name);
}

void readTimeZone(OsInfo& info)
{
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    const DWORD id = GetDynamicTimeZoneInformation(&zone);
    if (id == TIME_ZONE_ID_INVALID)
    {
        return;
    }
    // Bias is minutes *west* of UTC; the daylight or standard bias adds to it while in effect.
    LONG bias = zone.Bias;
    if (id == TIME_ZONE_ID_DAYLIGHT)
    {
        bias += zone.DaylightBias;
    }
    else if (id == TIME_ZONE_ID_STANDARD)
    {
        bias += zone.StandardBias;
    }
    info.utcOffsetMinutes = static_cast<int>(-bias);
    info.timeZone = WinString::wideToUtf8(zone.TimeZoneKeyName[0] != L'\0' ? zone.TimeZoneKeyName : zone.StandardName);
}

/// The raw SMBIOS table (a RawSMBIOSData blob) from GetSystemFirmwareTable('RSMB'); no administrator
/// rights needed. Empty on failure. The size can change between the two calls (it doesn't in
/// practice), so a second call that wants more room gives nothing.
[[nodiscard]] std::vector<std::uint8_t> readRawSmbios()
{
    constexpr DWORD RSMB = 0x52534D42;
    const UINT size = GetSystemFirmwareTable(RSMB, 0, nullptr, 0);
    if (size == 0)
    {
        return {};
    }
    std::vector<std::uint8_t> table(size);
    const UINT written = GetSystemFirmwareTable(RSMB, 0, table.data(), size);
    if (written == 0 || written > size)
    {
        return {};
    }
    table.resize(written);
    return table;
}

// DEVPKEY_Device_DriverDate and _DriverVersion (devpkey.h), GUID_DEVCLASS_DISPLAY (devguid.h) and
// GUID_DEVINTERFACE_MONITOR (ntddvdeo.h), spelled out so no translation unit needs <initguid.h>.
constexpr GUID DRIVER_PROPERTY_GUID{
    .Data1 = 0xa8b865dd,
    .Data2 = 0x2e3d,
    .Data3 = 0x4094,
    .Data4 = {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6},
};
constexpr DEVPROPKEY DRIVER_DATE_KEY{.fmtid = DRIVER_PROPERTY_GUID, .pid = 2};
constexpr DEVPROPKEY DRIVER_VERSION_KEY{.fmtid = DRIVER_PROPERTY_GUID, .pid = 3};
constexpr GUID DISPLAY_CLASS_GUID{
    .Data1 = 0x4d36e968,
    .Data2 = 0xe325,
    .Data3 = 0x11ce,
    .Data4 = {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18},
};
constexpr GUID MONITOR_INTERFACE_GUID{
    .Data1 = 0xe6f07b5f,
    .Data2 = 0xee97,
    .Data3 = 0x4a90,
    .Data4 = {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7},
};

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) - COM out-parameters and SetupAPI byte buffers
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token" // __uuidof

/// Every DXGI adapter with its outputs; nullopt when no DXGI factory can be made. No device is created.
[[nodiscard]] std::optional<std::vector<WindowsGraphics::AdapterRecord>> listDxgiAdapters()
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.releaseAndGetAddressOf()))) || !factory)
    {
        return std::nullopt;
    }
    std::vector<WindowsGraphics::AdapterRecord> records;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; SUCCEEDED(factory->EnumAdapters1(index, adapter.releaseAndGetAddressOf())) && adapter; ++index)
    {
        WindowsGraphics::AdapterRecord record;
        if (FAILED(adapter->GetDesc1(&record.desc)))
        {
            continue;
        }
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; SUCCEEDED(adapter->EnumOutputs(o, output.releaseAndGetAddressOf())) && output; ++o)
        {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)))
            {
                continue;
            }
            WindowsGraphics::OutputRecord out;
            out.deviceName = static_cast<const wchar_t*>(desc.DeviceName);
            out.desktop = desc.DesktopCoordinates;
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc1{};
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(output6.releaseAndGetAddressOf()))) &&
                output6 && SUCCEEDED(output6->GetDesc1(&desc1)))
            {
                out.hasDesc1 = true;
                out.colorSpace = desc1.ColorSpace;
                out.bitsPerColor = desc1.BitsPerColor;
            }
            record.outputs.push_back(std::move(out));
        }
        records.push_back(std::move(record));
    }
    return records;
}

/// Every present display-class device node's hardware id, PCI location and driver version and date.
[[nodiscard]] std::vector<WindowsGraphics::DriverNode> listDisplayDriverNodes()
{
    std::vector<WindowsGraphics::DriverNode> nodes;
    HDEVINFO devices = SetupDiGetClassDevsW(&DISPLAY_CLASS_GUID, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE)
    {
        return nodes;
    }
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &device) != FALSE; ++index)
    {
        WindowsGraphics::DriverNode node;
        std::array<wchar_t, 512> text{}; // the last element stays 0
        constexpr auto TEXT_BYTES = static_cast<DWORD>((512 - 1) * sizeof(wchar_t));
        if (SetupDiGetDeviceRegistryPropertyW(
                devices, &device, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(text.data()), TEXT_BYTES, nullptr) != FALSE)
        {
            node.hardwareId = WinString::wideToUtf8(text.data()); // a REG_MULTI_SZ's first string
        }
        DWORD value = 0;
        if (SetupDiGetDeviceRegistryPropertyW(
                devices, &device, SPDRP_BUSNUMBER, nullptr, reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr) != FALSE)
        {
            node.bus = value;
        }
        if (SetupDiGetDeviceRegistryPropertyW(
                devices, &device, SPDRP_ADDRESS, nullptr, reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr) != FALSE)
        {
            node.address = value;
        }
        DEVPROPTYPE type = 0;
        text.fill(L'\0');
        if (SetupDiGetDevicePropertyW(
                devices, &device, &DRIVER_VERSION_KEY, &type, reinterpret_cast<PBYTE>(text.data()), TEXT_BYTES, nullptr, 0) != FALSE &&
            type == DEVPROP_TYPE_STRING)
        {
            node.driverVersion = WinString::wideToUtf8(text.data());
        }
        FILETIME date{};
        if (SetupDiGetDevicePropertyW(
                devices, &device, &DRIVER_DATE_KEY, &type, reinterpret_cast<PBYTE>(&date), sizeof(date), nullptr, 0) != FALSE &&
            type == DEVPROP_TYPE_FILETIME)
        {
            node.driverDate = date;
        }
        nodes.push_back(std::move(node));
    }
    SetupDiDestroyDeviceInfoList(devices);
    return nodes;
}

/// The EDID of the monitor on DXGI output @p outputDeviceName ("\\.\DISPLAY1"): the monitor's device
/// interface from EnumDisplayDevicesW, then the EDID value of its device registry key. Empty when any
/// step fails.
[[nodiscard]] std::vector<std::uint8_t> readMonitorEdid(const std::wstring& outputDeviceName)
{
    DISPLAY_DEVICEW display{};
    display.cb = sizeof(display);
    if (EnumDisplayDevicesW(outputDeviceName.c_str(), 0, &display, EDD_GET_DEVICE_INTERFACE_NAME) == FALSE)
    {
        return {};
    }
    HDEVINFO devices = SetupDiGetClassDevsW(&MONITOR_INTERFACE_GUID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE)
    {
        return {};
    }
    std::vector<std::uint8_t> edid;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (SetupDiOpenDeviceInterfaceW(devices, static_cast<const wchar_t*>(display.DeviceID), 0, &iface) != FALSE &&
        (SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, nullptr, &device) != FALSE ||
         GetLastError() == ERROR_INSUFFICIENT_BUFFER))
    {
        HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE)
        {
            constexpr DWORD MAX_EDID_BYTES = 32768;
            DWORD size = 0;
            if (RegQueryValueExW(key, L"EDID", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size > 0 && size <= MAX_EDID_BYTES)
            {
                edid.resize(size);
                if (RegQueryValueExW(key, L"EDID", nullptr, nullptr, edid.data(), &size) == ERROR_SUCCESS)
                {
                    edid.resize(size);
                }
                else
                {
                    edid.clear();
                }
            }
            RegCloseKey(key);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return edid;
}

/// A device's string registry property (a REG_MULTI_SZ's first string), UTF-8; empty when absent.
[[nodiscard]] std::string deviceRegistryString(HDEVINFO devices, SP_DEVINFO_DATA& device, DWORD property)
{
    std::array<wchar_t, 512> text{}; // the last element stays 0
    constexpr auto TEXT_BYTES = static_cast<DWORD>((512 - 1) * sizeof(wchar_t));
    if (SetupDiGetDeviceRegistryPropertyW(devices, &device, property, nullptr, reinterpret_cast<PBYTE>(text.data()), TEXT_BYTES, nullptr) ==
        FALSE)
    {
        return {};
    }
    return WinString::wideToUtf8(text.data());
}

/// A device node's instance id, UTF-8; empty when it can't be read.
[[nodiscard]] std::string devNodeId(DEVINST node)
{
    std::array<wchar_t, MAX_DEVICE_ID_LEN + 1> id{};
    return CM_Get_Device_IDW(node, id.data(), MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS ? WinString::wideToUtf8(id.data()) : std::string{};
}

/// Every present device node (all classes) with its status. Reads only: no node is changed.
[[nodiscard]] std::optional<std::vector<WindowsDevices::DevNodeRecord>> listDevNodes()
{
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE)
    {
        return std::nullopt;
    }
    std::vector<WindowsDevices::DevNodeRecord> nodes;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &device) != FALSE; ++index)
    {
        WindowsDevices::DevNodeRecord node;
        node.instanceId = devNodeId(device.DevInst);
        if (DEVINST parent = 0; CM_Get_Parent(&parent, device.DevInst, 0) == CR_SUCCESS)
        {
            node.parentId = devNodeId(parent);
        }
        node.name = deviceRegistryString(devices, device, SPDRP_FRIENDLYNAME);
        node.name = node.name.empty() ? deviceRegistryString(devices, device, SPDRP_DEVICEDESC) : node.name;
        node.manufacturer = deviceRegistryString(devices, device, SPDRP_MFG);
        node.service = deviceRegistryString(devices, device, SPDRP_SERVICE);
        std::array<wchar_t, 256> className{};
        if (SetupDiGetClassDescriptionW(&device.ClassGuid, className.data(), static_cast<DWORD>(className.size() - 1), nullptr) != FALSE)
        {
            node.className = WinString::wideToUtf8(className.data());
        }
        DWORD value = 0;
        if (SetupDiGetDeviceRegistryPropertyW(
                devices, &device, SPDRP_BUSNUMBER, nullptr, reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr) != FALSE)
        {
            node.bus = value;
        }
        if (SetupDiGetDeviceRegistryPropertyW(
                devices, &device, SPDRP_ADDRESS, nullptr, reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr) != FALSE)
        {
            node.address = value;
        }
        node.statusRead = CM_Get_DevNode_Status(&node.status, &node.problem, device.DevInst, 0) == CR_SUCCESS;
        nodes.push_back(std::move(node));
    }
    SetupDiDestroyDeviceInfoList(devices);
    return nodes;
}

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), spelled out so no <initguid.h> is needed.
constexpr PROPERTYKEY FRIENDLY_NAME_KEY{
    .fmtid = {.Data1 = 0xa45c254e, .Data2 = 0xdf1c, .Data3 = 0x4efd, .Data4 = {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    .pid = 14,
};

/// The active audio endpoints, render and capture, from the MMDevice API; nullopt when it can't be used.
/// COM is initialised (multithreaded) for the call, on the reading thread.
[[nodiscard]] std::optional<std::vector<WindowsDevices::AudioRecord>> listAudioEndpoints()
{
    const ComScope com(COINIT_MULTITHREADED);
    if (!com.usable())
    {
        return std::nullopt;
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> collection;
    UINT count = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.releaseAndGetAddressOf()))) ||
        !enumerator || FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, collection.releaseAndGetAddressOf())) ||
        !collection || FAILED(collection->GetCount(&count)))
    {
        return std::nullopt;
    }
    std::vector<WindowsDevices::AudioRecord> endpoints;
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        ComPtr<IMMEndpoint> endpoint;
        EDataFlow flow = eRender;
        if (FAILED(collection->Item(i, device.releaseAndGetAddressOf())) || !device ||
            FAILED(device->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(endpoint.releaseAndGetAddressOf()))) ||
            !endpoint || FAILED(endpoint->GetDataFlow(&flow)))
        {
            continue;
        }
        WindowsDevices::AudioRecord record{.name = {}, .capture = flow == eCapture};
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.releaseAndGetAddressOf())) && store)
        {
            PROPVARIANT name;
            PropVariantInit(&name);
            // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - PROPVARIANT is a tagged union; vt is checked first
            if (SUCCEEDED(store->GetValue(FRIENDLY_NAME_KEY, &name)) && name.vt == VT_LPWSTR && name.pwszVal != nullptr)
            {
                record.name = WinString::wideToUtf8(name.pwszVal);
            }
            // NOLINTEND(cppcoreguidelines-pro-type-union-access)
            PropVariantClear(&name);
        }
        endpoints.push_back(std::move(record));
    }
    return endpoints;
}

/// The running kernel and file system driver services, with each one's start type and image path from
/// its configuration (the Services tab's reader). Reads only: no driver is started, stopped or changed.
[[nodiscard]] std::optional<std::vector<WindowsDrivers::DriverServiceRecord>> listDriverServices()
{
    const Windows::UniqueServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
    if (!scm)
    {
        return std::nullopt;
    }
    const Windows::ServiceConfigFunctions api;
    std::vector<WindowsDrivers::DriverServiceRecord> records;
    std::vector<ENUM_SERVICE_STATUS_PROCESSW> buffer;
    DWORD resume = 0;
    for (;;)
    {
        DWORD needed = 0;
        DWORD count = 0;
        const BOOL ok = EnumServicesStatusExW(scm.get(),
                                              SC_ENUM_PROCESS_INFO,
                                              SERVICE_DRIVER,
                                              SERVICE_ACTIVE,
                                              reinterpret_cast<LPBYTE>(buffer.data()),
                                              static_cast<DWORD>(buffer.size() * sizeof(ENUM_SERVICE_STATUS_PROCESSW)),
                                              &needed,
                                              &count,
                                              &resume,
                                              nullptr);
        const DWORD error = ok != FALSE ? ERROR_SUCCESS : GetLastError();
        if (error != ERROR_SUCCESS && error != ERROR_MORE_DATA)
        {
            return std::nullopt;
        }
        for (const ENUM_SERVICE_STATUS_PROCESSW& entry : std::span(buffer.data(), count))
        {
            const Windows::ServiceConfig config = Windows::readServiceConfig(api, scm.get(), entry.lpServiceName);
            records.push_back({
                .name = entry.lpServiceName != nullptr ? WinString::wideToUtf8(entry.lpServiceName) : std::string{},
                .displayName = entry.lpDisplayName != nullptr ? WinString::wideToUtf8(entry.lpDisplayName) : std::string{},
                .serviceType = entry.ServiceStatusProcess.dwServiceType,
                .currentState = entry.ServiceStatusProcess.dwCurrentState,
                .startType = config.startType,
                .binaryPath = config.binaryPath,
            });
        }
        if (error == ERROR_SUCCESS || needed == 0)
        {
            break;
        }
        buffer.resize((needed + sizeof(ENUM_SERVICE_STATUS_PROCESSW) - 1) / sizeof(ENUM_SERVICE_STATUS_PROCESSW));
    }
    return records;
}

[[nodiscard]] std::string windowsDirectory()
{
    return readDirectory([](wchar_t* buffer, UINT size) { return GetSystemWindowsDirectoryW(buffer, size); });
}

/// A file's version resource: its fixed file version and its first translation's CompanyName.
[[nodiscard]] WindowsDrivers::FileVersionRecord readFileVersion(const std::string& path)
{
    WindowsDrivers::FileVersionRecord record;
    const std::wstring wide = WinString::utf8ToWide(path);
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, wide.c_str(), &ignored);
    if (size == 0)
    {
        return record;
    }
    std::vector<std::byte> data(size);
    if (GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, wide.c_str(), 0, size, data.data()) == FALSE)
    {
        return record;
    }
    void* value = nullptr;
    UINT length = 0;
    if (VerQueryValueW(data.data(), L"\\", &value, &length) != FALSE && length >= sizeof(VS_FIXEDFILEINFO))
    {
        const auto* fixed = static_cast<const VS_FIXEDFILEINFO*>(value);
        record.version = WindowsDrivers::formatFileVersion(fixed->dwFileVersionMS, fixed->dwFileVersionLS);
    }
    struct Translation
    {
        WORD language;
        WORD codePage;
    };
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &value, &length) != FALSE && length >= sizeof(Translation))
    {
        const auto* translation = static_cast<const Translation*>(value);
        const std::wstring key = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\CompanyName", translation->language, translation->codePage);
        if (VerQueryValueW(data.data(), key.c_str(), &value, &length) != FALSE && length > 0)
        {
            const auto* company = static_cast<const wchar_t*>(value); // length is in characters, with or without the NUL
            record.company = WinString::wideToUtf8(std::wstring_view(company, wcsnlen(company, length)));
        }
    }
    return record;
}

#pragma clang diagnostic pop
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace

SystemInfoCapabilities WindowsSystemInfoProbe::capabilities() const
{
    return {.hasOs = true, .unavailableReason = {}};
}

OsInfo WindowsSystemInfoProbe::readOs()
{
    OsInfo info;
    info.family = OsFamily::Windows;

    const std::string currentBuild = readVersionString(L"CurrentBuild");
    const std::optional<std::uint32_t> build = WindowsOsInfo::parseBuild(currentBuild);
    info.name = WindowsOsInfo::productName(readVersionString(L"ProductName"), readVersionString(L"EditionID"), build);
    info.version = WindowsOsInfo::displayVersion(readVersionString(L"DisplayVersion"), readVersionString(L"ReleaseId"));
    info.build = WindowsOsInfo::buildString(currentBuild, readVersionDword(L"UBR"));
    info.installUnixSeconds = readVersionDword(L"InstallDate").value_or(0); // Unix seconds

    USHORT processMachine = 0;
    USHORT nativeMachine = 0;
    if (IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine) != FALSE)
    {
        info.architecture = std::string(WindowsOsInfo::machineName(nativeMachine));
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto upSeconds = static_cast<std::int64_t>(GetTickCount64() / 1000);
    info.bootUnixSeconds = now > upSeconds ? static_cast<std::uint64_t>(now - upSeconds) : 0;

    info.computerName =
        readSizedString([](wchar_t* buffer, ULONG& size) { return GetComputerNameExW(ComputerNameDnsHostname, buffer, &size); });
    readJoinInformation(info);
    info.userName = readSizedString([](wchar_t* buffer, ULONG& size) { return GetUserNameExW(NameSamCompatible, buffer, &size); });

    std::wstring locale(LOCALE_NAME_MAX_LENGTH, L'\0');
    if (const int length = GetUserDefaultLocaleName(locale.data(), LOCALE_NAME_MAX_LENGTH); length > 1)
    {
        locale.resize(static_cast<std::size_t>(length) - 1);
        info.locale = WinString::wideToUtf8(locale);
    }
    readTimeZone(info);

    info.systemDirectory = readDirectory([](wchar_t* buffer, UINT size) { return GetSystemDirectoryW(buffer, size); });
    info.windowsDirectory = readDirectory([](wchar_t* buffer, UINT size) { return GetSystemWindowsDirectoryW(buffer, size); });
    return info;
}

FirmwareInfo WindowsSystemInfoProbe::readFirmware()
{
    FirmwareInfo info;
    info.available = true;

    FIRMWARE_TYPE type = FirmwareTypeUnknown;
    if (GetFirmwareType(&type) != FALSE)
    {
        if (type == FirmwareTypeUefi)
        {
            info.firmwareMode = FirmwareMode::Uefi;
        }
        else if (type == FirmwareTypeBios)
        {
            info.firmwareMode = FirmwareMode::Legacy;
        }
    }

    if (const std::vector<std::uint8_t> table = readRawSmbios(); !table.empty())
    {
        Smbios::decodeFirmware(table, info);
    }

    // The power manager's role (from the ACPI FADT preferred profile) is what msinfo32 shows; the
    // chassis-derived role from the SMBIOS table stands in when it is unspecified.
    if (const std::string_view role =
            WindowsOsInfo::platformRoleName(static_cast<std::uint32_t>(PowerDeterminePlatformRoleEx(POWER_PLATFORM_ROLE_V2)));
        !role.empty())
    {
        info.platformRole = std::string(role);
    }
    return info;
}

MemoryModulesInfo WindowsSystemInfoProbe::readMemoryModules()
{
    MemoryModulesInfo info;
    info.available = true;
    if (const std::vector<std::uint8_t> table = readRawSmbios(); !table.empty())
    {
        Smbios::decodeMemoryModules(table, info);
    }
    readInstalledMemory(info);
    return info;
}

CommitPagingInfo WindowsSystemInfoProbe::readCommitPaging()
{
    CommitPagingInfo info;
    WindowsCommitPaging::readCommitPaging(info);
    return info;
}

StorageInfo WindowsSystemInfoProbe::readStorage()
{
    StorageInfo info;
    WindowsStorage::readStorage(info);
    return info;
}

GraphicsInfo WindowsSystemInfoProbe::readGraphics()
{
    const WindowsGraphics::Functions fns{
        .listAdapters = &listDxgiAdapters,
        .adapterType = [](const LUID& luid) -> std::optional<AdapterTypeBits>
        {
            const auto kind = adapterKind(luid);
            return kind.has_value() ? std::optional(adapterTypeBits(*kind)) : std::nullopt;
        },
        .pciLocation = [](const LUID& luid) { return adapterPciLocation(luid); },
        .listDisplayDrivers = &listDisplayDriverNodes,
        .readMonitorEdid = &readMonitorEdid,
    };
    GraphicsInfo info;
    WindowsGraphics::readGraphics(info, fns);
    return info;
}

DevicesInfo WindowsSystemInfoProbe::readDevices()
{
    DevicesInfo info;
    WindowsDevices::readDevices(info, {.listDevNodes = &listDevNodes, .listAudioEndpoints = &listAudioEndpoints});
    return info;
}

DriversInfo WindowsSystemInfoProbe::readDrivers()
{
    DriversInfo info;
    WindowsDrivers::readDrivers(
        info, {.listDriverServices = &listDriverServices, .windowsDirectory = &windowsDirectory, .readFileVersion = &readFileVersion});
    return info;
}

CrashesInfo WindowsSystemInfoProbe::readCrashes()
{
    CrashesInfo info;
    WindowsCrashEvents::readCrashes(info, WindowsCrashEvents::systemFunctions());
    return info;
}

} // namespace Platform
