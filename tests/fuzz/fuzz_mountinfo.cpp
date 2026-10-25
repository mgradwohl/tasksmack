#include "Platform/Linux/LinuxStorage.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// The Storage parsers (#1517) on arbitrary text, as each file they read: /proc/self/mountinfo (then the
// volume choice over what it parsed) and a udev database file. Nothing may read past the input, and the
// volumes chosen are a subset of the entries parsed.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    namespace Storage = Platform::LinuxStorage;

    const std::vector<Storage::MountEntry> entries = Storage::parseMountInfo(text);
    const std::vector<Storage::MountEntry> volumes = Storage::selectVolumes(entries);
    if (volumes.size() > entries.size())
    {
        __builtin_trap(); // the choice only drops entries
    }
    static_cast<void>(Storage::udevProperty(text, "ID_FS_LABEL"));
    static_cast<void>(Storage::udevProperty(text, "ID_SERIAL_SHORT"));
    static_cast<void>(Storage::trimmed(text));
    return 0;
}
