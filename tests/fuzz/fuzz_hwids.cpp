#include "Platform/HwIdsParser.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

// The pci.ids / usb.ids parser (#1520) on arbitrary text, as Linux reads it from hwdata. Every name kept
// must have been asked for, and no name may be empty.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::set<std::uint16_t> vendors{0x8086, 0x10DE, 0x046D};
    const std::set<std::uint32_t> devices{Platform::HwIds::deviceKey(0x8086, 0x46A6), Platform::HwIds::deviceKey(0x046D, 0xC52B)};
    const Platform::HwIds::IdNames names = Platform::HwIds::parseIds(text, vendors, devices);
    for (const auto& [id, name] : names.vendors)
    {
        if (!vendors.contains(id) || name.empty())
        {
            __builtin_trap();
        }
    }
    for (const auto& [key, name] : names.devices)
    {
        if (!devices.contains(key) || name.empty())
        {
            __builtin_trap();
        }
    }
    static_cast<void>(names.className(0x03, 0x00));
    return 0;
}
