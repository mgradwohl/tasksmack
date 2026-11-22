#include "Platform/Linux/SystemdUnitFile.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// A systemd unit file as the Startup tab reads it (#4357): packages and users write them, and TaskSmack can run as root
// while it parses one a user wrote. Nothing may read past the input, and the word reader the Install and ExecStart
// lists go through (takeFirstWord, #4398) always moves on, so no list can loop.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    namespace Unit = Platform::SystemdUnitFile;

    const Unit::Unit unit = Unit::parse(text);
    static_cast<void>(Unit::installsIntoSession(unit));
    for (std::string_view words : {std::string_view(unit.execStart), text})
    {
        words = Unit::Detail::trim(words);
        while (!words.empty())
        {
            const std::size_t before = words.size();
            static_cast<void>(Unit::takeFirstWord(words));
            if (words.size() >= before)
            {
                __builtin_trap(); // no progress: a list would loop forever
            }
        }
    }
    return 0;
}
