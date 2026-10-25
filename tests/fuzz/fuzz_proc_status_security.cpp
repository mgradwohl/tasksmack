#include "Platform/Linux/ProcStatusSecurityParser.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The /proc/[pid]/status security parser, the attr/current label and the cgroup parser (#1526) on
// arbitrary text. Nothing may read past the input; a label never holds a NUL or a newline.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    static_cast<void>(Platform::ProcStatusSecurity::parseStatus(text));
    static_cast<void>(Platform::ProcStatusSecurity::parseControlGroup(text));
    const std::string label = Platform::ProcStatusSecurity::parseSecurityLabel(text);
    if (label.contains('\0') || label.contains('\n'))
    {
        __builtin_trap(); // the label stops at the first NUL or newline
    }
    return 0;
}
