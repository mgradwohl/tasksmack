#include "Platform/Linux/LinuxOsInfo.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// /etc/os-release (its quoting and backslash escapes) and /proc/stat's btime, as System Information reads them
// (#4357). Nothing may read past the input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    namespace OsInfo = Platform::LinuxOsInfo;

    static_cast<void>(OsInfo::parseOsRelease(text));
    static_cast<void>(OsInfo::unquoteOsReleaseValue(text));
    static_cast<void>(OsInfo::parseBootTime(text));
    return 0;
}
