#include "Platform/Windows/WindowsVersionResourceMath.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// The version resource walk (#4057) on arbitrary bytes, as the Publisher column and the Modules section read a file
// they hold: the PE headers, the section table and the resource directory's three levels down to the VS_VERSIONINFO
// block, then its VS_FIXEDFILEINFO. Whoever wrote the executable chose every byte. The block found must lie inside
// the input, be no longer than its 16-bit wLength allows, and be a tree VerQueryValueW can walk. The input is also
// read as a bare block, so the node walk is fuzzed without a PE file around it (#4358).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    namespace Version = Platform::VersionResourceMath;
    const std::span<const std::byte> file = std::as_bytes(std::span<const std::uint8_t>(data, size));
    if (const std::optional<std::span<const std::byte>> block = Version::findVersionResource(file))
    {
        if (block->data() < file.data() || block->data() + block->size() > file.data() + file.size() || block->size() > 0xFFFFU ||
            block->size() < Version::VERSION_INFO_MIN || !Version::Detail::isVersionTree(*block))
        {
            __builtin_trap();
        }
        static_cast<void>(Version::fixedFileVersion(*block));
    }
    static_cast<void>(Version::Detail::isVersionTree(file));
    static_cast<void>(Version::fixedFileVersion(file));
    return 0;
}
