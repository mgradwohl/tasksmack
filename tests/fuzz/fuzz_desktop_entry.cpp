#include "Platform/Linux/DesktopEntryIcons.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The .desktop entry parser (#2172) on arbitrary text: files under ~/.local/share/applications are
// written by any program the user runs, and parsed on the icon worker of a TaskSmack that may run as
// root. parseDesktopEntry(), then splitExec() and execProgramKeys() on its Exec (and on the whole text
// as an Exec). Nothing may read past the input or throw.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    namespace Icons = Platform::DesktopEntryIcons;
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    if (const std::optional<Icons::DesktopEntry> entry = Icons::parseDesktopEntry(text); entry.has_value())
    {
        static_cast<void>(Icons::splitExec(entry->exec));
        static_cast<void>(Icons::execProgramKeys(entry->exec));
        static_cast<void>(Icons::execProgramKeys(entry->tryExec));
    }
    static_cast<void>(Icons::splitExec(text));
    static_cast<void>(Icons::execProgramKeys(text));
    return 0;
}
