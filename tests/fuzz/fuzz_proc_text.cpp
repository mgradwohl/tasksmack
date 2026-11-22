#include "Platform/Linux/PressureParser.h"
#include "Platform/Linux/ProcIoParser.h"
#include "Platform/Linux/SmapsRollupParser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// Kernel text read for every process and for the system (#4357): /proc/<pid>/smaps_rollup, /proc/<pid>/io and
// /proc/pressure/{cpu,memory,io}. Nothing may read past the input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    static_cast<void>(Platform::SmapsRollup::parse(text));
    static_cast<void>(Platform::PressureParser::parse(text));
    static_cast<void>(Platform::Linux::ProcIoParser::parse(text));
    return 0;
}
