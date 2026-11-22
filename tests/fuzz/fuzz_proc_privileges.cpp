#include "Platform/Linux/ProcPrivileges.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// /proc/<pid>/status's CapEff and Tgid lines, and /proc/self/mountinfo's /proc line, as the privilege checks read them
// (#4616). Another account's process writes none of these, but they're hand-written parsers. Nothing may read past the
// input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    namespace Privileges = Platform::ProcPrivileges;

    static_cast<void>(Privileges::parseCapEff(text));
    static_cast<void>(Privileges::parseTgid(text));
    static_cast<void>(Privileges::parseProcMount(text));
    return 0;
}
