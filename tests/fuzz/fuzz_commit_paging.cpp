#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxCommitPaging.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// The Commit & paging parsers (#1516) on arbitrary text, as each file they read: /proc/meminfo,
// overcommit_memory, /proc/swaps, a zram mm_stat, zswap's enabled flag and the THP selector. Nothing may
// read past the input, and a swap device always has a path.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    namespace Paging = Platform::LinuxCommitPaging;

    static_cast<void>(Paging::meminfoValue(text, "Committed_AS"));
    static_cast<void>(Paging::meminfoValue(text, "HugePages_Total"));
    static_cast<void>(Paging::parseOvercommitMode(text));
    static_cast<void>(Paging::parseZramMmStat(text));
    static_cast<void>(Paging::parseSysfsBool(text));
    static_cast<void>(Paging::parseBracketedChoice(text));
    static_cast<void>(Paging::unescapeSwapPath(text));
    const std::vector<Platform::PageFile> swaps = Paging::parseSwaps(text);
    for (const Platform::PageFile& swap : swaps)
    {
        if (swap.path.empty())
        {
            __builtin_trap(); // a line without a path isn't a swap device
        }
    }
    return 0;
}
