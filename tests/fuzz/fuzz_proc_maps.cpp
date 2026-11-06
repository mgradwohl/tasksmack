#include "Platform/IProcessMemoryMap.h"
#include "Platform/IProcessModules.h"
#include "Platform/Linux/ProcMapsParser.h"
#include "Platform/Linux/SmapsParser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// The /proc/[pid]/maps parser (#802) on arbitrary text: as one line and as a whole file. Nothing may
// read past the input, and every module it finds must name a file path. The same text as smaps (#3089),
// whose entries start with a maps line: every Image or File mapping names a file path, and none ends before
// it starts.
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
    for (const Platform::MemoryMapping& mapping : Platform::Smaps::parse(text))
    {
        const bool file = mapping.kind == Platform::MemoryMappingKind::Image || mapping.kind == Platform::MemoryMappingKind::File;
        if ((file && (mapping.path.empty() || mapping.path.front() != '/')) || mapping.end < mapping.start)
        {
            __builtin_trap();
        }
    }
    return 0;
}
