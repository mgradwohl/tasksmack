#include "Platform/IProcessOpenFiles.h"
#include "Platform/Linux/ProcFdParser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// The /proc/[pid]/fd link classifier and the fdinfo flags parser (#183) on arbitrary text. Nothing may
// read past the input, and a classified path is never longer than the link it came from.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    const auto flags = Platform::ProcFd::parseFdInfoFlags(text);
    static_cast<void>(Platform::ProcFd::parseFdName(text));
    const Platform::OpenFile file = Platform::ProcFd::classifyFdLink(3, text, flags);
    if (file.path.size() > text.size())
    {
        __builtin_trap();
    }
    return 0;
}
