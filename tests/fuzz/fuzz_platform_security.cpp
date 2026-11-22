#include "Platform/Linux/LinuxPlatformSecurity.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// The platform-security values System Information reads (#4616): the SecureBoot efivar and its legacy data, the LSM
// list, SELinux's enforce file and a partition type's UUID. Nothing may read past the input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    namespace Security = Platform::LinuxPlatformSecurity;

    static_cast<void>(Security::parseSecureBootVariable(text));
    static_cast<void>(Security::parseLegacySecureBootData(text));
    static_cast<void>(Security::parseLsmList(text));
    static_cast<void>(Security::parseSelinuxEnforce(text));
    static_cast<void>(Security::cryptType(text));
    return 0;
}
