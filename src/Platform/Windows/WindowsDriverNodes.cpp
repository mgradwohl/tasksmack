#include "WindowsDriverNodes.h"

#include "WinString.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace Platform::WindowsGraphics
{

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) - SetupAPI byte buffers
std::vector<DriverNode> listDriverNodes(const GUID* classGuid, const wchar_t* enumerator, DWORD flags, const SetupApiFunctions& api)
{
    std::vector<DriverNode> nodes;
    HDEVINFO raw = api.getClassDevs(classGuid, enumerator, nullptr, flags);
    if (raw == INVALID_HANDLE_VALUE)
    {
        return nodes;
    }
    // HDEVINFO is a pointer, and SetupDiGetClassDevsW never returns null on success, so the list is
    // destroyed exactly once, however this returns.
    const std::unique_ptr<void, SetupApiFunctions::DestroyDeviceInfoListFn> devices(raw, api.destroyDeviceInfoList);
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; api.enumDeviceInfo(devices.get(), index, &device) != FALSE; ++index)
    {
        DriverNode node;
        std::array<wchar_t, DRIVER_NODE_TEXT_CHARS> text{}; // the last element stays 0
        constexpr auto TEXT_BYTES = static_cast<DWORD>((DRIVER_NODE_TEXT_CHARS - 1) * sizeof(wchar_t));
        if (api.getRegistryProperty(
                devices.get(), &device, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(text.data()), TEXT_BYTES, nullptr) != FALSE)
        {
            node.hardwareId = WinString::wideToUtf8(text.data()); // a REG_MULTI_SZ's first string
        }
        DWORD value = 0;
        if (api.getRegistryProperty(
                devices.get(), &device, SPDRP_BUSNUMBER, nullptr, reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr) != FALSE)
        {
            node.bus = value;
        }
        if (api.getRegistryProperty(
                devices.get(), &device, SPDRP_ADDRESS, nullptr, reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr) != FALSE)
        {
            node.address = value;
        }
        DEVPROPTYPE type = 0;
        text.fill(L'\0');
        if (api.getProperty(
                devices.get(), &device, &DRIVER_VERSION_KEY, &type, reinterpret_cast<PBYTE>(text.data()), TEXT_BYTES, nullptr, 0) !=
                FALSE &&
            type == DEVPROP_TYPE_STRING)
        {
            node.driverVersion = WinString::wideToUtf8(text.data());
        }
        FILETIME date{};
        if (api.getProperty(devices.get(), &device, &DRIVER_DATE_KEY, &type, reinterpret_cast<PBYTE>(&date), sizeof(date), nullptr, 0) !=
                FALSE &&
            type == DEVPROP_TYPE_FILETIME)
        {
            node.driverDate = date;
        }
        nodes.push_back(std::move(node));
    }
    return nodes;
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace Platform::WindowsGraphics
