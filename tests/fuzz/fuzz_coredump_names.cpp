#include "Platform/Linux/LinuxCoredumps.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// The systemd-coredump core file name parser (#1524) on arbitrary text, as Linux lists the directory.
// Every name kept has a comm, a 32-digit boot id and no '.' left in its compression.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::optional<Platform::LinuxCoredumps::CoredumpName> name = Platform::LinuxCoredumps::parseCoredumpName(text);
    constexpr std::size_t BOOT_ID_DIGITS = 32;
    if (name.has_value() && (name->comm.empty() || name->bootId.size() != BOOT_ID_DIGITS || name->compression.contains('.')))
    {
        __builtin_trap();
    }
    return 0;
}
