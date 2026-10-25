#include "Platform/IProcessModules.h"
#include "Platform/Linux/ProcMapsParser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// The /proc/[pid]/maps parser (#802) on arbitrary text: as one line and as a whole file. Nothing may
// read past the input, and every module it finds must name a file path.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    static_cast<void>(Platform::ProcMaps::parseMapsLine(text));
    const std::vector<Platform::ProcessModule> modules = Platform::ProcMaps::parseProcMaps(text);
    for (const Platform::ProcessModule& module : modules)
    {
        if (module.path.empty() || module.path.front() != '/')
        {
            __builtin_trap(); // only file paths are modules
        }
    }
    return 0;
}
