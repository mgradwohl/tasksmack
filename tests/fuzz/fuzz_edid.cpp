#include "Platform/EdidParser.h"

#include <cstddef>
#include <cstdint>
#include <span>

// The EDID parser (#1519) on arbitrary bytes, as Windows reads them from a monitor's registry key and Linux
// from /sys/class/drm/*/edid. Nothing may read past the input, and the descriptor texts stay within their
// 13 bytes.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::span<const std::uint8_t> bytes(data, size);
    if (const auto info = Platform::Edid::parseEdid(bytes))
    {
        if (info->name.size() > 13 || info->serial.size() > 13 || info->manufacturerId.size() != 3)
        {
            __builtin_trap();
        }
        static_cast<void>(info->displayName());
        static_cast<void>(info->serialText());
    }
    static_cast<void>(Platform::Edid::descriptorText(bytes));
    return 0;
}
