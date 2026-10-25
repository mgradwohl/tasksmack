#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxKernelModules.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The /proc/modules parser (#1521) on arbitrary text, as Linux reads it. Every module kept has a name and
// a state, and no dependent is empty or the "[permanent]" marker.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::vector<Platform::KernelDriver> modules = Platform::LinuxKernelModules::parseProcModules(text);
    for (const Platform::KernelDriver& module : modules)
    {
        if (module.name.empty() || module.moduleState.empty())
        {
            __builtin_trap();
        }
        for (const std::string& user : module.usedBy)
        {
            if (user.empty() || user == Platform::LinuxKernelModules::PERMANENT)
            {
                __builtin_trap();
            }
        }
    }
    return 0;
}
