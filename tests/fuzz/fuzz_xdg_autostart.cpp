#include "Platform/Linux/XdgAutostart.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// An autostart .desktop file as the Startup tab reads and switches it (#4357): the user's own, in their home, which
// TaskSmack may parse as root. Nothing may read past the input; and switching a parsed entry on or off with
// withEnabled() gives a file that parses again, in that state.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    namespace Autostart = Platform::XdgAutostart;

    const std::optional<Autostart::Entry> entry = Autostart::parse(text);
    if (!entry.has_value())
    {
        return 0;
    }
    static_cast<void>(Autostart::startsIn(*entry, "GNOME:KDE"));
    static_cast<void>(Autostart::splitList(entry->name));
    for (const bool enabled : {false, true})
    {
        const std::optional<std::string> switched = Autostart::withEnabled(text, enabled);
        if (!switched.has_value())
        {
            continue;
        }
        const std::optional<Autostart::Entry> again = Autostart::parse(*switched);
        if (!again.has_value() || again->enabled() != enabled)
        {
            __builtin_trap(); // the switch didn't take, or broke the file
        }
    }
    return 0;
}
